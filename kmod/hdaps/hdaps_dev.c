#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/conf.h>
#include <sys/selinfo.h>
#include <sys/poll.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/uio.h>
#include <sys/syslog.h>
#include <sys/mouse.h>
#include <sys/sysctl.h>
#include "hdaps.h"
#include "hdaps_dev.h"

#define DEVICE_NAME "hdapstest"

#define FLAG_OPEN	1	/* is device opened */

#define BUFSIZE	240

//SYSCTL_DECL(_hw_hdaps);

//static int input_fuzz = 4;
//SYSCTL_INT(_hw_hdaps, OID_AUTO, input_fuzz, CTLFLAG_RW, &input_fuzz, 0, "HDAPS input fuzz");

static int state = 0;

static struct cdev *hdapsdev;

static d_open_t		hdaps_devopen;
static d_close_t	hdaps_devclose;
static d_read_t		hdaps_devread;
static d_ioctl_t	hdaps_devioctl;

static struct cdevsw hdaps_devsw = {
	.d_version = 	D_VERSION,
	.d_flags = 	D_NEEDGIANT,
	.d_open = 	hdaps_devopen,
	.d_close =	hdaps_devclose,
	.d_read =	hdaps_devread,
	.d_ioctl =	hdaps_devioctl,
	.d_name =	DEVICE_NAME,
};

static int
hdaps_devopen(struct cdev *dev, int flag, int fmt, struct thread *td)
{
	if (state & FLAG_OPEN)
		return (EBUSY);	

	state |= FLAG_OPEN;

	return 0;
}

static int
hdaps_devclose(struct cdev *dev, int flag, int fmt, struct thread *td)
{
	state &= ~FLAG_OPEN;
	return 0;
};

static int
hdaps_devread(struct cdev *dev, struct uio *uio, int flag)
{
	int buf[2], error;

	error = hdaps_update();
	if (error)
		return error;

	buf[0] = pos_x;
	buf[1] = pos_y;

	error = uiomove(buf, sizeof(buf), uio);

	return error;

}

static int
hdaps_devioctl(struct cdev *dev, u_long cmd, caddr_t addr, int flag, struct thread *td)
{
	return ENOTTY;
}

void hdaps_make_dev(void)
{
	state = 0;

	hdapsdev = make_dev(&hdaps_devsw, 0, UID_ROOT, GID_WHEEL, 0600, DEVICE_NAME);
}

void hdaps_destroy_dev(void)
{
	destroy_dev(hdapsdev);
}
