/* Metadata-only descriptor traversal. No shell, payload reads or filesystem writes. */
#define _GNU_SOURCE
#include <sys/stat.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "../app/spacewire.h"
#include "fpmount.h"
typedef struct space_scan {uint32_t next,errors,stop,error;int mode;} space_scan;
static int sp_io(int fd,void *data,size_t n,int output){char *p=data;while(n){ssize_t got=output?write(fd,p,n):read(fd,p,n);if(got<0&&errno==EINTR)continue;if(got<=0)return 0;p+=got;n-=got;}return 1;}
static int sp_live(space_scan *s){struct pollfd p={0,POLLIN,0};if(poll(&p,1,0)>0&&(p.revents&(POLLIN|POLLHUP|POLLERR|POLLNVAL)))s->stop=1;return !s->stop;}
static int sp_send(space_scan *s,sp_record *r,const char *name){r->magic=SP_MAGIC;r->chars=name?(uint32_t)strlen(name):0;if(!sp_io(1,r,sizeof *r,1)||(r->chars&&!sp_io(1,(void*)name,r->chars,1)))s->stop=1;if(r->type==SP_ENTRY&&(r->flags&SP_FAILED))s->errors++;return !s->stop;}
static void sp_walk(space_scan *s,int fd,const struct stat *st,uint32_t parent,const char *name,unsigned depth){
 if(!sp_live(s)){close(fd);return;}uint32_t id=s->next++;
 sp_record r={.type=SP_ENTRY,.id=id,.parent=parent,.flags=SP_DIR};
 if(depth>=128){r.flags|=SP_FAILED;r.error=EOVERFLOW;}
 r.device=st->st_dev;r.identity=st->st_ino;
 if(!sp_send(s,&r,name)||depth>=128){close(fd);return;}
 if(s->mode==2&&depth){close(fd);r=(sp_record){.type=SP_LEAVE,.id=id};sp_send(s,&r,NULL);return;}
 DIR *dir=fdopendir(fd);if(!dir){r=(sp_record){.type=SP_ENTRY,.id=s->next++,.parent=id,.flags=SP_FAILED|SP_SKIP,.error=(uint32_t)errno};sp_send(s,&r,"(enumeration error)");close(fd);return;}
 while(sp_live(s)&&s->next<(s->mode==3?SP_ANALYSIS_LIMIT:SP_LIMIT)){errno=0;struct dirent *e=readdir(dir);if(!e){if(errno){r=(sp_record){.type=SP_ENTRY,.id=s->next++,.parent=id,.flags=SP_FAILED|SP_SKIP,.error=(uint32_t)errno};sp_send(s,&r,"(enumeration error)");}break;}
  if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
  struct stat item;int child=fp_mount_open(fd,e->d_name,O_PATH);int error=child<0?errno:0,directory=0;
  if(!error&&fstat(child,&item))error=errno;
  if(!error&&S_ISDIR(item.st_mode)){directory=1;
   int opened=fp_mount_open(fd,e->d_name,O_RDONLY|O_DIRECTORY);struct stat check;
   if(opened<0)error=errno;else if(fstat(opened,&check)){error=errno;close(opened);}else if(check.st_dev!=item.st_dev||check.st_ino!=item.st_ino){error=ESTALE;close(opened);}else{close(child);sp_walk(s,opened,&item,id,e->d_name,depth+1);continue;}
  }
  r=(sp_record){.type=SP_ENTRY,.id=s->next++,.parent=id};
  if(error){r.flags=(directory?SP_DIR:SP_SKIP)|(error==EXDEV||error==ELOOP?0:SP_FAILED);r.error=error;}
  else if(!S_ISREG(item.st_mode))r.flags=SP_SKIP;
  else{r.logical=item.st_size<0?0:(uint64_t)item.st_size;r.allocated=item.st_blocks<0?0:(uint64_t)item.st_blocks*512;r.device=item.st_dev;r.identity=item.st_ino;if(s->mode==2&&item.st_nlink>1)r.reserved=1;if(item.st_size<0||item.st_blocks<0){r.flags|=SP_ALLOC_UNKNOWN|SP_FAILED;r.error=EOVERFLOW;}}
  if(child>=0)close(child);if(!sp_send(s,&r,e->d_name))break;
 }
 closedir(dir);r=(sp_record){.type=SP_LEAVE,.id=id};sp_send(s,&r,NULL);
}
#include "fpsummary.h"
static int sp_page_same(const struct stat *a,const struct stat *b){return a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&a->st_mtim.tv_sec==b->st_mtim.tv_sec&&a->st_mtim.tv_nsec==b->st_mtim.tv_nsec&&a->st_ctim.tv_sec==b->st_ctim.tv_sec&&a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;}
/* Re-enumerating partitions keeps Previous bounded without a growing cookie
 * history. Skipped pages need no payload reads or per-file metadata opens when
 * the filesystem supplies d_type. Directory dates guard membership/order. */
