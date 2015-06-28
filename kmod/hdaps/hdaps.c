/*
 * drivers/hwmon/hdaps.c - driver for IBM's Hard Drive Active Protection System
 *
 * Copyright (C) 2005 Robert Love <rml@novell.com>
 * Copyright (C) 2005 Jesper Juhl <jesper.juhl@gmail.com>
 *
 * The HardDisk Active Protection System (hdaps) is present in IBM ThinkPads
 * starting with the R40, T41, and X40.  It provides a basic two-axis
 * accelerometer and other data, such as the device's temperature.
 *
 * This driver is based on the document by Mark A. Smith available at
 * http://www.almaden.ibm.com/cs/people/marksmith/tpaps.html and a lot of trial
 * and error.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License v2 as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 *
 * Ported to FreeBSD by Maik Ehinger <m.ehinger@ltur.de>
 *
 *	Differences between FreeBSD and Linux Version
 *		- use sysctl's (hw.hdaps) instead of sysfs
 *		- added led state
 *		- provide extra sysctl (hw.hdaps.values)
 *			returns position, temperature, mouse and 
 *			keyboard activity with one sysctl read
 *		- mouse/joystick device needs still some work. ideas?
 *			(/dev/hdaps and /dev/joy0)
 */

#include <sys/types.h>

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/module.h>
#include <sys/systm.h>

#include <sys/sysctl.h>

#include <sys/conf.h>

#include "../thinkpad_ec.h"
#include "../smbios.h"
#include "hdaps.h"
#include "hdaps_dev.h"
//#include "hdaps_mousedev.h"
#include "hdaps_joydev.h"

#define DEVICE_NAME	"hdaps"

/* Embedded controller accelerometer read command and its result: */
static const struct thinkpad_ec_row ec_accel_args =
	{ .mask=0x0001, .val={0x11} };

#define EC_ACCEL_IDX_READOUTS	0x1	/* readouts included in this read */
					/* First readout, if READOUTS>=1: */
#define EC_ACCEL_IDX_YPOS1	0x2	/*   y-axis position word */
#define EC_ACCEL_IDX_XPOS1	0x4	/*   x-axis position word */
#define EC_ACCEL_IDX_TEMP1	0x6	/*   device temperature in Celsius */
					/* Second readout, if READOUTS>=2: */
#define EC_ACCEL_IDX_XPOS2	0x7	/*   y-axis position word */
#define EC_ACCEL_IDX_YPOS2	0x9	/*   x-axis pisition word */
#define EC_ACCEL_IDX_TEMP2	0xb	/*   device temperature in Celsius */
#define EC_ACCEL_IDX_QUEUED	0xc	/* Number of queued readouts left */
#define EC_ACCEL_IDX_KMACT	0xd	/* keyboard or mouse activity */
#define EC_ACCEL_IDX_RETVAL	0xf	/* command return value, good=0x00 */

#define KEYBD_MASK		0x20	/* set if keyboard activity */
#define MOUSE_MASK		0x40	/* set if mouse activity */

#define READ_TIMEOUT_MSECS	100	/* wait this long for device read */
#define RETRY_MSECS		3	/* retry delay */

#define KMACT_REMEMBER_PERIOD   (hz/10) /* keyboard/mouse persistance */

struct callout hdaps_co;

static devclass_t hdaps_devclass;

/* sysctl node (hw.hdaps) */
SYSCTL_NODE(_hw, OID_AUTO, hdaps, CTLFLAG_RD, NULL, "Hard Disk Active Protection System"); 

static unsigned int hdaps_invert;
static int needs_calibration = 0;

/* Configuration: */
static int sampling_rate = 50;       /* Sampling rate  */
static int oversampling_ratio = 5;   /* Ratio between our sampling rate and 
                                      * EC accelerometer sampling rate      */
static int running_avg_filter_order = 2; /* EC running average filter order */
static int fake_data_mode = 0;       /* Enable EC fake data mode? */

/* Latest state readout: */
int pos_x, pos_y;      /* position */
static int temperature;       /* temperature */
static int stale_readout = 1; /* last read invalid */
int rest_x, rest_y;    /* calibrated rest position */

/* Last time we saw keyboard and mouse activity: */
static u_long last_keyboard_ticks = -300000;
static u_long last_mouse_ticks = -300000;

