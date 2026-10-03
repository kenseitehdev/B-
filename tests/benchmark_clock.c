#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#include <process.h>
static double now(void){LARGE_INTEGER t,f;QueryPerformanceCounter(&t);QueryPerformanceFrequency(&f);return (double)t.QuadPart/(double)f.QuadPart;}
static int run(const char *path){
    /* The benchmark programs take no arguments. Quote their path for the CRT. */
    size_t n=0;while(path[n])++n;char *quoted=malloc(n+3);if(!quoted)return 0;
    snprintf(quoted,n+3,"\"%s\"",path);const char *args[]={quoted,NULL};
    intptr_t result=_spawnv(_P_WAIT,path,args);free(quoted);return result==0;
}
#else
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
static double now(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))exit(1);return (double)t.tv_sec+(double)t.tv_nsec/1e9;}
static int run(const char *path){pid_t child=fork();if(child<0)return 0;if(!child){execl(path,path,(char *)NULL);_exit(127);}int status;while(waitpid(child,&status,0)<0){if(errno!=EINTR)return 0;}return WIFEXITED(status)&&WEXITSTATUS(status)==0;}
#endif
int main(int argc,char **argv){if(argc!=3)return 2;unsigned n=(unsigned)strtoul(argv[1],NULL,10);if(!n||n>10000)return 2;
    if(!run(argv[2]))return 1;
    double start=now();for(unsigned j=0;j<n;++j)if(!run(argv[2]))return 1;
    printf("%.6f\n",(now()-start)*1000/n);return 0;
}
