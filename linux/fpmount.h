/* Open a visited leaf without crossing a mount, including a same-device bind
 * mount. Callers retain directory FDs throughout traversal. Explicit endpoint
 * parents are opened separately: browsing inside a mounted volume is allowed.
 * Never use this read-only helper with creation or truncation flags. */
#ifndef FP_MOUNT_H
#define FP_MOUNT_H
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static uint64_t fp_mount_id(int fd)
{
    /* Available since Linux 3.15, also with libc headers predating statx. */
    char path[64],line[256];
    snprintf(path,sizeof path,"/proc/self/fdinfo/%d",fd);
    FILE *f=fopen(path,"r");if(!f)return 0;
    unsigned long long id=0;
    while(fgets(line,sizeof line,f))if(sscanf(line,"mnt_id: %llu",&id)==1)break;
    fclose(f);return (uint64_t)id;
}
static int fp_mount_open(int parent,const char *name,int flags)
{
    /* All callers pass a single literal component. No filesystem writes may
     * precede the fallback's descriptor check. */
    if(!name||!name[0]||strchr(name,'/')||!strcmp(name,".")||!strcmp(name,"..")||
       /* musl includes O_PATH/O_SEARCH in O_ACCMODE; reject only write access. */
       (flags&(O_CREAT|O_TRUNC|O_WRONLY|O_RDWR))){errno=EINVAL;return -1;}
    flags|=O_NOFOLLOW|O_CLOEXEC;
    if(!(flags&O_PATH))flags|=O_NONBLOCK;
#ifdef SYS_openat2
    static int unavailable;
    if(!unavailable){
        /* Linux UAPI open_how v0; avoid requiring newer distro libc headers. */
        struct {uint64_t flags,mode,resolve;} how={(uint64_t)flags,0,0x01u|0x02u};
        int fd=(int)syscall(SYS_openat2,parent,name,&how,sizeof how);
        if(fd>=0)return fd;
        if(errno!=ENOSYS&&errno!=EINVAL)return -1;
        unavailable=1;
    }
#endif
    int fd=openat(parent,name,flags);if(fd<0)return -1;
    struct stat a,b;int err=0;
    if(fstat(parent,&a)||fstat(fd,&b))err=errno;
    else if(a.st_dev!=b.st_dev)err=EXDEV;
    else {
        uint64_t source=fp_mount_id(parent),child=fp_mount_id(fd);
        if(!source||!child)err=ENOTSUP;
        else if(source!=child)err=EXDEV;
    }
    if(err){close(fd);errno=err;return -1;}return fd;
}
static int fp_mount_check(int parent,const char *name)
{
    int fd=fp_mount_open(parent,name,O_PATH);
    if(fd<0)return errno;close(fd);return 0;
}
#endif
