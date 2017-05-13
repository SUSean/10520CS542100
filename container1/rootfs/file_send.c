#include <sys/inotify.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#define BUF_LEN (10 * (sizeof(struct inotify_event) + NAME_MAX + 1))

int main(int argc, char *argv[])
{
	int inotifyFd, wd, j;
	char buf[BUF_LEN] __attribute__ ((aligned(8)));
	ssize_t numRead;
	char *p;
	struct inotify_event *event;

	inotifyFd = inotify_init();
	if (inotifyFd == -1) {
		perror(strerror(errno));
		printf("inotifyFd\n");
		return 1;
	}

	wd = inotify_add_watch(inotifyFd, getcwd(NULL, 0), IN_CLOSE_WRITE);
	if (wd == -1) {
		perror(strerror(errno));
		printf("inotify_add_watch\n");
		return 1;
	}
	FILE *fp = fopen("message", "w");
	char ch;
	char signal[4];
	int i = 0;
	printf("Send : ");
	while((ch = getchar()) != '\n'){
			fputc(ch, fp);
			putchar(ch);
			if(i<4)
					signal[i]=ch;
			i++;
	}
	fputc('\n', fp);
	putchar('\n');
	fclose(fp);

	while(1) {
		numRead = read(inotifyFd, buf, BUF_LEN);
		if (numRead <= 0) {
			perror(strerror(errno));
			printf("read() from inotify fd returned %d!", numRead);
			return 1;
		}

		for (p = buf; p < buf + numRead; ) {
			event = (struct inotify_event *) p;

			if(!strcmp(signal,"exit") && i == 4)
				goto end;
			if((event->mask & IN_CLOSE_WRITE) && !strcmp(event->name, "return")){
				fp = fopen("return", "r");
				printf("Recv : ");
				while((ch = fgetc(fp)) != '\n')
					putchar(ch);
				printf("\n");
				fclose(fp);
				system("rm -f return");
				
				fp = fopen("message", "w");
				i = 0;
				printf("Send : ");
				while((ch = getchar()) != '\n'){
					fputc(ch, fp);
					putchar(ch);
					if(i<4)
						signal[i]=ch;
					i++;
				}
				fputc('\n', fp);
				putchar('\n');
				fclose(fp);
			}

			p += sizeof(struct inotify_event) + event->len;
		}
	}

end:
	close(inotifyFd);
	close(wd);
	exit(EXIT_SUCCESS);
}
