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
#include <time.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/wait.h>


#include"msg_helper.h"

#define BUF_LEN (10 * (sizeof(struct inotify_event) + NAME_MAX + 1))

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
	int i,flag;

	int msgqid, rc;
	struct msg_buf msg;

	if(setns(open(argv[2], O_RDONLY), CLONE_NEWIPC)){
                printf("setns ipc fails\n");
                return 1;
        }
	
	if(setns(open(argv[1], O_RDONLY), CLONE_NEWNS)){
		printf("setns mnt fails\n");
		return 1;
	}

	inotifyFd = inotify_init();                 
	if (inotifyFd == -1) {
		perror(strerror(errno));
		printf("inotifyFd\n");
		return 1;
	}

	msgqid = msgget(MAGIC, 0);
	if (msgqid < 0) {
		perror(strerror(errno));
		printf("failed to create message queue with msgqid = %d\n", msgqid);
		return 1;
	}

	
	while(1){
		wd = inotify_add_watch(inotifyFd, getcwd(NULL, 0), IN_CLOSE_WRITE);
		if (wd == -1) {
			perror(strerror(errno));
			printf("inotify_add_watch\n");
			return 1;
		}

		flag = 0;
		while(1) {
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
						i++;
					}
					printf("\n");
					fclose(fp);
					system("rm -f message");
					flag = 1;
				}
				
				p += sizeof(struct inotify_event) + event->len;
			}
			if(flag == 1){
				break;
			}
		}
		
		msg.mtype = 1;
		memset(msg.mtext, '\0', sizeof(msg.mtext));
		strncpy(msg.mtext, input, i);
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

		wd = inotify_add_watch(inotifyFd, getcwd(NULL, 0), IN_DELETE);
		if (wd == -1) {
			perror(strerror(errno));
			printf("inotify_add_watch\n");
			return 1;
		}

		fp = fopen("message", "w");
		fputs(msg.mtext, fp);
		fputc('\n', fp);
		fclose(fp);
		printf("Bridge send to mnt : %s\n",msg.mtext);
		
		flag = 0; 
		while(1) {                                 
			numRead = read(inotifyFd, buf, BUF_LEN);
			if (numRead <= 0) {
				perror(strerror(errno));
				printf("read() from inotify fd returned %ld!", numRead);
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
	return 0;
}

