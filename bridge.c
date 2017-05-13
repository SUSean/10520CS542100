#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <limits.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#define N_NS 3

int ns[N_NS] = {CLONE_NEWNET, CLONE_NEWNS, CLONE_NEWIPC};

int main(int argc, char *argv[])
{
	if(argc != 3){
		printf("usage: %s <client namespace> <server namespace>\n", argv[0]);
		printf("\t %s /proc/5566/ns/mnt /proc/5566/ns/ipc \n", argv[0]);
		return 1;
	}

	int inotifyFd, wd, j;
	char buf[BUF_LEN] __attribute__ ((aligned(8)));
	ssize_t numRead;
	char *p;
	struct inotify_event *event;

	FILE *fp;
	char ch;
	char input[1024];
	char signal[4];
	int i;

	int msgqid, rc;
	struct msg_buf msg;

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

	while(1){
		if(setns(open(argv[1], O_RDONLY), ns[1])){
			printf("setns mnt fails\n");
			return 1;
		}

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
				printf("Bridge recv from mnt : ");
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
				if(!strcmp(signal,"exit") && i == 4){
					close(wd);
					close(inotifyFd);
					break;
				}
			}

			p += sizeof(struct inotify_event) + event->len;
		}

		if(setns(open(argv[2], O_RDONLY), ns[2])){
			printf("setns ipc fails\n");
			return 1;
		}

		msg.mtype = 1;
		sscanf(input, "%s", msg.mtext);
		printf("Bridge send to ipc : %s\n",msg.mtext);
		rc = msgsnd(msgqid, &msg, sizeof(msg.mtext), 0);
		if (rc < 0) {
			perror( strerror(errno) );
			printf("msgsnd failed, rc = %d\n", rc);
			return 1;
		}
		if(!strcmp(msg.mtext,"exit"))
				break;

		rc = msgrcv(msgqid, &msg, sizeof(msg.mtext), 0, 0); 
		if (rc < 0) {
			perror( strerror(errno) );
			printf("msgrcv failed, rc=%d\n", rc);
			return 1;
		} 
		printf("Bridge recv from ipc : %s\n",msg.mtext);

		if(setns(open(argv[1], O_RDONLY), ns[1])){
			printf("setns mnt fails\n");
			return 1;
		}
		fp = fopen("return", "w");
		printf("Send : ");
		for(i = 0; i < strlen(msg.mtext); i++){
			fputc(msg.mtext[i], fp);
			putchar(msg.mtext[i]);
		}
		fputc('\n', fp);
		putchar('\n');
		fclose(fp);
	}
	return 0;
}