/* Some models require an axis transformation to the standard reprsentation */
static void transform_axes(int *x, int *y)
{
        if (hdaps_invert) {
                *x = -*x;
                *y = -*y;
        }
}

/**
 * __hdaps_update - query current state, with locks already acquired
 * @fast: if nonzero, do one quick attempt without retries.
 *
 * Query current accelerometer state and update global state variables.
 * Also prefetches the next query. Caller must hold controller lock.
 */
static int __hdaps_update(int fast)
{
	/* Read data: */
	struct thinkpad_ec_row data;
	int ret;

	data.mask = (1 << EC_ACCEL_IDX_READOUTS) | (1 << EC_ACCEL_IDX_KMACT) |
	            (3 << EC_ACCEL_IDX_YPOS1)    | (3 << EC_ACCEL_IDX_XPOS1) |
	            (1 << EC_ACCEL_IDX_TEMP1)    | (1 << EC_ACCEL_IDX_RETVAL);
	if (fast)
		ret = thinkpad_ec_try_read_row(&ec_accel_args, &data);
	else
		ret = thinkpad_ec_read_row(&ec_accel_args, &data);

	thinkpad_ec_prefetch_row(&ec_accel_args); /* Prefetch even if error */
	if (ret)
		return ret;

	/* Check status: */
	if (data.val[EC_ACCEL_IDX_RETVAL] != 0x00) {
		printf("hdaps: read RETVAL=0x%02x\n",
		       data.val[EC_ACCEL_IDX_RETVAL]);
		return -EIO;
	}

	if (data.val[EC_ACCEL_IDX_READOUTS] < 1)
		return -EBUSY; /* no pending readout, try again later */

	/* Parse position data: */
	pos_x = *(short*)(data.val+EC_ACCEL_IDX_XPOS1);
	pos_y = *(short*)(data.val+EC_ACCEL_IDX_YPOS1);
	transform_axes(&pos_x, &pos_y);


	/* Keyboard and mouse activity status is cleared as soon as it's read,
	 * so applications will eat each other's events. Thus we remember any
	 * event for KMACT_REMEMBER_PERIOD jiffies.
	 */
	if (data.val[EC_ACCEL_IDX_KMACT] & KEYBD_MASK)
		last_keyboard_ticks = ticks;
	if (data.val[EC_ACCEL_IDX_KMACT] & MOUSE_MASK)
		last_mouse_ticks = ticks;

	temperature = data.val[EC_ACCEL_IDX_TEMP1];

	stale_readout = 0;
	if (needs_calibration) {
		rest_x = pos_x;
		rest_y = pos_y;
		needs_calibration = 0;
	}

	return 0;
}

/**
 * hdaps_update - acquire locks and query current state
 *
 * Query current accelerometer state and update global state variables.
 * Also prefetches the next query.
 * Retries until timeout if the accelerometer is not in ready status (common).
 * Does its own locking.
 */
int hdaps_update(void)
{
	int total, ret;
	if (!stale_readout) /* already updated recently? */
		return 0;

	for (total=0; total<READ_TIMEOUT_MSECS; total+=RETRY_MSECS) {
		ret = thinkpad_ec_lock();
		if (ret)
			return ret;
		ret = __hdaps_update(0);
		thinkpad_ec_unlock();

		if (!ret)
			return 0;
		if (ret != -EBUSY)
			break;
		/* use tsleep here ? */
		DELAY(RETRY_MSECS);
	}
	return ret;
}

/**
 * hdaps_set_power - enable or disable power to the accelerometer.
 * Returns zero on success and negative error code on failure.  Can sleep.
 */
static int hdaps_set_power(int on) {
	struct thinkpad_ec_row args = 
		{ .mask=0x0003, .val={0x14, on?0x01:0x00} };
	struct thinkpad_ec_row data = { .mask = 0x8000 };
	int ret = thinkpad_ec_read_row(&args, &data);
	if (ret)
		return ret;
	if (data.val[0xF]!=0x00)
		return -EIO;
	return 0;
}

/**
 * hdaps_set_fake_data_mode - enable or disable EC test mode
 * EC test mode fakes accelerometer data using an incrementing counter.
 * Returns zero on success and negative error code on failure.  Can sleep.
 */
