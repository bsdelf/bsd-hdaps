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
#include "hdaps_mousedev.h"

#define DEVICE_NAME "hdaps"

struct mtx hdaps_mouse_mtx;

#define FLAG_OPEN	1	/* is device opened */

#define BUFSIZE	240

//SYSCTL_DECL(_hw_hdaps);

//static int input_fuzz = 4;
//SYSCTL_INT(_hw_hdaps, OID_AUTO, input_fuzz, CTLFLAG_RW, &input_fuzz, 0, "HDAPS input fuzz");

struct ringbuf {
	int count;
	int head;
	int tail;
	u_char buf[BUFSIZE];
} queue;

struct selinfo rsel;
static mousehw_t hw = {	.buttons = 0,
			.iftype = MOUSE_IF_PS2,
			.type = MOUSE_MOUSE,
			.model = MOUSE_MODEL_GENERIC,
			.hwid = 0 };

mousemode_t mode = { 	.level = 0,
			.protocol = MOUSE_PROTO_PS2,
			.rate = 50,
			.resolution = 1,
			.accelfactor = 0, /* disabled */
			.packetsize = MOUSE_PS2_PACKETSIZE,
			.syncmask[0] = MOUSE_PS2_SYNCMASK,
			.syncmask[1] = MOUSE_PS2_SYNC };

mousestatus_t	status;
static int state = 0;
static int old_x, old_y;

static struct cdev *mousedev;

static d_open_t		hdaps_mouse_devopen;
static d_close_t	hdaps_mouse_devclose;
static d_read_t		hdaps_mouse_devread;
static d_ioctl_t	hdaps_mouse_devioctl;
static d_poll_t		hdaps_mouse_devpoll;

static struct cdevsw hdaps_mouse_devsw = {
	.d_version = 	D_VERSION,
	.d_flags = 	D_NEEDGIANT,
	.d_open = 	hdaps_mouse_devopen,
	.d_close =	hdaps_mouse_devclose,
	.d_read =	hdaps_mouse_devread,
	.d_ioctl =	hdaps_mouse_devioctl,
	.d_poll =	hdaps_mouse_devpoll,
	.d_name =	DEVICE_NAME,
};

static int
hdaps_mouse_devopen(struct cdev *dev, int flag, int fmt, struct thread *td)
{
	if (state & FLAG_OPEN)
		return (EBUSY);	

	queue.count = 0;
	queue.head = 0;
	queue.tail = 0;
	status.flags = 0;
	status.button = 0;
	status.obutton = 0;
	status.dx = 0;
	status.dy = 0;
	status.dz = 0;

	state |= FLAG_OPEN;

	return 0;
}

static int
hdaps_mouse_devclose(struct cdev *dev, int flag, int fmt, struct thread *td)
{
	state &= ~FLAG_OPEN;
	return 0;
};

static int
hdaps_mouse_devread(struct cdev *dev, struct uio *uio, int flag)
{
	int l, error = 0;
	u_char buf[BUFSIZE];

 	/* copy data to the user land */
	/* lock queue.count ??? */
	while ((queue.count > 0) && (uio->uio_resid > 0)) {

		mtx_lock(&hdaps_mouse_mtx);

		l = imin(queue.count, uio->uio_resid);
		if (l > sizeof(buf))
			l = sizeof(buf);
		if (l > sizeof(queue.buf) - queue.head) {
			bcopy(&queue.buf[queue.head], &buf[0], 
				sizeof(queue.buf) - queue.head);
			bcopy(&queue.buf[0],
				&buf[sizeof(queue.buf) - queue.head],
				l - (sizeof(queue.buf) - queue.head));
		} else {
			bcopy(&queue.buf[queue.head], &buf[0], l);
		}
		queue.count -= l;
		queue.head = (queue.head + l) % sizeof(queue.buf);

		mtx_unlock(&hdaps_mouse_mtx);

		error = uiomove(buf, l, uio);

		if (error)
			break;
    }

    return error;

}

static int
hdaps_mouse_devioctl(struct cdev *dev, u_long cmd, caddr_t addr, int flag, struct thread *td)
{
	int error = 0;
	mousestatus_t tmpstatus;

	/* Perform IOCTL command */
	switch (cmd) {

	case MOUSE_GETHWINFO:
		*(mousehw_t *)addr = hw;
		break;

	case MOUSE_GETMODE:
		*(mousemode_t *)addr = mode;
		break;

	case MOUSE_GETLEVEL:
 		*(int *)addr = mode.level;
		break;

	case MOUSE_SETLEVEL:
		error = EINVAL;
		break;

	case MOUSE_GETSTATUS:
		tmpstatus = status;
		tmpstatus.flags = 0;
		tmpstatus.obutton = status.button;
		tmpstatus.button = 0;
		tmpstatus.dx = 0;
		tmpstatus.dy = 0;
		tmpstatus.dz = 0;
		*(mousestatus_t *)addr = tmpstatus;
		break;

	default:
		error = ENOTTY;

	}

	return error;
}

static int
hdaps_mouse_devpoll(struct cdev *dev, int events, struct thread *td)
{
	int revents = 0;

	/* Return true if a mouse event available */
	if (events & (POLLIN | POLLRDNORM)) {
		/* lock ??? */
		if (queue.count > 0)
			revents |= events & (POLLIN | POLLRDNORM);
		else
			selrecord(td, &rsel);
	}

	return (revents);
}

void hdaps_mouse_make_dev(void)
{
	state = 0;

	mtx_init(&hdaps_mouse_mtx, "hdapsmouse", NULL, MTX_DEF);

	mousedev = make_dev(&hdaps_mouse_devsw, 0, UID_ROOT, GID_WHEEL, 0600, DEVICE_NAME);
}

void hdaps_mouse_destroy_dev(void)
{
	destroy_dev(mousedev);
	mtx_destroy(&hdaps_mouse_mtx);
}

/* relative */
void hdaps_mouse_report_pos(int x, int y)
{
	/* PS/2 Mouse Packet Size 3 byte */
	u_char buf[MOUSE_PS2_PACKETSIZE];
	int i, l, dx, dy;

	if (!(state & FLAG_OPEN))
		return;

	/* calculate deltas */
	/* needs some work */
	dx = x - old_x;
	dy = y - old_y;

	/* don't queue zero movements */	
	if (!(dx||dy))
		return;

	old_x = x;
	old_y = y;
//	printf("--> x: %4i y: %4i\n", dx, dy);

	/* no Buttons */
	buf[0] = MOUSE_PS2_SYNC;

	i = imax(imin(dx, 255), -256);
	if (i < 0)
		buf[0] |= MOUSE_PS2_XNEG;
	buf[1] = i;

	i = imax(imin(dy, 255), -256);
	if (i < 0)
		buf[0] |= MOUSE_PS2_YNEG;
	buf[2] = i;

	mtx_lock(&hdaps_mouse_mtx); 

	/* queue data */
	if (queue.count + MOUSE_PS2_PACKETSIZE < sizeof(queue.buf)) {
		l = imin(MOUSE_PS2_PACKETSIZE, sizeof(queue.buf) - queue.tail);
		bcopy(&buf[0], &queue.buf[queue.tail], l);
		if (MOUSE_PS2_PACKETSIZE > l)
			bcopy(&buf[l], &queue.buf[0], MOUSE_PS2_PACKETSIZE - l);
		queue.tail = (queue.tail + MOUSE_PS2_PACKETSIZE) % sizeof(queue.buf);
		queue.count += 1;
	}

	mtx_unlock(&hdaps_mouse_mtx); 

	selwakeuppri(&rsel, PZERO);

	return;

}
