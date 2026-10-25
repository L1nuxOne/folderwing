/* Link-time faults for the production pinned-directory helper, never shipped. */
#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
int fp_guard_main(void);
int __real_fsync(int fd);
int __wrap_fsync(int fd)
{
    static unsigned calls;calls++;char link[64],path[16385];snprintf(link,sizeof link,"/proc/self/fd/%d",fd);
    ssize_t n=readlink(link,path,sizeof path-1);if(n<0)n=0;path[n]=0;
    const char *nth=getenv("FP_TEST_SYNC_NTH"),*value=getenv("FP_TEST_SYNC_ERRNO"),*log=getenv("FP_TEST_SYNC_LOG");
    int rc,error=0;if(nth&&value&&calls==strtoul(nth,NULL,10)){rc=-1;error=atoi(value);}else{rc=__real_fsync(fd);if(rc)error=errno;}
    if(log){FILE *out=fopen(log,"a");if(out){fprintf(out,"%u %d %s\n",calls,error,path);fclose(out);}}
    errno=error;return rc;
}
/* Deny only mount-identity observations. /proc/<pid>/fd anchors stay usable,
 * so a failure proves unsupported mount identity rather than a broken pin. */
FILE *__real_fopen(const char *path,const char *mode);
FILE *__wrap_fopen(const char *path,const char *mode)
{
    if(getenv("FP_TEST_NO_MOUNT_ID")&&!strncmp(path,"/proc/self/fdinfo/",18)){
        errno=EACCES;return NULL;
    }
    return __real_fopen(path,mode);
}
int main(void){return fp_guard_main();}
