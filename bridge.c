#define _GNU_SOURCE
#include<string.h>
#include<stdio.h>
#include<stdlib.h>
#include<sched.h>
#include<unistd.h>
#include<fcntl.h>

#define N_NS 3

char *ns_substr[N_NS] = {"net", "mnt", "ipc"};
int ns[N_NS] = {CLONE_NEWNET, CLONE_NEWNS, CLONE_NEWIPC};

int main(int argc, char *argv[])
{
	if(argc != 3){
		printf("usage: %s <client namespace> <server namespace>\n", argv[0]);
		printf("\t %s /proc/5566/ns/ipc /proc/5566/ns/mnt \n", argv[0]);
		return 1;
	}

	while(1){
		if(setns(open(argv[1], O_RDONLY), ns[i])){
			printf("setns client fails\n");
			return 1;
		}
		
		if(setns(open(argv[2], O_RDONLY), ns[i])){
        		printf("setns server fails\n");
                	return 1;
        	}
	}
	return 0;
}
