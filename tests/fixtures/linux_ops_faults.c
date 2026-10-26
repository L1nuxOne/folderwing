/* Link-time fixture only. Never compiled into the normal helper. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int __real_fsync(int fd);
int __wrap_fsync(int fd)
{
    char alias[64], path[65536]; snprintf(alias,sizeof alias,"/proc/self/fd/%d",fd);
    ssize_t size=readlink(alias,path,sizeof path-1); if(size<0)path[0]=0;else path[size]=0;
    const char *exact=getenv("FW_OPS_FAIL_PATH"),*contains=getenv("FW_OPS_FAIL_CONTAINS");
    static unsigned hits;
    int match=(exact&&!strcmp(exact,path))||(contains&&strstr(path,contains));
    const char *nth=getenv("FW_OPS_FAIL_NTH"); unsigned target=nth?(unsigned)strtoul(nth,NULL,10):1;
    const char *pause=getenv("FW_OPS_PAUSE_CONTAINS"),*marker=getenv("FW_OPS_MARKER"),*release=getenv("FW_OPS_RELEASE");
    if(pause&&strstr(path,pause)&&marker&&release){
        int m=open(marker,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
        if(m>=0){close(m);for(unsigned i=0;i<1000&&access(release,F_OK);i++)usleep(10000);}
    }
    int rc;
    if(match&&++hits==target){errno=EIO;rc=-1;}else rc=__real_fsync(fd);
    int error=rc?errno:0;const char *log=getenv("FW_OPS_LOG");
    if(log){int out=open(log,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0600);if(out>=0){dprintf(out,"%d\t%s\n",error,path);close(out);}}
    errno=error;return rc;
}
