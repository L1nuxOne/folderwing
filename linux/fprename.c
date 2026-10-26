/* One bounded native rename draft, one pinned parent directory, no shell. */
#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "wslproto.h"
#include "fpancestry.h"
#define MAX_ITEMS 256
#define R_PENDING 0
#define R_READY 1
#define R_UNCHANGED 2
#define R_RENAMED 3
#define R_ERROR 4
#define R_STOPPED 5
#define R_BLOCKED 6
#define R_UNCERTAIN 7
_Static_assert(sizeof(wslp_rename_item)==40,"rename request layout");
_Static_assert(sizeof(wslp_rename_result)==40,"rename result layout");
typedef struct item {char from[256],to[256];wslp_rename_result r;wslp_rename_identity expected;} item;
static int io(int fd,void *data,size_t n,int output)
{
    char *p=data;while(n){ssize_t k=output?write(fd,p,n):read(fd,p,n);if(k<0&&errno==EINTR)continue;if(k<=0)return 0;p+=k;n-=k;}return 1;
}
static int send_frame(uint32_t type,const void *data,uint32_t size)
{
    wslp_head h={type,size,1};return io(1,&h,sizeof h,1)&&io(1,(void*)data,size,1);
}
static int cancelled(void)
{
    struct pollfd p={0,POLLIN,0};return poll(&p,1,0)>0&&(p.revents&(POLLIN|POLLHUP|POLLERR|POLLNVAL));
}
static wslp_rename_identity identity(const struct stat *st)
{
    return (wslp_rename_identity){st->st_dev,st->st_ino,(uint64_t)st->st_ctim.tv_sec,st->st_ctim.tv_nsec,
        (S_ISDIR(st->st_mode)?0x10u:0)|(S_ISLNK(st->st_mode)?0x400u:0)};
}
static int name(const char *s,size_t n){return n&&n<=255&&!memchr(s,0,n)&&!memchr(s,'/',n)&&!(n==1&&s[0]=='.')&&!(n==2&&s[0]=='.'&&s[1]=='.');}
static int check(int parent,item *it,int apply)
{
    struct stat st;if(fstatat(parent,it->from,&st,AT_SYMLINK_NOFOLLOW))return errno;
    it->r.identity=identity(&st);if(apply&&memcmp(&it->r.identity,&it->expected,sizeof it->expected))return ESTALE;
    if(!strcmp(it->from,it->to))return 0;
    if(!fstatat(parent,it->to,&st,AT_SYMLINK_NOFOLLOW))return EEXIST;
    return errno==ENOENT?0:errno;
}
static void mark(item *items,uint32_t count,uint32_t state)
{
    for(uint32_t i=0;i<count;i++)if(items[i].r.state==R_PENDING||items[i].r.state==R_READY)items[i].r.state=state;
}
int fp_rename_main(void)
{
    signal(SIGPIPE,SIG_IGN);wslp_hello hi={WSLP_MAGIC,WSLP_VERSION,getuid(),getpid()};wslp_head h={WSLP_HELLO,sizeof hi,0};
    if(!io(1,&h,sizeof h,1)||!io(1,&hi,sizeof hi,1)||!io(0,&h,sizeof h,0)||h.type!=WSLP_RENAME_BATCH||h.id!=1||h.size<sizeof(wslp_rename_request)||h.size>WSLP_MAX)return 1;
    unsigned char *buf=malloc(h.size);if(!buf)return 1;if(!io(0,buf,h.size,0)){free(buf);return 1;}
    wslp_rename_request q;memcpy(&q,buf,sizeof q);size_t at=sizeof q;
    if(q.apply>1||!q.count||q.count>MAX_ITEMS||q.pad||!q.root_bytes||q.root_bytes>16384||q.root_bytes>h.size-at||memchr(buf+at,0,q.root_bytes)||buf[at]!='/'){free(buf);return 1;}
    char root[16385];memcpy(root,buf+at,q.root_bytes);root[q.root_bytes]=0;at+=q.root_bytes;
    size_t n=strlen(root);while(n>1&&root[n-1]=='/')root[--n]=0;
    item *items=calloc(q.count,sizeof *items);if(!items){free(buf);return 1;}
    for(uint32_t i=0;i<q.count;i++){
        wslp_rename_item record;
        if(h.size-at<sizeof record)goto malformed;memcpy(&record,buf+at,sizeof record);at+=sizeof record;
        if(record.from_bytes>h.size-at||!name((char*)buf+at,record.from_bytes))goto malformed;
        memcpy(items[i].from,buf+at,record.from_bytes);at+=record.from_bytes;
        if(record.to_bytes>h.size-at||!name((char*)buf+at,record.to_bytes))goto malformed;
        memcpy(items[i].to,buf+at,record.to_bytes);at+=record.to_bytes;items[i].expected=record.identity;
    }
    if(at!=h.size)goto malformed;
    free(buf);buf=NULL;int stopped=0,failed=0,parent=open(root,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NONBLOCK);
    if(parent<0){for(uint32_t i=0;i<q.count;i++){items[i].r.state=R_ERROR;items[i].r.error=errno;}failed=1;}
    /* Duplicate sources/outputs are conflicts even for hard-linked inodes. */
    for(uint32_t i=0;i<q.count;i++)for(uint32_t j=0;j<i;j++)if(!strcmp(items[i].from,items[j].from)||!strcmp(items[i].to,items[j].to)){
        items[i].r.state=items[j].r.state=R_ERROR;items[i].r.error=items[j].r.error=EEXIST;failed=1;
    }
    for(uint32_t i=0;i<q.count&&parent>=0;i++){
        if(cancelled()){stopped=1;break;}
        if(items[i].r.state==R_ERROR)continue;
        int err=check(parent,&items[i],q.apply);items[i].r.error=err;
        items[i].r.state=err?R_ERROR:!strcmp(items[i].from,items[i].to)?R_UNCHANGED:R_READY;failed|=err!=0;
        wslp_rename_progress p={i+1,0};if(!send_frame(WSLP_RENAME_PROGRESS,&p,sizeof p)){stopped=1;break;}
    }
    if(stopped)mark(items,q.count,R_STOPPED);
    else if(q.apply&&failed)mark(items,q.count,R_BLOCKED);
    else if(q.apply){
      int barrier=fp_ancestry_sync(parent,cancelled);
      if(barrier)for(uint32_t i=0;i<q.count;i++)if(items[i].r.state==R_READY){
          items[i].r.state=barrier==ECANCELED?R_STOPPED:R_ERROR;items[i].r.error=barrier==ECANCELED?0:barrier;
      }
      if(barrier==ECANCELED)stopped=1;
      for(uint32_t i=0;!barrier&&i<q.count;i++){
        if(cancelled()){stopped=1;mark(items,q.count,R_STOPPED);break;}
        if(items[i].r.state==R_UNCHANGED)continue;
        int err=check(parent,&items[i],1);
        if(!err&&syscall(SYS_renameat2,parent,items[i].from,parent,items[i].to,1))err=errno; /* RENAME_NOREPLACE */
        items[i].r.error=err;items[i].r.state=err?R_ERROR:R_RENAMED;
        if(!err){
            struct stat after;if(fstatat(parent,items[i].to,&after,AT_SYMLINK_NOFOLLOW)||
                (uint64_t)after.st_dev!=items[i].expected.device||(uint64_t)after.st_ino!=items[i].expected.inode){items[i].r.state=R_UNCERTAIN;items[i].r.error=ESTALE;}
            int sync;do sync=fsync(parent);while(sync<0&&errno==EINTR);
            if(sync<0){items[i].r.state=R_UNCERTAIN;items[i].r.error=errno;}
        }
        wslp_rename_progress p={i+1,1};if(!send_frame(WSLP_RENAME_PROGRESS,&p,sizeof p)){stopped=1;mark(items,q.count,R_STOPPED);break;}
    }
    }
    if(parent>=0)close(parent);
    size_t bytes=sizeof(wslp_rename_end)+q.count*sizeof(wslp_rename_result);unsigned char *reply=malloc(bytes);
    if(!reply){free(items);return 1;}wslp_rename_end end={q.count,stopped};memcpy(reply,&end,sizeof end);
    for(uint32_t i=0;i<q.count;i++)memcpy(reply+sizeof end+i*sizeof(wslp_rename_result),&items[i].r,sizeof items[i].r);
    int ok=send_frame(WSLP_RENAME_RESULT,reply,(uint32_t)bytes);free(reply);free(items);return ok?0:1;
malformed:free(buf);free(items);return 1;
}
