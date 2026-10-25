/* Private crash fixture only: the production helper has no kill switches.
 * Link the actual writers and stop without running any cleanup/atexit code. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <fnmatch.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int fp_jobs_main(void);
int fp_transfer_main(int);
int __real_fsync(int);
ssize_t __real_write(int,const void *,size_t);
int __real_unlinkat(int,const char *,int);
static int match(int fd,const char *key)
{
    const char *pattern=getenv(key);if(!pattern)return 0;
    char proc[64],path[65536];snprintf(proc,sizeof proc,"/proc/self/fd/%d",fd);
    ssize_t n=readlink(proc,path,sizeof path-1);if(n<0)return 0;path[n]=0;
    return !fnmatch(pattern,path,FNM_PATHNAME);
}
static void crash(void){kill(getpid(),SIGKILL);_exit(99);}
int __wrap_unlinkat(int fd,const char *name,int flags)
{
    if(match(fd,"FP_CRASH_UNLINK"))crash();
    return __real_unlinkat(fd,name,flags);
}
int __wrap_fsync(int fd)
{
    static unsigned count;const char *nth=getenv("FP_CRASH_NTH");
    if(match(fd,"FP_CRASH_SYNC")&&++count==(unsigned)(nth?atoi(nth):1))crash();
    return __real_fsync(fd);
}
ssize_t __wrap_write(int fd,const void *bytes,size_t n)
{
    if(match(fd,"FP_CRASH_WRITE")&&n>1){
        ssize_t written=__real_write(fd,bytes,n/2);
        if(written!=(ssize_t)(n/2))_exit(98);
        crash();
    }
    return __real_write(fd,bytes,n);
}
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    if(!strcmp(argv[1],"--jobs"))return fp_jobs_main();
    if(!strcmp(argv[1],"--import"))return fp_transfer_main(1);
    if(!strcmp(argv[1],"--export"))return fp_transfer_main(0);
    return 2;
}
