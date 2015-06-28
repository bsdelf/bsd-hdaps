/*
 *  thinkpad_ec.c - coordinate access to ThinkPad-specific hardware resources
 * 
 *  The embedded controller on ThinkPad laptops has a non-standard interface
 *  at IO ports 0x1600-0x161F (mapped to LCP channel 3 of the H8S chip).
 *  The interface provides various system management services (currently 
 *  known: battery information and accelerometer readouts). This driver
 *  provides access and mutual exclusion for the EC interface.
 *  For information about the LPC protocol and terminology, see:
 *  "H8S/2104B Group Hardware Manual",
 * http://documentation.renesas.com/eng/products/mpumcu/rej09b0300_2140bhm.pdf
 *
 *  Copyright (C) 2006 Shem Multinymous <multinymous@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  Ported to FreeBSD by Maik Ehinger <m.ehinger@ltur.de>
 *
 *	Differences between FreeBSD and Linux Version
 *	- DELAY(9) instead of ndelay()
 *		 (microseconds instead nanoseconds)
 *	- use sysctl instead of sysfs
 *
 */

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/module.h>
#include <sys/systm.h>

#include <sys/lock.h>
#include <sys/mutex.h>

//#include <sys/time.h>

#include <machine/bus.h>
#include <sys/rman.h>
#include <machine/resource.h>
#include <machine/cpufunc.h>

#include "thinkpad_ec.h"

#include "smbios.h" /* FreeBSD SMBIOS/DMI hack */

#define TP_VERSION "0.30-FreeBSD"

#define DEVICE_NAME	"thinkpad_ec"

/* IO ports used by embedded controller LPC channel 3: */
#define TPC_BASE_PORT	0x1600
#define TPC_NUM_PORTS	0x20
#define TPC_STR3_PORT	0x04  /* Reads H8S EC register STR3 */
#define TPC_TWR0_PORT	0x10 /* Mapped to H8S EC register TWR0MW/SW  */
#define TPC_TWR15_PORT	0x1f /* Mapped to H8S EC register TWR15. */
  /* (and port TPC_TWR0_PORT+i is mapped to H8S reg TWRi for 0<i<16) */

/* H8S STR3 status flags (see "H8S/2104B Group Hardware Manual" p.549) */
#define H8S_STR3_IBF3B 0x80  /* Bidi. Data Register Input Buffer Full */
#define H8S_STR3_OBF3B 0x40  /* Bidi. Data Register Output Buffer Full */
#define H8S_STR3_MWMF  0x20  /* Master Write Mode Flag */
#define H8S_STR3_SWMF  0x10  /* Slave Write Mode Flag */
#define H8S_STR3_MASK  0xf0  /* All bits we care about in STR3 */

/* Timeouts and retries */
/* At the moment we have only microsecond (1/1,000,000th second) delay instead
 * of the nanoseconds (1/1,000,000,000th second) delay
 */
#define TPC_READ_RETRIES       75 /* Original Linux 150 */
#define TPC_READ_NDELAY        1 /* Original Linux 500 */
#define TPC_REQUEST_RETRIES    1000
#define TPC_REQUEST_NDELAY     1 /* Original Linux 10 */
#define TPC_PREFETCH_TIMEOUT   (hz/10)  /* invalidate prefetch after 0.1sec */

/* A few macros for printk()ing: */
#define MSG_FMT(fmt, args...) \
  "thinkpad_ec: %s: " fmt "\n", __func__, ## args
#define REQ_FMT(msg, code) \
  MSG_FMT("%s: (0x%02x:0x%02x)->0x%02x", \
          msg, args->val[0x0], args->val[0xF], code)

/* State of request prefetching: */
static u_char prefetch_arg0, prefetch_argF;          /* Args of last prefetch */

/* ticks are from type int */
static int prefetch_ticks;                      /* time of prefetch, or: */
#define TPC_PREFETCH_NONE   -300*hz       /* - No prefetch */
#define TPC_PREFETCH_JUNK   (-300*hz+1)   /* - Ignore prefetch */

struct thinkpad_ec_softc
{
	device_t dev;
	bus_space_tag_t	bst;
	bus_space_handle_t bsh;
	int	rid;
	struct resource	*res;
};

struct thinkpad_ec_softc *sc;

static devclass_t thinkpad_ec_devclass;

