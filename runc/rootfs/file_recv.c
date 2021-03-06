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

	FILE *fp;
	char ch;
	char input[1024];
	char signal[4];
	int i, flag;

	while(1) {     
		wd = inotify_add_watch(inotifyFd, getcwd(NULL, 0), IN_CLOSE_WRITE);
		if (wd == -1) {
			perror(strerror(errno));
			printf("inotify_add_watch\n");
			return 1;
		}

		flag = 0;
		while(1){
			numRead = read(inotifyFd, buf, BUF_LEN);
			if (numRead <= 0) {
				perror(strerror(errno));
				printf("read() from inotify fd returned %d!", numRead);
				return 1;
			}

			for (p = buf; p < buf + numRead; ) {
				event = (struct inotify_event *) p;

				if((event->mask & IN_CLOSE_WRITE) && !strcmp(event->name, "message")){
					fp = fopen("message", "r");
					i = 0;
					memset(input, '\0', sizeof(input)); 
					printf("Recv : ");
					while((ch = fgetc(fp)) != '\n'){
						putchar(ch);
						input[i]=ch;
						if(i<4)
							signal[i]=ch;
						i++;
					}
					printf("\n");
					fclose(fp);
					system("rm -f message");
					flag = 1;
					if(!strcmp(signal,"exit") && i == 4)
						goto end;
				}

				p += sizeof(struct inotify_event) + event->len;
			}
			if(flag == 1){
				break;
			}
		}

		wd = inotify_add_watch(inotifyFd, getcwd(NULL, 0), IN_DELETE);
		if (wd == -1) {
			perror(strerror(errno));
			printf("inotify_add_watch\n");
			return 1;
		}

		fp = fopen("message", "w");
		printf("Send : %s\n", input);
		fputs(input, fp);
		fputc('\n', fp);
		fclose(fp);

		flag = 0;
		while(1){
			numRead = read(inotifyFd, buf, BUF_LEN);
			if (numRead <= 0) {
				perror(strerror(errno));
				printf("read() from inotify fd returned %d!", numRead);
				return 1;
			}

			for (p = buf; p < buf + numRead; ) {
				event = (struct inotify_event *) p;

				if((event->mask & IN_DELETE) && !strcmp(event->name, "message"))
					flag = 1;

				p += sizeof(struct inotify_event) + event->len;
			}
			if(flag == 1){
				break;
			}
		}
	}

end:
	close(wd);
	close(inotifyFd);
	exit(EXIT_SUCCESS);
}