static int hdaps_set_fake_data_mode(int on)
{
	struct thinkpad_ec_row args = 
		{ .mask=0x0007, .val={0x17, 0x83, on?0x01:0x00} };
	struct thinkpad_ec_row data = { .mask = 0x8000 };
	int ret = thinkpad_ec_read_row(&args, &data);
	if (ret)
		return ret;
	if (data.val[0xF]!=0x00) {
		printf("failed setting hdaps fake data to %d\n", on);
		return -EIO;
	}
	printf("hdaps: fake_data_mode set to %d\n", on);
	return 0;
}

/**
 * hdaps_set_ec_config - set accelerometer parameters.
 * @ec_rate: embedded controller sampling rate
 * @order: embedded controller running average filter order
 * (Normally we have @ec_rate = sampling_rate * oversampling_ratio.)
 * Returns zero on success and negative error code on failure.  Can sleep.
 */
static int hdaps_set_ec_config(int ec_rate, int order)
{
	struct thinkpad_ec_row args = { .mask=0x000F, 
		.val={0x10, (u_char)ec_rate, (u_char)(ec_rate>>8), order} };
	struct thinkpad_ec_row data = { .mask = 0x8000 };
	int ret = thinkpad_ec_read_row(&args, &data);
	printf("hdaps: setting ec_rate=%d, filter_order=%d\n",
	       ec_rate, order);
	if (ret)
		return ret;
	if (data.val[0xF]==0x03) {
		printf("hdaps: config param out of range\n");
		return -EINVAL;
	}
	if (data.val[0xF]==0x06) {
		printf("hdaps: config change already pending\n");
		return -EBUSY;
	}
	if (data.val[0xF]!=0x00) {
		printf("hdaps: config change error, ret=%d\n",
		      data.val[0xF]);
		return -EIO;
	}
	return 0;
}

/**
 * hdaps_get_ec_config - get accelerometer parameters.
 * @ec_rate: embedded controller sampling rate
 * @order: embedded controller running average filter order
 * Returns zero on success and negative error code on failure.  Can sleep.
 */
static int hdaps_get_ec_config(int *ec_rate, int *order)
{
	const struct thinkpad_ec_row args = 
		{ .mask=0x0003, .val={0x17, 0x82} };
	struct thinkpad_ec_row data = { .mask = 0x801F };
	int ret = thinkpad_ec_read_row(&args, &data);
	if (ret)
		return ret;
	if (data.val[0xF]!=0x00)
		return -EIO;
	if (!(data.val[0x1] & 0x01))
		return -ENXIO; /* accelerometer polling not enabled */
	if (data.val[0x1] & 0x02)
		return -EBUSY; /* config change in progress, retry later */
	*ec_rate = data.val[0x2] | ((int)(data.val[0x3]) << 8);
	*order = data.val[0x4];
	return 0;
}

/**
 * hdaps_get_ec_mode - get EC accelerometer mode
 * Returns zero on success and negative error code on failure.  Can sleep.
 */
static int hdaps_get_ec_mode(u_char *mode)
{
	const struct thinkpad_ec_row args = { .mask=0x0001, .val={0x13} };
	struct thinkpad_ec_row data = { .mask = 0x8002 };
	int ret = thinkpad_ec_read_row(&args, &data);
	if (ret)
		return ret;
	if (data.val[0xF]!=0x00) {
		printf("accelerometer not implemented (0x%02x)\n", data.val[0xF]);
		return -EIO;
	}
	*mode = data.val[0x1];
	return 0;
}

/**
 * hdaps_check_ec - checks something about the EC.
 * Follows the clean-room spec for HDAPS; we don't know what it means.
 * Returns zero on success and negative error code on failure.  Can sleep.
 */
static int hdaps_check_ec(u_char *mode)
{
	const struct thinkpad_ec_row args = 
		{ .mask=0x0003, .val={0x17, 0x81} };
	struct thinkpad_ec_row data = { .mask = 0x800E };
	int ret = thinkpad_ec_read_row(&args, &data);
    return 0;
	if (ret)
		return  ret;
	if (data.val[0x1]!=0x00 || data.val[0x2]!=0x60 ||
	    data.val[0x3]!=0x00 || data.val[0xF]!=0x00)
		return -EIO;
	return 0;
}

/**
 * hdaps_device_init - initialize the accelerometer.
 *
 * Call several embedded controller functions to test and initialize the
 * accelerometer.
 * Returns zero on success and negative error code on failure. Can sleep.
 */
