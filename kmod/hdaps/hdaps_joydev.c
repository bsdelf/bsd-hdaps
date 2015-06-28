#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/syslog.h>
#include <sys/joystick.h>
#include <sys/sysctl.h>

#include "hdaps.h"
#include "hdaps_joydev.h"

#define DEVICE_NAME "joy0"

#define FLAG_OPEN	1	/* is device opened */

#define BUFSIZE	240

SYSCTL_DECL(_hw_hdaps);

static int input_fuzz = 4;
SYSCTL_INT(_hw_hdaps, OID_AUTO, input_fuzz, CTLFLAG_RW, &input_fuzz, 0, "HDAPS input fuzz");

static int state = 0;
static int old_x, old_y;

static struct cdev *joydev;

static d_open_t		hdaps_joy_devopen;
static d_close_t	hdaps_joy_devclose;
static d_read_t		hdaps_joy_devread;
static d_ioctl_t	hdaps_joy_devioctl;

static struct cdevsw hdaps_joy_devsw = {
	.d_version = 	D_VERSION,
	.d_flags = 	D_NEEDGIANT,
	.d_open = 	hdaps_joy_devopen,
	.d_close =	hdaps_joy_devclose,
	.d_read =	hdaps_joy_devread,
	.d_ioctl =	hdaps_joy_devioctl,
	.d_name =	DEVICE_NAME,
};

static int
hdaps_joy_devopen(struct cdev *dev, int flag, int fmt, struct thread *td)
{
	if (state & FLAG_OPEN)
		return (EBUSY);	
	
	old_x = pos_x;
	old_y = pos_y;

	state |= FLAG_OPEN;

	return 0;
}

static int
hdaps_joy_devclose(struct cdev *dev, int flag, int fmt, struct thread *td)
{

	state &= ~FLAG_OPEN;
	return 0;
};
/**
static void fuzz(int *value, int *old_value)
{

	if (input_fuzz) {
		
		if ( ( *value > *old_value - (input_fuzz >> 1)) && 
		     ( *value < *old_value + (input_fuzz >> 1)))
			return;

		if ( (*value > *old_value - input_fuzz) &&
		     (*value < *old_value + input_fuzz))
			*value = (*old_value * 3 + *value) >> 2;

		if ( (*value > *old_value - (input_fuzz << 1)) &&
		     (*value < *old_value + (input_fuzz << 1)))
			*value = (*old_value + *value) >> 1; 
	}
	
	if ( *old_value == *value )
		return;

	*old_value = *value;
}
**/
static int
hdaps_joy_devread(struct cdev *dev, struct uio *uio, int flag)
{

	struct joystick joydata;
	int ret;

	ret = hdaps_update();
	if (ret)
		return ret;

	/* anti-jitter */

	/* zero */

	joydata.x = (old_x + pos_x) >> 1;
	joydata.y = (old_y + pos_y) >> 1;
	joydata.b1 = 0;
	joydata.b2 = 0;

	old_x = pos_x;
	old_y = pos_y;

	return uiomove(&joydata, sizeof(struct joystick), uio);
}

static int
hdaps_joy_devioctl(struct cdev *dev, u_long cmd, caddr_t addr, int flag, struct thread *td)
{
	int error = 0;

	/* Perform IOCTL command */
	switch (cmd) {
	default:
		error = ENOTTY;

	}

	return error;
}

void hdaps_joy_make_dev(void)
{
	state = 0;

	/* not optimal we need to detect other devices of "joy" to get
	 * the next free unit number
	 */

	joydev = make_dev(&hdaps_joy_devsw, 0, UID_ROOT, GID_WHEEL, 0444, DEVICE_NAME);
}

void hdaps_joy_destroy_dev(void)
{
	destroy_dev(joydev);
}
