#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdlib.h>

#include <ab_os_interface.h>


//number of child thread
#define NUM_THREADS 20


static void suicide()
{
	//let me be killed by a signal
	unsigned long x = 0;
	*((int *) x) = 1;
}

static void child_func(unsigned long addr)
{
	if (absys_thread_control(AB_SET_ME_SPECIAL)) {
		printf("[PID %lu] set special thread failed.\n", 
		       (unsigned long)getpid());
		suicide();
	}
	//wait parent 
	sleep(10);
	printf("doing some work...\n");
	sleep(1);
	//printf("work done\n");

	//loop
	for(;;);
}


extern int mapping_test()
{
	int i, status;
	int count = 0;
	int normal = 0;
	pid_t pid[NUM_THREADS], wpid;
	unsigned long addr;
	unsigned long addr_to_map;
	void *ret;

	if (absys_thread_control(AB_SET_ME_ARBITER)) {
		printf("set arbiter failed!\n");
		goto test_failed;
	}
     
	addr = sbrk(0) + 20*4096;

	for (i = 0; i < NUM_THREADS; ++i) {
		pid[i] = fork();
		if (pid[i] < 0) {
			perror("thread cannot be created.\n");
			exit(0);
		}
		if(pid[i] == 0) {
			printf("child process %i created, pid = %lu.\n", i, (unsigned long)getpid());
			child_func(addr);
			exit(0);
		}		
	}
	//wait some time for all the child to start
	sleep(2);

	
	printf("mapping to address %lx.\n", addr);
	brk(addr);
	printf("touch the memory.\n");
	i = * (int *)(addr - 32);
	
	addr_to_map = (addr - 32) & 0xfffff000;


	//pick one child
	ret = (void *)absys_mmap(pid[5], (void *) addr_to_map, 4096, PROT_READ, MAP_ANONYMOUS|MAP_SHARED|MAP_FIXED, -1, 0);
	printf("absys_mmap returns %lx.\n", ret);

	while(1) {
		wpid = waitpid(-1, &status, 0);
		if(wpid <= 0)
			break;
		count++;
		if(WIFEXITED(status)) {
			printf("child %lu exited normally. count = %d\n", 
			       wpid, count);
			normal++;
		}
		if(WIFSIGNALED(status)) {
			printf("child %lu terminated by a signal. count = %d\n",
			       wpid, count);
		}
		else
			printf("wait pid returns with status %d\n", status);
	}
	
	
	if (normal == NUM_THREADS) {
		printf("mapping test successful.\n");
		return 0;
	}


test_failed:
	printf("test failed!!\n");
	return -1;			  
}
