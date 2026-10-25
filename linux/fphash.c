/* Isolated read-only SHA-256. Never read a FIFO, device or leaf symlink. */
#define _GNU_SOURCE
#include <sys/stat.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include "wslproto.h"
#include "fpsha256.h"
_Static_assert(sizeof(wslp_hash)==56,"hash wire");
static int io(int fd,void *p,size_t n,int out){char *s=p;while(n){ssize_t k=out?write(fd,s,n):read(fd,s,n);if(k<0&&errno==EINTR)continue;if(k<=0)return 0;s+=k;n-=k;}return 1;}
static uint64_t clock_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static int send_hash(wslp_hash *r){wslp_head h={WSLP_HASH_RESULT,sizeof *r,1};return io(1,&h,sizeof h,1)&&io(1,r,sizeof *r,1);}
static int stopped(void){struct pollfd f={0,POLLIN,0};return poll(&f,1,0)>0;}
int fp_hash_main(void)
{
    signal(SIGPIPE,SIG_IGN);wslp_hello hi={WSLP_MAGIC,WSLP_VERSION,getuid(),getpid()};wslp_head h={WSLP_HELLO,sizeof hi,0};
    if(!io(1,&h,sizeof h,1)||!io(1,&hi,sizeof hi,1)||!io(0,&h,sizeof h,0)||h.type!=WSLP_HASH||h.id!=1||!h.size||h.size>16384)return 1;
    char path[16385];if(!io(0,path,h.size,0)||memchr(path,0,h.size))return 1;path[h.size]=0;
    wslp_hash r={0};int fd=-1;struct stat before,after,current;uint8_t buf[65536];fp_sha256 sha;fp_sha_init(&sha);uint64_t last=0;
    if(path[0]!='/')r.err=EINVAL;
    else if(stopped())r.err=ECANCELED;
    else if((fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC))<0)r.err=errno;
    else if(fstat(fd,&before))r.err=errno;
    else if(!S_ISREG(before.st_mode))r.err=EINVAL;
    while(fd>=0&&!r.err){
        if(stopped()){r.err=ECANCELED;break;}
        uint64_t t=clock_ms();if(t-last>=50){last=t;if(!send_hash(&r)){close(fd);return 1;}}
        ssize_t n=read(fd,buf,sizeof buf);if(n<0&&errno==EINTR)continue;if(n<0){r.err=errno;break;}if(!n)break;
        fp_sha_update(&sha,buf,(size_t)n);r.size+=(uint64_t)n;
    }
    if(fd>=0&&!r.err){
        if(fstat(fd,&after)||lstat(path,&current))r.err=errno;
        else if(before.st_dev!=current.st_dev||before.st_ino!=current.st_ino||!S_ISREG(current.st_mode)||
            before.st_size!=after.st_size||r.size!=(uint64_t)after.st_size||
            before.st_mtim.tv_sec!=after.st_mtim.tv_sec||before.st_mtim.tv_nsec!=after.st_mtim.tv_nsec||
            before.st_ctim.tv_sec!=after.st_ctim.tv_sec||before.st_ctim.tv_nsec!=after.st_ctim.tv_nsec)r.err=ESTALE;
        else{fp_sha_final(&sha,r.sha256);r.mtime=(int64_t)(after.st_mtim.tv_sec+11644473600LL)*10000000+after.st_mtim.tv_nsec/100;}
    }
    if(fd>=0)close(fd);if(stopped())r.err=ECANCELED;
    if(r.err){memset(r.sha256,0,32);r.mtime=0;}r.done=1;return send_hash(&r)?0:1;
}