#define ABORT_INIT(msg) printf("hdaps init failed at: %s\n", msg)
static int hdaps_device_init(void)
{
	int ret;
	u_char mode;

	ret = thinkpad_ec_lock();
	if (ret)
		return ret;

	if (hdaps_get_ec_mode(&mode))
                { ABORT_INIT("hdaps_get_ec_mode failed"); goto bad; }

	printf("hdaps: initial mode latch is 0x%02x\n", mode);
	if (mode==0x00)
                { ABORT_INIT("accelerometer not available"); goto bad; }

	if (hdaps_check_ec(&mode))
                { ABORT_INIT("hdaps_check_ec failed"); goto bad; }

	if (hdaps_set_power(1))
                { ABORT_INIT("hdaps_set_power failed"); goto bad; }

	if (hdaps_set_ec_config(sampling_rate*oversampling_ratio,
	                        running_avg_filter_order))
                { ABORT_INIT("hdaps_set_ec_config failed"); goto bad; }

	if (hdaps_set_fake_data_mode(fake_data_mode))
                { ABORT_INIT("hdaps_set_fake_data_mode failed"); goto bad; }

	thinkpad_ec_invalidate();
	DELAY(200);

	/* Just prefetch instead of reading, to avoid ~1sec delay on load */
	ret = thinkpad_ec_prefetch_row(&ec_accel_args);
	if (ret)
                { ABORT_INIT("initial prefetch failed"); goto bad; }
	goto good;
bad:
	thinkpad_ec_invalidate();
	ret = ENXIO;
good:
	stale_readout = 1;
	thinkpad_ec_unlock();
	return ret;
}

/**
 * hdaps_device_shutdown - power off the accelerometer
 * Returns nonzero on failure. Can sleep.
 */
static int hdaps_device_shutdown(void)
{
        int ret;
        ret = hdaps_set_power(0);
        if (ret) {
                printf("hdaps: cannot power off\n");
                return ret;
        }
        ret = hdaps_set_ec_config(0, 1);
        if (ret)
                printf("hdaps: cannot stop EC sampling\n");
        return ret;
}

/* Device model stuff */


static int hdaps_suspend(device_t dev)
{
	/* Don't do hdaps polls until resume re-initializes the sensor. */
	callout_stop(&hdaps_co);
        hdaps_device_shutdown(); /* ignore errors, effect is negligible */
	return 0;
}

static void hdaps_mousedev_poll(void* args);
static int hdaps_resume(device_t dev)
{
	int ret = hdaps_device_init();
	if (ret)
		return ret;

	callout_reset(&hdaps_co, hz/sampling_rate, hdaps_mousedev_poll, NULL);
	return 0;
}

/**
 * hdaps_calibrate - set our "resting" values.
 * Does its own locking.
 */
static void hdaps_calibrate(void)
{
	needs_calibration = 1;
	hdaps_update();
	/* If that fails, the mousedev poll will take care of things later. */
}

/* Timer handler for updating the input device. Runs in softirq context,
 * so avoid lenghty or blocking operations.
 */
static void hdaps_mousedev_poll(void* args)
{
	int ret;

	stale_readout = 1;

	/* Cannot sleep.  Try nonblockingly.  If we fail, try again later. */
	if (thinkpad_ec_try_lock())
		goto keep_active;

	ret = __hdaps_update(1); /* fast update, we're in softirq context */
	thinkpad_ec_unlock();
	/* Any of "successful", "not yet ready" and "not prefetched"? */
	if (ret!=0 && ret!=-EBUSY && ret!=-ENOATTR) {
		printf("hdaps: poll failed, disabling updates\n");
		return;
	}

keep_active:
	/* Even if we failed now, pos_x,y may have been updated earlier: */

	/* Retrun mouse movement */
//	hdaps_mouse_report_pos(pos_x, pos_y);
//	hdaps_joy_report_pos(pos_x - rest_x, pos_y - rest_y);
//	hdaps_mouse_report_pos(pos_x - rest_x, pos_y - rest_y);
	callout_reset(&hdaps_co, hz/sampling_rate, hdaps_mousedev_poll, NULL);
}

/*********************
 * 
 * SYSCTL functions
 *
 */

static int hdaps_sampling_rate_sysctlproc(SYSCTL_HANDLER_ARGS)
{
	int error = 0, rate;

	/* sysctl read or size requested */
	error = SYSCTL_OUT(req, &sampling_rate, sizeof(sampling_rate));

	if(!error && req->newptr) {
		/* sysctl write */
		error = SYSCTL_IN(req, &rate, sizeof(rate));

		if (error)
			return error;

		if (rate > hz || rate < 1)
			return (EINVAL);

		if (rate != sampling_rate) {
			error = hdaps_set_ec_config(rate*oversampling_ratio, 
					running_avg_filter_order);
			if (error)
				return error;

			sampling_rate = rate;
		}
	}

	return error;
}

