/* Link-time controls only; never ship test switches in the helper. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int fp_jobs_main(void),fp_transfer_main(int),fp_guard_main(void);
int __real_fsync(int);
int __wrap_fsync(int fd)
{
    char proc[64],path[65536];snprintf(proc,sizeof proc,"/proc/self/fd/%d",fd);
    ssize_t n=readlink(proc,path,sizeof path-1);if(n<0)_exit(91);path[n]=0;
    const char *match=getenv("FP_ANCESTRY_MATCH"),*value=getenv("FP_ANCESTRY_ERRNO");
    static unsigned hits;int hit=match&&!fnmatch(match,path,FNM_PATHNAME),error=0;
    if(hit&&++hits==1&&value)error=atoi(value);
    if(!error&&__real_fsync(fd))error=errno;
    const char *log=getenv("FP_ANCESTRY_LOG");
    if(log){FILE *out=fopen(log,"a");if(!out)_exit(92);fprintf(out,"%d\t%s\n",error,path);if(fclose(out))_exit(93);}
    const char *move=getenv("FP_ANCESTRY_MOVE"),*to=getenv("FP_ANCESTRY_TO");
    if(hit&&hits==1&&move&&to&&rename(move,to))_exit(94);
    if(hit&&hits==1&&getenv("FP_ANCESTRY_STOP"))raise(SIGSTOP);
    errno=error;return error?-1:0;
}
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    if(!strcmp(argv[1],"--jobs"))return fp_jobs_main();
    if(!strcmp(argv[1],"--import"))return fp_transfer_main(1);
    if(!strcmp(argv[1],"--export"))return fp_transfer_main(0);
    if(!strcmp(argv[1],"--guard"))return fp_guard_main();
    return 2;
}