/*****
	EC access functions
*/

static struct mtx thinkpad_ec_mtx;

/**
 * thinkpad_ec_lock - get lock on the ThinkPad EC
 *
 * Get exclusive lock for accesing the ThinkPad embedded controller LPC3
 * interface. Returns 0 iff lock acquired.
 */
int thinkpad_ec_lock(void)
{
	mtx_lock(&thinkpad_ec_mtx);
	return 0;
}

/**
 * thinkpad_ec_try_lock - try getting lock on the ThinkPad EC
 *
 * Try getting an exclusive lock for accesing the ThinkPad embedded
 * controller LPC3. Returns immediately if lock is not available; neither
 * blocks nor sleeps. Returns 0 iff lock acquired .
 */
int thinkpad_ec_try_lock(void)
{
	return !mtx_trylock(&thinkpad_ec_mtx);
}

/**
 * thinkpad_ec_unlock - release lock on ThinkPad EC
 *
 * Release a previously acquired exclusive lock on the ThinkPad ebmedded
 * controller LPC3 interface.
 */
void thinkpad_ec_unlock(void)
{
	mtx_unlock(&thinkpad_ec_mtx);
}

/************************************
 * bus_space_read/write functions need bus_spacetag/handle 
 *
 */

/* Tell embedded controller to prepare a row */
static int thinkpad_ec_request_row(const struct thinkpad_ec_row *args)
{
	u_char str3;
	int i;

	/* EC protocol requires write to TWR0 (function code): */
	if (!(args->mask & 0x0001)) {
		device_printf(sc->dev, MSG_FMT("bad args->mask=0x%02x", args->mask));
		return -EINVAL;
	}

	/* Check initial STR3 status */
	str3 = bus_space_read_1(sc->bst, sc->bsh, TPC_STR3_PORT) & H8S_STR3_MASK;
	if (str3 & H8S_STR3_OBF3B) { /* data already pending */
		bus_space_read_1(sc->bst, sc->bsh, TPC_TWR15_PORT); /* marks end of previous transaction */
		if (prefetch_ticks == TPC_PREFETCH_NONE)
			device_printf(sc->dev, REQ_FMT("readout already pending", str3));
		return -EBUSY; /* EC will be ready in a few usecs */
	} else if (str3 == H8S_STR3_SWMF) { /* busy with previous request */
		if (prefetch_ticks == TPC_PREFETCH_NONE)
			device_printf(sc->dev, REQ_FMT("EC handles previous request", str3));
		return -EBUSY; /* data will be pending in a few usecs */
	} else if (str3 != 0x00) { /* unexpected status */
		device_printf(sc->dev, REQ_FMT("bad initial STR3", str3));
		return -EIO;
	}

	/* Send TWR0MW */
	bus_space_write_1(sc->bst, sc->bsh, TPC_TWR0_PORT, args->val[0]);
	str3 = bus_space_read_1(sc->bst, sc->bsh, TPC_STR3_PORT) & H8S_STR3_MASK;
	if (str3 != H8S_STR3_MWMF) { /* not accepted */
		device_printf(sc->dev, REQ_FMT("arg0 rejected", str3));
		return -EIO;
	}

	/* Send TWR1 through TWR14 */
	for (i=1; i<TP_CONTROLLER_ROW_LEN-1; i++) 
		if ((args->mask>>i)&1)
			bus_space_write_1(sc->bst, sc->bsh, TPC_TWR0_PORT+i, args->val[i]);
			
	/* Send TWR15 (default to 0x01). This marks end of command. */
	bus_space_write_1(sc->bst, sc->bsh, TPC_TWR15_PORT, (args->mask & 0x8000) ? args->val[0xf] : 0x01);

	 /* Wait until EC starts writing its reply (~60ns on average).
         * Releasing locks before this happens may cause an EC hang
         * due to firmware bug!
         */

	for (i=0; i<TPC_REQUEST_RETRIES; i++) {
		str3 = bus_space_read_1(sc->bst, sc->bsh, TPC_STR3_PORT) & H8S_STR3_MASK;
		if (str3 & H8S_STR3_SWMF) /* EC started replying */
			return 0;
		else if ( str3 == (H8S_STR3_IBF3B|H8S_STR3_MWMF) || str3 == 0x00 ) /* normal progress, wait it out */
			DELAY(TPC_REQUEST_NDELAY); /*XXX*/
		else { /* weired EC status */
			device_printf(sc->dev, REQ_FMT("bad end STR3", str3));
			return -EIO;
		}
	
	} /* for */

	device_printf(sc->dev, REQ_FMT("EC is mysteriously silent", str3));
        return -EIO;

}

