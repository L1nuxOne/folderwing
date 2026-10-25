/* Read-only descriptor-relative size traversal; never follows a link. */
#define _GNU_SOURCE
#include <sys/stat.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include "wslproto.h"
_Static_assert(sizeof(wslp_size)==48,"size wire layout");
typedef struct scan {wslp_size r;uint64_t notified;int stopped;dev_t device;} scan;
static int io(int fd,void *data,size_t n,int output)
{
    char *p=data;while(n){ssize_t k=output?write(fd,p,n):read(fd,p,n);if(k<0&&errno==EINTR)continue;if(k<=0)return 0;p+=k;n-=k;}return 1;
}
static uint64_t now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static int send_result(scan *s)
{
    wslp_head h={WSLP_SIZE_RESULT,sizeof s->r,1};return io(1,&h,sizeof h,1)&&io(1,&s->r,sizeof s->r,1);
}
static void failed(scan *s,int error){s->r.errors++;if(!s->r.first_error)s->r.first_error=error?error:EIO;}
static int pulse(scan *s)
{
    struct pollfd f={0,POLLIN,0};
    if(poll(&f,1,0)>0){
        /* Any input after the request is a stop; malformed input cannot make
         * traversal continue or block the worker waiting for a partial frame. */
        if(f.revents&(POLLIN|POLLHUP|POLLERR|POLLNVAL))s->stopped=1;
    }
    uint64_t t=now();if(!s->stopped&&t-s->notified>=50){s->notified=t;if(!send_result(s))s->stopped=1;}
    return !s->stopped;
}
static void walk(scan *s,int fd,unsigned depth)
{
    struct stat st;if(fstat(fd,&st)){failed(s,errno);close(fd);return;}
    if(st.st_dev!=s->device){s->r.skipped++;close(fd);return;}
    s->r.folders++;
    if(depth>=128){failed(s,EOVERFLOW);close(fd);return;}
    DIR *d=fdopendir(fd);if(!d){failed(s,errno);close(fd);return;}
    while(pulse(s)){
        errno=0;struct dirent *e=readdir(d);if(!e){if(errno)failed(s,errno);break;}
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        if(fstatat(fd,e->d_name,&st,AT_SYMLINK_NOFOLLOW)){failed(s,errno);continue;}
        if(S_ISDIR(st.st_mode)){
            int child=openat(fd,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
            if(child<0)failed(s,errno);else walk(s,child,depth+1);
        }else if(S_ISREG(st.st_mode)){
            if(st.st_size<0||UINT64_MAX-s->r.bytes<(uint64_t)st.st_size)failed(s,EOVERFLOW);
            else{s->r.files++;s->r.bytes+=(uint64_t)st.st_size;}
        }else s->r.skipped++;
    }
    closedir(d);
}
int fp_sizes_main(void)
{
    signal(SIGPIPE,SIG_IGN);wslp_hello hi={WSLP_MAGIC,WSLP_VERSION,getuid(),getpid()};wslp_head h={WSLP_HELLO,sizeof hi,0};
    if(!io(1,&h,sizeof h,1)||!io(1,&hi,sizeof hi,1)||!io(0,&h,sizeof h,0)||h.type!=WSLP_SIZE||h.id!=1||!h.size||h.size>16384)return 1;
    char path[16385];if(!io(0,path,h.size,0)||memchr(path,0,h.size))return 1;path[h.size]=0;
    while(h.size>1&&path[h.size-1]=='/')path[--h.size]=0;
    scan s={0};int fd=-1;struct stat st;
    if(path[0]!='/')failed(&s,EINVAL);
    else if(!pulse(&s)){}
    else if((fd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK))<0)failed(&s,errno);
    else if(fstat(fd,&st)){failed(&s,errno);close(fd);}
    else{s.device=st.st_dev;walk(&s,fd,0);}
    pulse(&s);s.r.state=s.stopped?3:s.r.errors?2:1;return send_result(&s)?0:1;
}