SYSCTL_PROC(_hw_hdaps, OID_AUTO, sampling_rate, CTLTYPE_INT|CTLFLAG_RW, NULL, 0, hdaps_sampling_rate_sysctlproc, "I", "sampling rate");

static int hdaps_oversampling_ratio_sysctlproc(SYSCTL_HANDLER_ARGS)
{
	int error = 0, ratio, ec_rate, order;

	if(req->oldptr) {
		/* read */
		error = hdaps_get_ec_config(&ec_rate, &order);
		if (error)
			return error; /* EINVAL ??? */
		
		ratio = ec_rate / sampling_rate;
	
		error = SYSCTL_OUT(req, &ratio, sizeof(ratio));

		if (error)
			return error;
		
		oversampling_ratio = ratio;
		running_avg_filter_order = order;

	} else
		error = SYSCTL_OUT(req,0,sizeof(oversampling_ratio));

	if(!error && req->newptr) {
		/* write */
	
		error = SYSCTL_IN(req, &ratio, sizeof(ratio));

		if (error)
			return error;

		if (ratio <1)
			return (EINVAL);

		 if (ratio != oversampling_ratio) {
			error = hdaps_set_ec_config(sampling_rate*ratio, 
					running_avg_filter_order);
			if (error)
				return error;

			oversampling_ratio = ratio;
		}
	}

	return error;
}

SYSCTL_PROC(_hw_hdaps, OID_AUTO, oversampling_ratio, CTLTYPE_INT|CTLFLAG_RW, NULL, 0, hdaps_oversampling_ratio_sysctlproc, "I", "oversampling ratio");

static int hdaps_running_avg_filter_order_sysctlproc(SYSCTL_HANDLER_ARGS)
{
	int error = 0, ec_rate, order, rate;

	if(req->oldptr) {
		/* read */
		error = hdaps_get_ec_config(&ec_rate, &order);
		if (error)
			return error; /* EINVAL ??? */
		
		error = SYSCTL_OUT(req, &order, sizeof(order));

		if (error)
			return error;
		
		running_avg_filter_order = order;

		rate = ec_rate / oversampling_ratio;

		if ( (rate <= hz) && (rate > 0) )
			sampling_rate = rate;

	} else
		error = SYSCTL_OUT(req,0,sizeof(running_avg_filter_order));

	if(!error && req->newptr) {
		/* write */
	
		error = SYSCTL_IN(req, &order, sizeof(order));

		if (error)
			return error;

		 if (order != running_avg_filter_order) {
			error = hdaps_set_ec_config(
					sampling_rate*oversampling_ratio, 
					order);
			if (error)
				return error;

			running_avg_filter_order = order;
		}
	}

	return error; /* 0 */
}

SYSCTL_PROC(_hw_hdaps, OID_AUTO, running_avg_filter_order, CTLTYPE_INT|CTLFLAG_RW, NULL, 0, hdaps_running_avg_filter_order_sysctlproc, "I", "running avg filter order");

static int hdaps_fake_data_mode_sysctlproc(SYSCTL_HANDLER_ARGS)
{
	int error = 0, on;

	/* sysctl read or size requested */
	error = SYSCTL_OUT(req, &fake_data_mode, sizeof(fake_data_mode));

	if(!error && req->newptr) {
		/* sysctl write */
		error = SYSCTL_IN(req, &on, sizeof(on));

		if (error)
			return error;

		if (on < 0 || on > 1)
			return (EINVAL);

		if (on!= fake_data_mode) {
			error = hdaps_set_fake_data_mode(on);

			if (error)
				return error;

			fake_data_mode = on;
		}
	}

	return error;
}

SYSCTL_PROC(_hw_hdaps, OID_AUTO, fake_data_mode, CTLTYPE_INT|CTLFLAG_RW, NULL, 0, hdaps_fake_data_mode_sysctlproc, "I", "fake data mode");

