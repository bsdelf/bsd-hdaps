/*
 * Copyright (c) 2005 Maik Ehinger <m.ehinger@ltur.de>
 *
 * Displays the informations provided by hdaps.ko (sysctl hw.hdaps)
 * needs sgvalib
 *
 * compile with
 *	cc -lvga -I/usr/local/include -L/usr/local/lib -o hdapsmonitor_vga hdapsmonitor_vga.c
 */

#include <sys/types.h>

#include <sys/sysctl.h>

#include <vga.h>

#include <fcntl.h>

#define DELAY_USEC	50000	/* 50000 looks nice */

int main()
{


	u_int i, xpos, datapos = 0, temppos, step = 5;
	int values[2], rest_pos[2], diffx, diffy, max_x, max_y, min_x, min_y;
	struct timeval timeout = { 0, DELAY_USEC };

	float scale;

 	/* to store the number of rows and the number of colums of the screen */
 	long row, col, rowhalf;

	int len;

	vga_setmode(G320x240x256);

	col=320;
	row=240;
	rowhalf = row >> 1;

	scale = (float)rowhalf / 128;

 	long xdata[col];
 	long ydata[col];

	col--;

	for(i=col;i>0;i--) {
		 xdata[i] = rowhalf;
		 ydata[i] = rowhalf;
	}

	len = sizeof(rest_pos);
 	sysctlbyname("hw.hdaps.rest_position", &rest_pos, &len, NULL, 0);

	len = sizeof(values);
	if (sysctlbyname("hw.hdaps.position", &values, &len, NULL, 0)) {
		vga_setmode(TEXT);
		return 1;
	}

	/* loop */
	while(1) {

		sysctlbyname("hw.hdaps.position", &values, &len, NULL, 0);

		/* */
		diffy =  rest_pos[1] - values[1];
		ydata[datapos] = rowhalf + (scale) * diffy;

		diffx =  -1 * (rest_pos[0] - values[0]);
		if (diffx > max_x) max_x = diffx;
		xdata[datapos] = rowhalf + (scale) * diffx;

		/* redraw screen */
		for(xpos=1;xpos<col;xpos++) {

			temppos = xpos + datapos;
			if(temppos > col) 
				temppos -= col;

			vga_setcolor(0); /* black */
			vga_drawline(xpos-1, xdata[temppos-1], xpos, xdata[temppos]);
			vga_drawline(xpos-1, ydata[temppos-1], xpos, ydata[temppos]);
			temppos += 1;
			if(temppos>col) temppos = 1;

			vga_setcolor(1); /* blue */
			vga_drawline(xpos-1, ydata[temppos-1], xpos, ydata[temppos]);
			vga_setcolor(4); /* red */
			vga_drawline(xpos-1, xdata[temppos-1], xpos, xdata[temppos]);

		}


		if(datapos<col)
			 datapos++;
		else
			datapos = 0;

		if( vga_getkey() == 'q' ) {
			vga_setmode(TEXT);
			return 0;
		}

		select(0, NULL, NULL, NULL, &timeout);

	}

	vga_setmode(TEXT);
	return 0;
}

