/* Pin a directory for an isolated scanner/copy worker. All descendants open
 * descriptor-relative without crossing mounts or links; /proc anchors are our live parents. */
#define _GNU_SOURCE
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include "wslproto.h"
#include "fpancestry.h"
#include "fpmount.h"
static int io(int fd,void *p,size_t n,int out){char *s=p;while(n){ssize_t k=out?write(fd,s,n):read(fd,s,n);if(k<0&&errno==EINTR)continue;if(k<=0)return 0;s+=k;n-=k;}return 1;}
static int guard_sync(int fd){int rc;do rc=fsync(fd);while(rc<0&&errno==EINTR);return rc<0?errno:0;}
int fp_guard_main(void)
{
    signal(SIGPIPE,SIG_IGN);wslp_hello hi={WSLP_MAGIC,WSLP_VERSION,getuid(),getpid()};wslp_head h={WSLP_HELLO,sizeof hi,0};
    if(!io(1,&h,sizeof h,1)||!io(1,&hi,sizeof hi,1)||!io(0,&h,sizeof h,0)||h.type!=WSLP_GUARD||h.id!=1||h.size<=sizeof(wslp_guard_request)||h.size>16384+sizeof(wslp_guard_request))return 1;
    wslp_guard_request q;char path[16385];size_t n=h.size-sizeof q;
    if(!io(0,&q,sizeof q,0)||!io(0,path,n,0)||memchr(path,0,n)||q.anchored>1||q.create>1)return 1;path[n]=0;
    wslp_guard_result r={0};int fd=-1;char *rest=NULL;
    if(path[0]!='/'||(q.create&&!q.anchored))r.error=EINVAL;
    else if(q.anchored){
        unsigned pid,handle;int used=0;
        if(sscanf(path,"/proc/%u/fd/%u%n",&pid,&handle,&used)!=2||!pid||!handle||(path[used]&&path[used]!='/'))r.error=EINVAL;
        else{char saved=path[used];path[used]=0;fd=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC);path[used]=saved;rest=path+used;if(*rest=='/')rest++;if(fd<0)r.error=errno;}
    }else{fd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);if(fd<0)r.error=errno;}
    struct stat st;
    if(!r.error&&fstat(fd,&st))r.error=errno;
    if(!r.error&&q.device&&(uint64_t)st.st_dev!=q.device)r.error=EXDEV;
    uint64_t device=!r.error?(uint64_t)st.st_dev:0;
    if(!r.error&&rest&&*rest){
        char *part=rest;
        for(;;){char *slash=strchr(part,'/');if(slash)*slash=0;
            if(!*part||!strcmp(part,".")||!strcmp(part,"..")||strlen(part)>255){r.error=EINVAL;break;}
            if(q.create&&mkdirat(fd,part,0777)&&errno!=EEXIST){r.error=errno;break;}
            int child=fp_mount_open(fd,part,O_RDONLY|O_DIRECTORY);
            if(child<0){r.error=errno;break;}
            if(fstat(child,&st)){r.error=errno;close(child);break;}
            if((uint64_t)st.st_dev!=device){r.error=EXDEV;close(child);break;}
            if(q.create){int error=guard_sync(child),parent_error=guard_sync(fd);if(error||parent_error){r.error=error?error:parent_error;close(child);break;}}
            close(fd);fd=child;if(!slash)break;part=slash+1;
        }
    }
    if(!r.error){r.device=device;r.pid=(uint32_t)getpid();r.fd=(uint32_t)fd;}
    h=(wslp_head){WSLP_GUARD_RESULT,sizeof r,1};int ok=io(1,&h,sizeof h,1)&&io(1,&r,sizeof r,1);
    /* Commands act only on this exact descriptor, even if its name changes.
     * Creation barriers precede the initial result; final barriers precede
     * each sync ACK. EOF releases the anchor without path-based cleanup. */
    int result=0;
    if(ok&&!r.error)while(io(0,&h,sizeof h,0)){
        if((h.type!=WSLP_GUARD_SYNC&&h.type!=WSLP_GUARD_ANCESTRY)||h.id!=1||h.size){result=1;break;}
        int32_t error=h.type==WSLP_GUARD_ANCESTRY?fp_ancestry_sync(fd,NULL):guard_sync(fd);h=(wslp_head){WSLP_GUARD_ACK,sizeof error,1};
        if(!io(1,&h,sizeof h,1)||!io(1,&error,sizeof error,1)){result=1;break;}
    }
    if(fd>=0)close(fd);return ok?result:1;
}
