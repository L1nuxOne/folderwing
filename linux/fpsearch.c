/* Descriptor-relative streaming filename traversal. One process per search. */
#define _GNU_SOURCE
#include <sys/stat.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include "wslproto.h"
typedef struct scan {wslp_search_end r;dev_t device;uint64_t mount;char path[16385];int stopped;} scan;
static uint64_t mount_id(int fd)
{
    /* Linux 3.15+ exposes mount identity here, including same-device bind
     * mounts; this also works with libc headers predating STATX_MNT_ID. */
    char path[64],line[256];snprintf(path,sizeof path,"/proc/self/fdinfo/%d",fd);FILE *f=fopen(path,"r");
    if(!f)return 0;unsigned long long id=0;while(fgets(line,sizeof line,f))if(sscanf(line,"mnt_id: %llu",&id)==1)break;fclose(f);return (uint64_t)id;
}
static int io(int fd,void *data,size_t n,int output){char *p=data;while(n){ssize_t k=output?write(fd,p,n):read(fd,p,n);if(k<0&&errno==EINTR)continue;if(k<=0)return 0;p+=k;n-=k;}return 1;}
static int pulse(scan *s){struct pollfd p={0,POLLIN,0};if(poll(&p,1,0)>0&&p.revents)s->stopped=1;return !s->stopped;}
static void failed(scan *s,int err){s->r.errors++;if(!s->r.error)s->r.error=err;}
static void walk(scan *s,int fd,size_t length,unsigned depth)
{
    struct stat st;if(fstat(fd,&st)){failed(s,errno);close(fd);return;}
    uint64_t mount=mount_id(fd);if(!mount){failed(s,ENOTSUP);close(fd);return;}
    if(st.st_dev!=s->device||mount!=s->mount){s->r.skipped++;close(fd);return;}
    DIR *d=fdopendir(fd);if(!d){failed(s,errno);close(fd);return;}
    while(pulse(s)){
        errno=0;struct dirent *e=readdir(d);if(!e){if(errno)failed(s,errno);break;}
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        if(s->r.scanned==100000){failed(s,EOVERFLOW);break;}s->r.scanned++;
        if(fstatat(fd,e->d_name,&st,AT_SYMLINK_NOFOLLOW)){failed(s,errno);continue;}
        size_t n=strlen(e->d_name),sep=length!=0;if(length+sep+n>16384){failed(s,ENAMETOOLONG);continue;}
        if(sep)s->path[length]='/';memcpy(s->path+length+sep,e->d_name,n+1);
        size_t size=length+sep+n;
        wslp_search_entry item={S_ISREG(st.st_mode)&&st.st_size>=0?(uint64_t)st.st_size:0,
            (int64_t)st.st_mtim.tv_sec*10000000+st.st_mtim.tv_nsec/100+116444736000000000LL,
            S_ISDIR(st.st_mode)?0x10u:S_ISREG(st.st_mode)?0x80u:0x400u,(uint32_t)size};
        wslp_head head={WSLP_SEARCH_ENTRY,(uint32_t)sizeof item+(uint32_t)size,1};
        if(!io(1,&head,sizeof head,1)||!io(1,&item,sizeof item,1)||!io(1,s->path,size,1)){s->stopped=1;break;}
        if(S_ISDIR(st.st_mode)){
            if(depth>=127)failed(s,EOVERFLOW);
            else{int child=openat(fd,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
                if(child<0)failed(s,errno);else walk(s,child,size,depth+1);}
        }else if(!S_ISREG(st.st_mode))s->r.skipped++;
        s->path[length]=0;if(s->r.scanned==100000&&s->r.error==EOVERFLOW)break;
    }closedir(d);
}
int fp_search_main(void)
{
    signal(SIGPIPE,SIG_IGN);wslp_hello hi={WSLP_MAGIC,WSLP_VERSION,getuid(),getpid()};wslp_head h={WSLP_HELLO,sizeof hi,0};
    if(!io(1,&h,sizeof h,1)||!io(1,&hi,sizeof hi,1)||!io(0,&h,sizeof h,0)||h.type!=WSLP_SEARCH||h.id!=1||!h.size||h.size>16384)return 1;
    char path[16385];if(!io(0,path,h.size,0)||memchr(path,0,h.size))return 1;path[h.size]=0;
    while(h.size>1&&path[h.size-1]=='/')path[--h.size]=0;
    scan s={0};int fd;struct stat st;
    if(path[0]!='/')failed(&s,EINVAL);
    else if(!pulse(&s)){}
    else if((fd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK))<0)failed(&s,errno);
    else if(fstat(fd,&st)){failed(&s,errno);close(fd);}else{s.device=st.st_dev;s.mount=mount_id(fd);walk(&s,fd,0,0);}
    if(s.stopped&&!s.r.error)failed(&s,ECANCELED);
    h=(wslp_head){WSLP_SEARCH_END,sizeof s.r,1};return io(1,&h,sizeof h,1)&&io(1,&s.r,sizeof s.r,1)?0:1;
}