static int hdaps_invert_sysctlproc(SYSCTL_HANDLER_ARGS)
{
	int error = 0, invert;

	/* sysctl read or size requested */
	error = SYSCTL_OUT(req, &hdaps_invert, sizeof(hdaps_invert));

	if(!error && req->newptr) {
		/* sysctl write */
		error = SYSCTL_IN(req, &invert, sizeof(invert));

		if (error)
			return error;

		if (invert < 0 || invert > 1)
			return (EINVAL);

		if (invert != hdaps_invert) {
			hdaps_invert = invert;
			hdaps_calibrate();
		}
	}

	return error;
}

SYSCTL_PROC(_hw_hdaps, OID_AUTO, invert, CTLTYPE_INT|CTLFLAG_RW, NULL, 0, hdaps_invert_sysctlproc, "I", "invert axes");

static int hdaps_calibrate_sysctlproc(SYSCTL_HANDLER_ARGS)
{
	int error = 0, on;

	/* sysctl read or size requested */
	error = SYSCTL_OUT(req, &needs_calibration, sizeof(needs_calibration));
	
	if(!error && req->newptr) {
		/* sysctl write */
		error = SYSCTL_IN(req, &on, sizeof(on));

		if (error)
			return error;

		if (on < 0 || on > 1)
			return (EINVAL);

			hdaps_calibrate();
	}

	return error;
}

SYSCTL_PROC(_hw_hdaps, OID_AUTO, calibrate, CTLTYPE_INT|CTLFLAG_RW, NULL, 0, hdaps_calibrate_sysctlproc, "I", "calibrate");

static int hdaps_mouse_activity_sysctlproc(SYSCTL_HANDLER_ARGS)
{
	int error = 0, activity;

	if (!req->oldptr)
		return SYSCTL_OUT(req, 0, sizeof(activity));

	error = hdaps_update();
	
	if (error)
		return error;

	activity = ticks < last_mouse_ticks + KMACT_REMEMBER_PERIOD;

	/* sysctl read or size requested */
	return SYSCTL_OUT(req, &activity, sizeof(activity));
}

SYSCTL_PROC(_hw_hdaps, OID_AUTO, mouse_activity, CTLTYPE_INT|CTLFLAG_RD, NULL, 0, hdaps_mouse_activity_sysctlproc, "I", "mouse activity");

static int hdaps_keyboard_activity_sysctlproc(SYSCTL_HANDLER_ARGS)
{
	int error = 0, activity;

	if (!req->oldptr)
		return SYSCTL_OUT(req, 0, sizeof(activity));

	error = hdaps_update();
	
	if (error)
		return error;

	activity = ticks < last_keyboard_ticks + KMACT_REMEMBER_PERIOD;

	/* sysctl read or size requested */
	return SYSCTL_OUT(req, &activity, sizeof(activity));
}

SYSCTL_PROC(_hw_hdaps, OID_AUTO, keyboard_activity, CTLTYPE_INT|CTLFLAG_RD, NULL, 0, hdaps_keyboard_activity_sysctlproc, "I", "keyboard activity");

static int hdaps_temp1_sysctlproc(SYSCTL_HANDLER_ARGS)
{
	int error = 0;

	if (!req->oldptr)
		return SYSCTL_OUT(req, 0, sizeof(temperature));

	error = hdaps_update();
	
	if (error)
		return error;

	/* sysctl read or size requested */
	return SYSCTL_OUT(req, &temperature, sizeof(temperature));
}

SYSCTL_PROC(_hw_hdaps, OID_AUTO, temp1, CTLTYPE_INT|CTLFLAG_RD, NULL, 0, hdaps_temp1_sysctlproc, "I", "temperature");

static int hdaps_position_sysctlproc(SYSCTL_HANDLER_ARGS)
{
	int error = 0, position[2];

	if (!req->oldptr)
		return SYSCTL_OUT(req, 0, 2*sizeof(int));

	error = hdaps_update();
	if (error)
		return error;

	position[0] = pos_x;
	position[1] = pos_y;

	return SYSCTL_OUT(req, &position, 2*sizeof(int));
}

SYSCTL_PROC(_hw_hdaps, OID_AUTO, position, CTLTYPE_STRING|CTLFLAG_RD, NULL, 0, hdaps_position_sysctlproc, "I", "position x y");

static int hdaps_rest_position_sysctlproc(SYSCTL_HANDLER_ARGS)
{
	int position[2];

	position[0] = rest_x;
	position[1] = rest_y;
	
	return SYSCTL_OUT(req, &position, 2*sizeof(int));
}