/* Read current row data from the controller, assuming it's already
 * requested. 
 */
static int thinkpad_ec_read_data(const struct thinkpad_ec_row *args,
				 struct thinkpad_ec_row *data)
{
        int i;
	u_char str3 = bus_space_read_1(sc->bst, sc->bsh, TPC_STR3_PORT) & H8S_STR3_MASK;
        /* Once we make a request, STR3 assumes the sequence of values listed
         * in the following 'if' as it reads the request and writes its data.
         * It takes about a few dozen nanosecs total, with very high variance.
         */
        if (str3==(H8S_STR3_IBF3B|H8S_STR3_MWMF) ||
            str3==0x00 ||   /* the 0x00 is indistinguishable from idle EC! */
            str3==H8S_STR3_SWMF )
                return -EBUSY; /* not ready yet */

        /* Finally, the EC signals output buffer full: */
        if (str3 != (H8S_STR3_OBF3B|H8S_STR3_SWMF)) {
		device_printf(sc->dev,
			 REQ_FMT("bad initial STR3", str3));
                return -EIO;
        }

        /* Read first byte (signals start of read transactions): */
        data->val[0] = bus_space_read_1(sc->bst, sc->bsh, TPC_TWR0_PORT);
        /* Optionally read 14 more bytes: */
        for (i=1; i<TP_CONTROLLER_ROW_LEN-1; i++)
                if ((data->mask >> i)&1)
        		data->val[i] = bus_space_read_1(sc->bst, sc->bsh, TPC_TWR0_PORT+i);

        /* Read last byte from 0x161F (signals end of read transaction): */
        data->val[0xf] = bus_space_read_1(sc->bst, sc->bsh, TPC_TWR15_PORT);
                
        /* Readout still pending? */
	str3 = bus_space_read_1(sc->bst, sc->bsh, TPC_STR3_PORT) & H8S_STR3_MASK;
        if (str3 & H8S_STR3_OBF3B)
		device_printf(sc->dev,
			 REQ_FMT("OBF3B=1 after read", str3));

	/* If port 0x161f return 0x80 to often, the EC may lock up */
	if (data->val[0xf] == 0x80)
		device_printf(sc->dev,
			REQ_FMT("0x161f reports error", data->val[0xf]));

        return 0;
}

/* Is the given row currently prefetched? 
 * To keep things simple we compare only the first and last args;
 * in practice this suffices                                        .*/
static int thinkpad_ec_is_row_fetched(const struct thinkpad_ec_row *args)
{
	int ret;
        ret = (prefetch_ticks != TPC_PREFETCH_NONE) &&
               (prefetch_ticks != TPC_PREFETCH_JUNK) &&
               (prefetch_arg0 == args->val[0]) &&
               (prefetch_argF == args->val[0xF]) &&
               (ticks < prefetch_ticks + TPC_PREFETCH_TIMEOUT);
/*	printf("prefetch_ticks: %i NONE:JUNK: %i:%i\n \
		parg0:pargF: %i:%i arg0:argF: %i:%i\n \
		ticks:TIMEOUT: %i:%i --> %i\n",
		prefetch_ticks, TPC_PREFETCH_NONE, TPC_PREFETCH_JUNK,
		prefetch_arg0, prefetch_argF, args->val[0], args->val[0xf],
		ticks, TPC_PREFETCH_TIMEOUT, ret);
*/
	return ret;

}

/**
 * thinkpad_ec_read_row - request and read data from ThinkPad EC
 * @args Input register arguments
 * @data Output register values
 *
 * Read a data row from the ThinkPad embedded controller LPC3 interface.
 * Does fetching and retrying if needed. The row args are specified by
 * 16 byte arguments, some of which may be missing (but the first is
 * mandatory). These are given in @args->val[], where @args->val[i] is
 * used iff (@args->mask>>i)&1). The rows's data is stored in @data->val[],
 * but is only guaranteed to be valid for indices corresponding to set
 * bit in @data->mask. That is, if (@data->mask>>i)&1==0 then @data->val[i]
 * may not be filled (to save time).
 *
 * Returns -EBUSY on transient error and -EIO on abnormal condition.
 * Caller must hold controller lock.
 */
