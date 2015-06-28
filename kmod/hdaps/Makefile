KMOD=	hdaps
SRCS=	hdaps.c hdaps_dev.c hdaps_joydev.c
SRCS+=	pci_if.h bus_if.h device_if.h

utils:
	cc -Wall -lncurses -o hdapsmonitor hdapsmonitor.c
	cc -lvga -I/usr/local/include -L/usr/local/lib -o hdapsmonitor_vga hdapsmonitor_vga.c 

.include <bsd.kmod.mk>
