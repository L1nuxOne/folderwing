/* Persist the existing namespace above an already-open destination once,
 * before any copy/move mutation or importer acceptance. Never reopen a path,
 * cross a mount boundary (including bind mounts), or sync unrelated volumes. */
#ifndef FP_ANCESTRY_H
#define FP_ANCESTRY_H
#include "fpmount.h"
static uint64_t fp_ancestry_mount_id(int fd)
{
#ifdef SYS_statx
    /* Stable statx v0 UAPI layout; do not require recent distro libc headers.
     * Mount ID is at byte144 in the 256-byte, eight-byte-aligned result. Modern
     * kernels can identify a held FD without depending on a mounted /proc. */
    uint64_t data[32]={0},mount=0;uint32_t mask=0;
    if(!syscall(SYS_statx,fd,"",0x1000u|0x100u,0x1000u,data)){
        memcpy(&mask,data,sizeof mask);memcpy(&mount,(char *)data+144,sizeof mount);
        if((mask&0x1000u)&&mount)return mount;
    }
#endif
    /* Linux3.15+ fallback when statx mount IDs are unavailable. */
    return fp_mount_id(fd);
}
static int fp_ancestry_sync(int destination,int (*cancelled)(void))
{
    int current=fcntl(destination,F_DUPFD_CLOEXEC,0);
    if(current<0)return errno;
    uint64_t mount=fp_ancestry_mount_id(current);int error=mount?0:ENOTSUP;
    for(unsigned depth=0;!error;depth++){
        if(cancelled&&cancelled()){error=ECANCELED;break;}
        if(depth==256){error=ENAMETOOLONG;break;}
        struct stat child,up;
        if(fstat(current,&child)){error=errno;break;}
        int parent=openat(current,"..",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
        if(parent<0){error=errno;break;}
        if(fstat(parent,&up))error=errno;
        uint64_t parent_mount=error?0:fp_ancestry_mount_id(parent);
        if(!error&&!parent_mount)error=ENOTSUP;
        int boundary=!error&&(parent_mount!=mount||
            (child.st_dev==up.st_dev&&child.st_ino==up.st_ino));
        if(!error&&!boundary){
            int rc;
            do{rc=fsync(parent);}while(rc<0&&errno==EINTR&&!(cancelled&&cancelled()));
            if(rc<0)error=errno==EINTR&&cancelled&&cancelled()?ECANCELED:errno;
            if(!error&&cancelled&&cancelled())error=ECANCELED;
            /* A blocking barrier must not bless a parent relationship that
             * changed while it ran. This remains a POSIX check/use boundary,
             * not a lock against later same-UID/root namespace mutations. */
            if(!error){
                int check=openat(current,"..",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
                struct stat now;
                if(check<0)error=errno;
                else {
                    if(fstat(check,&now))error=errno;
                    else if(now.st_dev!=up.st_dev||now.st_ino!=up.st_ino||fp_ancestry_mount_id(check)!=mount)error=ESTALE;
                    close(check);
                }
            }
        }
        close(current);current=parent;
        if(boundary)break;
    }
    close(current);return error;
}
#endif
