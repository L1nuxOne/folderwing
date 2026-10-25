/* Link-time metadata faults and observations, never part of the shipped helper. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
int fp_jobs_main(void);int fp_transfer_main(int);
ssize_t __real_flistxattr(int,char *,size_t);
ssize_t __real_fgetxattr(int,const char *,void *,size_t);
int __real_fsetxattr(int,const char *,const void *,size_t,int);
int __real_fremovexattr(int,const char *);
int __real_fsync(int);
static int path_of(int fd,char *path){char proc[64];snprintf(proc,sizeof proc,"/proc/self/fd/%d",fd);ssize_t n=readlink(proc,path,65535);if(n<0)return 0;path[n]=0;return 1;}
static int fault(const char *kind,int fd,const char *attr)
{
    const char *which=getenv("FP_META_FAIL"),*pattern=getenv("FP_META_MATCH"),*named=getenv("FP_META_NAME");char path[65536];
    if(!which||strcmp(which,kind)||!pattern||!path_of(fd,path)||fnmatch(pattern,path,FNM_PATHNAME)||(named&&(!attr||strcmp(named,attr))))return 0;
    static unsigned count;const char *nth=getenv("FP_META_NTH");if(++count!=(unsigned)(nth?atoi(nth):1))return 0;
    const char *error=getenv("FP_META_ERRNO");errno=error?atoi(error):EACCES;return 1;
}
ssize_t __wrap_flistxattr(int fd,char *buf,size_t size){return fault("list",fd,NULL)?-1:__real_flistxattr(fd,buf,size);}
ssize_t __wrap_fgetxattr(int fd,const char *name,void *buf,size_t size){return fault("get",fd,name)?-1:__real_fgetxattr(fd,name,buf,size);}
int __wrap_fsetxattr(int fd,const char *name,const void *buf,size_t size,int flags)
{if(fault("set",fd,name))return -1;if(fault("drop",fd,name))return 0;return __real_fsetxattr(fd,name,buf,size,flags);}
int __wrap_fremovexattr(int fd,const char *name){return fault("remove",fd,name)?-1:__real_fremovexattr(fd,name);}
int __wrap_fsync(int fd)
{
    int rc=__real_fsync(fd);char path[65536];if(rc||!path_of(fd,path))return rc;
    const char *log=getenv("FP_META_SYNC_LOG"),*source=getenv("FP_META_SOURCE"),*target=getenv("FP_META_TARGET");
    if(log){
        int out=open(log,O_WRONLY|O_CREAT|O_APPEND,0600);if(out<0)_exit(101);
        char value[64];ssize_t n=__real_fgetxattr(fd,"user.openpilot.audit",value,sizeof value);struct stat st;int held=!fstat(fd,&st);
        char line[66000];int size=snprintf(line,sizeof line,"%d\t%d\t%d\t%zd\t%s\n",held&&S_ISREG(st.st_mode),source&&!access(source,F_OK),target&&!access(target,F_OK),n,path);
        if(write(out,line,(size_t)size)!=size)_exit(102);close(out);
    }
    static int mutated;const char *match=getenv("FP_META_MUTATE_MATCH"),*mode=getenv("FP_META_MUTATE");
    if(!mutated&&mode&&match&&!fnmatch(match,path,FNM_PATHNAME)){
        mutated=1;if(!source||setxattr(source,"user.openpilot.audit","late metadata",13,0))_exit(103);
    }
    return rc;
}
int main(int argc,char **argv)
{if(argc!=2)return 2;if(!strcmp(argv[1],"--jobs"))return fp_jobs_main();if(!strcmp(argv[1],"--import"))return fp_transfer_main(1);if(!strcmp(argv[1],"--export"))return fp_transfer_main(0);return 2;}