SYSCTL_PROC(_hw_hdaps, OID_AUTO, rest_position, CTLTYPE_STRING|CTLFLAG_RD, NULL, 0, hdaps_rest_position_sysctlproc, "I", "calibrated rest position x y");

static int hdaps_values_sysctlproc(SYSCTL_HANDLER_ARGS)
{
	int error = 0, values[5];

	if (!req->oldptr)
		return SYSCTL_OUT(req, 0, 5*sizeof(int));

	error = hdaps_update();
	if (error)
		return error;

	values[0] = pos_x;
	values[1] = pos_y;
	values[2] = temperature;
	values[3] = ticks < last_keyboard_ticks + KMACT_REMEMBER_PERIOD;
	values[4] = ticks < last_mouse_ticks + KMACT_REMEMBER_PERIOD;
	
	return SYSCTL_OUT(req, &values, 5*sizeof(int));
}

SYSCTL_PROC(_hw_hdaps, OID_AUTO, values, CTLTYPE_STRING|CTLFLAG_RD, NULL, 0, hdaps_values_sysctlproc, "I", "x y temp1 kbd_act mse_act");

/******************************************

	Driver functions 
*/

static void hdaps_identify(driver_t *driver, device_t parent)
{
	device_t child;

	child = device_find_child(parent, DEVICE_NAME, -1);

	if (child)
		return;

	child = device_add_child_ordered(parent, 0, DEVICE_NAME, -1);

}

static int hdaps_probe(device_t dev)
{
	int ret = 0;

	if (device_get_unit(dev) != 0) {
		return (ENXIO);
	}

	ret = hdaps_device_init();
	if (ret)
		return ret;

	device_set_desc(dev, "Accelerometer");
	
	printf("hdaps: device successfully initialized.\n");

	return 0;
}

static int hdaps_attach(device_t dev)
{

	/* FreeBSD DMI workaround */

	/* List of models with abnormal axis configuration.
	 * Note that HDAPS_DMI_MATCH_NORMAL("ThinkPad T42") would match
	 * "ThinkPad T42p", so the order of the entries matters
	 */
	smbios_system_id hdaps_whitelist[] = {
		{ "IBM", "ThinkPad R50p" },
		{ "IBM", "ThinkPad T41p" },
		{ "IBM", "ThinkPad T42p" },
		{ "LENOVO", "ThinkPad T60p" },
		{ "LENOVO", "ThinkPad X60" },
		{ NULL, NULL }
		};

	if ( smbios_check_system(hdaps_whitelist)) {
		hdaps_invert = 1;
		printf("hdaps: inverting axes\n");
	}
	
	/* init callout */
	callout_init(&hdaps_co, 0); /* mpsafe ? */

        /* calibration for the input device (deferred to avoid delay) */
	needs_calibration = 1;

	/* create device */
	//hdaps_mouse_make_dev();
	hdaps_joy_make_dev();
	hdaps_make_dev();

	/* start timer */
	callout_reset(&hdaps_co, hz/sampling_rate, hdaps_mousedev_poll, NULL);

        printf("hdaps: driver successfully loaded.\n");

        return 0;
}

static int hdaps_detach(device_t dev)
{
	
	callout_stop(&hdaps_co);
//	hdaps_mouse_destroy_dev();
	hdaps_joy_destroy_dev();
	hdaps_destroy_dev();
        hdaps_device_shutdown(); /* ignore errors, effect is negligible */
        printf("hdaps: driver unloaded.\n");
	return 0;

}

static device_method_t hdaps_methods[] = {
        DEVMETHOD(device_identify,      hdaps_identify),
        DEVMETHOD(device_probe,         hdaps_probe),
        DEVMETHOD(device_attach,        hdaps_attach),
        DEVMETHOD(device_detach,        hdaps_detach),
        DEVMETHOD(device_suspend,	hdaps_suspend),
        DEVMETHOD(device_resume,	hdaps_resume),
        {0, 0}
};

static driver_t hdaps_driver = {
        DEVICE_NAME,
        hdaps_methods,
	0, /* is this allowed ? */
};

DRIVER_MODULE(hdaps, thinkpad_ec, hdaps_driver, hdaps_devclass, 0, 0);
//DRIVER_MODULE(hdaps, acpi, hdaps_driver, hdaps_devclass, 0, 0);
MODULE_DEPEND(hdaps, thinkpad_ec, 1, 1, 1);