int thinkpad_ec_read_row(const struct thinkpad_ec_row *args,
                           struct thinkpad_ec_row *data)
{
        int retries, ret;

        if (thinkpad_ec_is_row_fetched(args))
                goto read_row; /* already requested */

        /* Request the row */
        for (retries=0; retries<TPC_READ_RETRIES; ++retries) {
                ret = thinkpad_ec_request_row(args);
                if (!ret)
                        goto read_row;
                if (ret != -EBUSY)
                        break;
                DELAY(TPC_READ_NDELAY); /* XXX */
        }
	device_printf(sc->dev, REQ_FMT("failed requesting row", ret));
        goto out;

read_row:
        /* Read the row's data */
        for (retries=0; retries<TPC_READ_RETRIES; ++retries) {
                ret = thinkpad_ec_read_data(args, data);
                if (!ret)
                        goto out;
                if (ret!=-EBUSY)
                        break;
                DELAY(TPC_READ_NDELAY); /*XXX*/
        }

	device_printf(sc->dev, REQ_FMT("failed waiting for data", ret));

out:
        prefetch_ticks = TPC_PREFETCH_JUNK;
        return ret;
}

/**
 * thinkpad_ec_try_read_row - try reading prefetched data from ThinkPad EC
 * @args Input register arguments
 * @data Output register values
 *
 * Try reading a data row from the ThinkPad embedded controller LPC3
 * interface, if this raw was recently prefetched using
 * thinkpad_ec_prefetch_row(). Does not fetch, retry or block.
 * The parameters have the same meaning as in thinkpad_ec_read_row().
 *
 * Returns -EBUSY is data not ready and -ENOATTR if row not prefetched.
 * Caller must hold controller lock.
 */
int thinkpad_ec_try_read_row(const struct thinkpad_ec_row *args,
                               struct thinkpad_ec_row *data)
{
        int ret;
        if (!thinkpad_ec_is_row_fetched(args)) {
                ret = -ENOATTR;
        } else {
                ret = thinkpad_ec_read_data(args, data);
                if (!ret)
                        prefetch_ticks = TPC_PREFETCH_NONE; /* eaten up */
        }
        return ret;
}

/**
 * thinkpad_ec_prefetch_row - prefetch data from ThinkPad EC
 * @args Input register arguments
 *
 * Prefetch a data row from the ThinkPad embedded controller LCP3
 * interface. A subsequent call to thinkpad_ec_read_row() with the
 * same arguments will be faster, and a subsequent call to
 * thinkpad_ec_try_read_row() stands a good chance of succeeding if
 * done neither too soon nor too late. See
 * thinkpad_ec_read_row() for the meaning of @args.
 *
 * Returns -EBUSY on transient error and -EIO on abnormal condition.
 * Caller must hold controller lock.
 */
int thinkpad_ec_prefetch_row(const struct thinkpad_ec_row *args)
{
        int ret;
        ret = thinkpad_ec_request_row(args);
        if (ret) {
                prefetch_ticks = TPC_PREFETCH_JUNK;
        } else {
                prefetch_ticks = ticks;
                prefetch_arg0 = args->val[0x0];
                prefetch_argF = args->val[0xF];
        }
        return ret;
}

/**
 * thinkpad_ec_invalidate - invalidate prefetched ThinkPad EC data
 *
 * Invalidate the data prefetched via thinkpad_ec_prefetch_row() from the
 * ThinkPad embedded controller LPC3 interface.
 * Must be called before unlocking by any code that accesses the controller
 * ports directly.
 */
void thinkpad_ec_invalidate(void) 
{
        prefetch_ticks = TPC_PREFETCH_JUNK;
}

/*** Checking for EC hardware ***/

/* thinkpad_ec_test:
 * Ensure the EC LPC3 channel really works on this machine by making
 * an arbitrary harmless EC request and seeing if the EC follows protocol.
 * This test writes to IO ports, so execute only after checking DMI.
 */