static void sp_page_walk(space_scan *s,int fd,const struct stat *st,const sp_page_request *q){
 sp_record root={.type=SP_ENTRY,.parent=SP_NONE,.flags=SP_DIR,.device=st->st_dev,.identity=st->st_ino};s->next=1;
 if(!sp_send(s,&root,"")){close(fd);return;}
 if((q->device||q->identity)&&(q->device!=(uint64_t)st->st_dev||q->identity!=(uint64_t)st->st_ino))s->error=ESTALE;
 if((q->mtime||q->ctime||q->mtime_ns||q->ctime_ns)&&(q->mtime!=(uint64_t)st->st_mtim.tv_sec||q->ctime!=(uint64_t)st->st_ctim.tv_sec||q->mtime_ns!=(uint32_t)st->st_mtim.tv_nsec||q->ctime_ns!=(uint32_t)st->st_ctim.tv_nsec))s->error=ESTALE;
 DIR *dir=NULL;if(!s->error){dir=fdopendir(fd);if(!dir)s->error=errno;}
 uint64_t page=0,ordinal=0,first=0,names=0;uint32_t rows=0,sent=0;int more=0;
 while(dir&&!s->error&&sp_live(s)){
  errno=0;struct dirent *e=readdir(dir);if(!e){if(errno)s->error=errno;break;}
  if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")||e->d_type==DT_DIR)continue;
  struct stat item;int child=-1,error=0,known=0;
  if(e->d_type==DT_UNKNOWN){child=fp_mount_open(fd,e->d_name,O_PATH);error=child<0?errno:0;if(!error){if(fstat(child,&item))error=errno;else known=1;}if(known&&S_ISDIR(item.st_mode)){close(child);continue;}}
  size_t length=strlen(e->d_name);if(length>4095){if(child>=0)close(child);s->error=EOVERFLOW;break;}uint64_t bytes=(length+1)*2;
  if(rows==SP_PAGE_FILES||names+bytes>SP_PAGE_NAMES){page++;rows=0;names=0;first=ordinal;}
  if(page>q->page){if(child>=0)close(child);more=1;break;}
  rows++;names+=bytes;ordinal++;
  if(page!=q->page){if(child>=0)close(child);continue;}
  if(child<0&&!error){child=fp_mount_open(fd,e->d_name,O_PATH);error=child<0?errno:0;}
  if(!error&&!known&&fstat(child,&item))error=errno;
  sp_record r={.type=SP_ENTRY,.id=s->next++,.parent=0};
  if(error){r.flags=SP_SKIP|(error==EXDEV||error==ELOOP?0:SP_FAILED);r.error=error;}
  else if(!S_ISREG(item.st_mode))r.flags=SP_SKIP;
  else{r.logical=item.st_size<0?0:(uint64_t)item.st_size;r.allocated=item.st_blocks<0?0:(uint64_t)item.st_blocks*512;r.device=item.st_dev;r.identity=item.st_ino;r.reserved=item.st_nlink>1;if(item.st_size<0||item.st_blocks<0){r.flags|=SP_ALLOC_UNKNOWN|SP_FAILED;r.error=EOVERFLOW;}}
  if(child>=0)close(child);sent++;if(!sp_send(s,&r,e->d_name))break;
 }
 struct stat after;if(!s->error&&(fstat(fd,&after)||!sp_page_same(st,&after)))s->error=ESTALE;
 if(!s->error&&q->page>page)s->error=ERANGE;
 if(dir)closedir(dir);else close(fd);
 if(!s->error&&!s->stop){sp_record end={.type=SP_LEAVE,.id=0};sp_send(s,&end,NULL);
  sp_record result={.type=SP_PAGE,.logical=q->page,.allocated=more?ordinal-sent: first,.reserved=sent,.flags=more?SP_PAGE_MORE:0,.device=(uint64_t)st->st_mtim.tv_sec,.identity=(uint64_t)st->st_ctim.tv_sec,.id=(uint32_t)st->st_mtim.tv_nsec,.parent=(uint32_t)st->st_ctim.tv_nsec};sp_send(s,&result,NULL);
 }
}
int fp_space_mode_main(int mode){signal(SIGPIPE,SIG_IGN);uint32_t request[3];if(!sp_io(0,request,sizeof request,0)||request[0]!=SP_MAGIC||request[1]!=SP_VERSION||!request[2]||request[2]>32768)return 1;
 char *path=calloc(request[2]+1,1);if(!path||!sp_io(0,path,request[2],0)||memchr(path,0,request[2])||path[0]!='/'){free(path);return 1;}
 sp_page_request page={0};if(mode==4&&(!sp_io(0,&page,sizeof page,0)||page.mtime_ns>=1000000000||page.ctime_ns>=1000000000)){free(path);return 1;}
 while(request[2]>1&&path[request[2]-1]=='/')path[--request[2]]=0;
 space_scan s={.mode=mode};if(mode==1||mode==3){sp_record reset={.type=SP_RESET,.identity=mode==1?SP_BACKEND_SUMMARY:SP_BACKEND_ANALYSIS};sp_send(&s,&reset,NULL);}struct stat st;int fd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC),error=fd<0?errno:0;
 if(!error&&fstat(fd,&st))error=errno;
 if(error){if(fd>=0)close(fd);sp_record r={.type=SP_ENTRY,.parent=SP_NONE,.flags=SP_DIR|SP_FAILED,.error=error};sp_send(&s,&r,"");}
 else if(mode==1){sp_links links={0};sp_summary_walk(&s,&links,fd,&st,SP_NONE,"",0,0);free(links.items);}else if(mode==4)sp_page_walk(&s,fd,&st,&page);else sp_walk(&s,fd,&st,SP_NONE,"",0);
 uint32_t limit=mode==1?SP_SUMMARY_LIMIT:mode==3?SP_ANALYSIS_LIMIT:SP_LIMIT;
 sp_record end={.type=SP_END,.flags=s.stop?SP_STOPPED:s.errors||s.error||s.next>=limit?SP_PARTIAL:SP_COMPLETE,.error=s.error?s.error:s.next>=limit?EOVERFLOW:0};sp_send(&s,&end,NULL);free(path);return 0;
}
int fp_space_main(void){return fp_space_mode_main(0);}
