/*
 * Copyright (c) 2005 Maik Ehinger <m.ehinger@ltur.de>
 *
 * Displays the Accelerometer data provided by hdaps.ko (sysctl hw.hdaps)
 * needs ncurses
 *
 * compile with
 * 		cc -Wall -lncurses -o hdapsmonitor hdapsmonitor.c
 */

#include <sys/types.h>

#include <sys/sysctl.h>

#include <ncurses.h>/* ncurses.h includes stdio.h */  

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#define DELAY_USEC	50000	/* 50000 looks nice */

int main()
{

	int i, xpos, datapos = 0, temppos, values[5] = { 0 }, rest_pos[2];
	int max_x = 0, max_y = 0, min_x = 0 , min_y = 0, diffx, diffy;
	struct timeval timeout = { 0, DELAY_USEC };

	/* to store the number of rows and the number of colums of the screen */
 	int row, col, rowhalf;
	
	float scale;

	int len;

	initscr();/* start the curses mode */

	getmaxyx(stdscr,row,col);/* get the number of rows and columns */

	rowhalf = row >> 1;
	scale = (float)rowhalf / 128; 

 	int xdata[col];
 	int ydata[col];

	col--;

	/* init arrays */
	for(i=col;i>0;i--) {
		 xdata[i] = rowhalf;
		 ydata[i] = rowhalf;
	}

	printw("rows: %i columns:%i scale %.5f delay: %lius\n",
			 row, col, scale, timeout.tv_usec);
	
	len = sizeof(rest_pos);

	int fd, error;

	fd = open("/dev/hdapstest", O_RDONLY);

	if (fd < 0)
		return 1;


	printf("fd: %i\n", fd);
 //	sysctlbyname("hw.hdaps.rest_position", &rest_pos, &len, NULL, 0);
 
	len = sizeof(rest_pos);

	error = read(fd, &rest_pos, len);

//	if (sysctlbyname("hw.hdaps.values", &values, &len, NULL, 0)) {
	if (!error) {
		perror("-->");
		close(fd);
		endwin();
		return 1;
	}
	
	len = 2*sizeof(int);
	/* loop */
	while(1) {
		
		error = read(fd, &values, len);
	 	//sysctlbyname("hw.hdaps.values", &values, &len, NULL, 0);
		if (!error) {
			close(fd);
			endwin();
			return error;
		}
		/* */
		diffy =  rest_pos[1] - values[1];
		if (diffy > max_y) max_y = diffy;
		if (diffy < min_y) min_y = diffy;
		ydata[datapos] = rowhalf + (scale) * diffy;

		diffx =  -1 * (rest_pos[0] - values[0]);
		if (diffx > max_x) max_x = diffx;
		if (diffx < min_x) min_x = diffx;
		xdata[datapos] = rowhalf + (scale) * diffx;

		/* redraw screen */
		for(xpos=1;xpos<col;xpos++) {

			temppos = xpos + datapos;
			if(temppos > col) 
				temppos -= col;

			mvaddch(xdata[temppos], xpos, ' ');
			mvaddch(ydata[temppos], xpos, ' ');

			temppos += 1;
			if(temppos>col) temppos = 1;

			mvaddch(xdata[temppos], xpos, '+');
			mvaddch(ydata[temppos], xpos, '-');

		}

		mvprintw(2,2,"x: %5i   y: %5i temp1: %4i kbd_act: %2i mse_act: %2i",
				diffx, diffy, values[2], values[3], values[4]);
		mvprintw(3,0,"max: %5i max: %5i\nmin: %5i min: %5i",
				max_x, max_y, min_x, min_y);
		mvprintw(row-1,0,"Exit: <STRG+C>");

		refresh();

		if(datapos<col)
			 datapos++;
		else
			datapos = 0;


		select(0, NULL, NULL, NULL, &timeout);

	}

	close(fd);

	endwin();

	return 0;
}