static int thinkpad_ec_test(void)
{
        int ret;
        const struct thinkpad_ec_row args = /* battery 0 basic status */
          { .mask=0x8001, .val={0x01,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x00} };
        struct thinkpad_ec_row data = { .mask = 0x0000 };

        ret = thinkpad_ec_lock();
	if (ret)
		return ret;

        ret = thinkpad_ec_read_row(&args, &data);

        thinkpad_ec_unlock();

        return ret;
}

/* FreeBSD SMBIOS/DMI Hack */

/* Check DMI for exstence of ThinkPad embedded controller */
static int check_dmi_for_ec(void)
{

	static smbios_system_id tp_whitelist[] = {
			{ "IBM", "ThinkPad A30" },
			{ "IBM", "ThinkPad T23" },
			{ "IBM", "ThinkPad X24" },
			{ NULL, NULL }
			};

	return smbios_find_oem_substring("IBM ThinkPad Embedded Controller") ||
		smbios_check_system(tp_whitelist);
}

/******************************************
 *	
 *	Driver functions 
 *
 */

static void thinkpad_ec_identify(driver_t *driver, device_t parent)
{
	/* Don't use softc here */

	device_t child;

        if (!check_dmi_for_ec()) {
                printf("thinkpad_ec: no ThinkPad embedded controller!\n");
		return;
               // return (-ENODEV);
        }
	
	if (device_find_child(parent, DEVICE_NAME, -1)) {
                return;
	}

	child = BUS_ADD_CHILD(parent, 0, DEVICE_NAME, -1);

	if (child)
		bus_set_resource(child, SYS_RES_IOPORT, 0,
					TPC_BASE_PORT, TPC_NUM_PORTS);

}

static int thinkpad_ec_probe(device_t dev)
{
	/* Don't use softc here */

	if (bus_get_resource_start(dev, SYS_RES_IOPORT, 0) != TPC_BASE_PORT)
		return (ENXIO);

	if (device_get_unit(dev) != 0 )
		return (ENXIO);

        if (!check_dmi_for_ec())
		return (ENXIO);

	//device_set_desc(dev, "IBM ThinkPad Embedded Controller");
	device_set_desc_copy(dev, smbios_values.oem_string);

	return (0);

}

static int thinkpad_ec_attach(device_t dev)
{
	
	sc = (struct thinkpad_ec_softc *) device_get_softc(dev);
	sc->dev = dev;

	sc->rid = 0;

	sc->res = bus_alloc_resource(dev, SYS_RES_IOPORT, &sc->rid, 0, ~0, 1,RF_ACTIVE);

	if (sc->res == NULL) {
		device_printf(dev,
			"thinkpad_ec_attach: cannot claim io ports %#x-%#x\n",
			TPC_BASE_PORT,
			TPC_BASE_PORT + TPC_NUM_PORTS -1);
		return ENXIO;
	}

	sc->bst = rman_get_bustag(sc->res);
	sc->bsh = rman_get_bushandle(sc->res);

	/* Try accessing EC */
	mtx_init(&thinkpad_ec_mtx, DEVICE_NAME, NULL, MTX_DEF);

	prefetch_ticks = TPC_PREFETCH_JUNK;
        if (thinkpad_ec_test()) {
                device_printf(sc->dev, "initial ec test failed\n");
                return -ENXIO;
        }

        device_printf(dev, "thinkpad_ec " TP_VERSION " loaded.\n");
	
	return 0;

}

static int thinkpad_ec_detach(device_t dev)
{
	mtx_destroy(&thinkpad_ec_mtx);

	bus_release_resource(dev, SYS_RES_IOPORT, sc->rid, sc->res);
	device_printf(dev, "thinkpad_ec: unloaded.\n");
	return 0;

}

static device_method_t thinkpad_ec_methods[] = {
	DEVMETHOD(device_identify,	thinkpad_ec_identify),
	DEVMETHOD(device_probe,		thinkpad_ec_probe),
	DEVMETHOD(device_attach,	thinkpad_ec_attach),
	DEVMETHOD(device_detach,	thinkpad_ec_detach),
	{0, 0}
};

static driver_t thinkpad_ec_driver = {
	DEVICE_NAME,
	thinkpad_ec_methods,
	sizeof(struct thinkpad_ec_softc),
};

DRIVER_MODULE(thinkpad_ec, acpi, thinkpad_ec_driver, thinkpad_ec_devclass, 0, 0);
MODULE_VERSION(thinkpad_ec, 1);
