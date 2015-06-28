#include <unistd.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <fcntl.h>
#include <sys/time.h>
#include <stdio.h>

#define LOOPS	100000

int main(void)
{

	int buf[2], len, i;
	struct timeval tv0, tv1, tv2, tv3;

	len = sizeof(buf);

/* sysclt */

	gettimeofday(&tv1, NULL);


	for (i=0;i<LOOPS; i++) {
		if( sysctlbyname("hw.hdaps.position", &buf, &len, NULL, 0)) {
			printf("sysctl error\n");
			break;
		}


	} /* for */

	gettimeofday(&tv2, NULL);

	timersub(&tv2, &tv1, &tv0);
	printf("%lu %lu.%06lu seconds\n", i, tv0.tv_sec, tv0.tv_usec);

	printf("%i %i\n", buf[0], buf[1]);


/* dev */

	int fd, error;

	fd = open("/dev/hdapstest", O_RDONLY);

	if (fd < 0) {
		printf("dev open error\n");
		return 1;
	}

	gettimeofday(&tv1, NULL);
	for (i=0;i<LOOPS; i++) {

		error = read(fd, &buf, len);	
		if (!error)
			break;
	} /* for */

	gettimeofday(&tv2, NULL);

	close(fd);

	timersub(&tv2, &tv1, &tv0);
	printf("%lu %lu.%06lu seconds\n", i, tv0.tv_sec, tv0.tv_usec);

	printf("%i %i\n", buf[0], buf[1]);


	gettimeofday(&tv1, NULL);
	for (i=0;i<LOOPS; i++) {
		fd = open("/dev/hdapstest", O_RDONLY);


		if (fd < 0) {
			printf("dev open error\n");
			return 1;
		}


		error = read(fd, &buf, len);	
		if (!error)
			break;
	
		close(fd);
	} /* for */


	gettimeofday(&tv2, NULL);

	timersub(&tv2, &tv1, &tv0);
	printf("%lu %lu.%06lu seconds\n", i, tv0.tv_sec, tv0.tv_usec);

	printf("%i %i\n", buf[0], buf[1]);

	return 0;
}
