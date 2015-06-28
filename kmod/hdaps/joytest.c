/*
 * Copyright (c) 2005 Maik Ehinger <m.ehinger@ltur.de>
 */
/*
 * Displays the Accelerometer data provided by hdaps.ko (sysctl hw.hdaps)
 *
 * compile with
 * 		cc -Wall -lncurse -o hdapsmonitor hdapsmonitor.c
 */

#include <sys/types.h>

#include <sys/sysctl.h>
#include <sys/joystick.h>

#include <ncurses.h>/* ncurses.h includes stdio.h */  

#include <fcntl.h>

#define DELAY_USEC	50000	/* 50000 looks nice */

int main()
{

	int pos_x, pos_y, rest_x, rest_y, old_pos_x, old_pos_y, dx, dy;
	int xmin, xmax, ymin, ymax, xcorr, ycorr;
	int char_x, char_y, delchar_x, delchar_y;
	struct joystick joydata = { 0 };
	struct timeval timeout = { 0, DELAY_USEC };

	/* to store the number of rows and the number of colums of the screen */
 	int row, col, lastrow;
	
	float sxscale, syscale;
	float xscale = 1, yscale = 1;

	int len, fd, ret;

	pos_x = pos_y = old_pos_x = old_pos_y = 0;
	xmin = ymin = 32000;
	xmax = ymax = -32000;

	fd = open("/dev/joy0", O_RDONLY);

	if (fd < 0) {
		printf("Device open error\n");
		return (1);	
	}

	initscr();/* start the curses mode */

	getmaxyx(stdscr,row,col);/* get the number of rows and columns */

	syscale = (float)row / 255;
	sxscale = (float)col / 255;

	col--;

	printw("rows: %i columns:%i sxscale %.5f syscale %.5f delay: %lius\n",
			 row, col, sxscale, syscale, timeout.tv_usec);

	lastrow = row - 1;
	row = row >> 1;
	col = col >> 1;

	ret = read(fd, &joydata, sizeof(joydata));
	if (!ret) {
		endwin();
		printf("Device read error\n");
		return (1);
	}

	rest_x = old_pos_x = joydata.x;
	rest_y = old_pos_y = joydata.y;
	pos_x = pos_y = 0;

	/* calculate screen position */
	delchar_x = col;
	delchar_y = row;

	/* loop */
	for(;;) {
		
		ret = read(fd, &joydata, sizeof(joydata));
		if (!ret) {
			endwin();
			printf("Device read error\n");
			return (1);
		}

		/* calculate screen position */
		char_x = sxscale * joydata.x;
		char_y = syscale * joydata.y;

		/* */

		mvaddch(delchar_y, delchar_x, ' ');
		mvaddch(char_y, char_x, '*');

		mvprintw(2,0,"    x: %4i  y: %4i", joydata.x, joydata.y);
		mvprintw(3,0,"charx: %4i  y: %4i", char_x, char_y);
		mvprintw(lastrow, 0, "Exit: <STRG+C>");

		delchar_x = char_x;
		delchar_y = char_y;

		refresh();

		select(0, NULL, NULL, NULL, &timeout);

	}

	endwin();

	return 0;
}
