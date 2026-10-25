/* Native Linux content search; one descriptor-relative traversal per request. */
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
#include <time.h>
#include <locale.h>
#include "../app/contentmatch.h"
#include "../src/namecodec.h"
#include "fpmount.h"
typedef struct scan {cf_options options;cf_summary sum;int stopped,limited;uint64_t notified;char path[32768];} scan;
static int io(int fd,void *p,size_t n,int output){while(n){ssize_t got=output?write(fd,p,n):read(fd,p,n);if(got<0&&errno==EINTR)continue;if(got<=0)return 0;p=(char*)p+got;n-=got;}return 1;}
static uint64_t now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static int live(scan *s){struct pollfd p={0,POLLIN,0};if(poll(&p,1,0)>0&&p.revents)s->stopped=1;return !s->stopped&&!s->limited;}
static void failed(scan *s,int error){s->sum.errors++;if(!s->sum.error)s->sum.error=error?error:EIO;}
static int send_record(scan *s,cf_record *r,const char *relative){r->magic=CF_MAGIC;uint16_t *path=NULL;
 if(relative){size_t n=strlen(relative),units=0;wchar_t *wide=calloc(n*2+1,sizeof *wide);if(!wide){failed(s,ENOMEM);s->limited=1;return 0;}if(!fpe_lx_from_bytes((void*)relative,n,wide,n*2+1,1,1,&units)||units>=32768){free(wide);failed(s,ENAMETOOLONG);return 0;}path=malloc(units*2);if(!path){free(wide);failed(s,ENOMEM);s->limited=1;return 0;}for(size_t i=0;i<units;i++)path[i]=(uint16_t)wide[i];free(wide);r->path_chars=(uint32_t)units;}
 int ok=io(1,r,sizeof *r,1)&&(!r->path_chars||io(1,path,r->path_chars*2,1));free(path);if(!ok)s->stopped=1;return ok;
}
static int progress(scan *s){if(!live(s))return 0;uint64_t t=now();if(t-s->notified>=50){s->notified=t;cf_record r={.type=CF_PROGRESS,.summary=s->sum};send_record(s,&r,NULL);}return live(s);}
static int same(const struct stat *a,const struct stat *b){return a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&a->st_size==b->st_size&&a->st_mtim.tv_sec==b->st_mtim.tv_sec&&a->st_mtim.tv_nsec==b->st_mtim.tv_nsec&&a->st_ctim.tv_sec==b->st_ctim.tv_sec&&a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;}
static void file_search(scan *s,int fd,const struct stat *before){
 if(before->st_size<0){failed(s,EOVERFLOW);return;}uint64_t size=(uint64_t)before->st_size;if(size>CF_FILE_BYTES){s->sum.large++;return;}if(size>CF_TOTAL_BYTES-s->sum.bytes){s->limited=1;return;}
 unsigned char *data=malloc(size+1);if(!data){failed(s,ENOMEM);return;}size_t at=0;int error=0;
 while(at<size&&progress(s)){ssize_t n=read(fd,data+at,size-at>65536?65536:size-at);if(n<0&&errno==EINTR)continue;if(n<=0){error=n<0?errno:ESTALE;break;}at+=(size_t)n;s->sum.bytes+=(uint64_t)n;}
 if(!live(s)){free(data);return;}if(error){free(data);failed(s,error);return;}
 cf_hit *hits=NULL;uint32_t count=0;int more=0;int text=content_match_bytes(data,at,&s->options,CF_RESULTS-(uint32_t)s->sum.matches,&hits,&count,&more);free(data);struct stat after;
 if(text<0){failed(s,ENOMEM);return;}if(fstat(fd,&after)){free(hits);failed(s,errno);return;}if(!same(before,&after)){free(hits);failed(s,ESTALE);return;}s->sum.files++;
 if(!text){s->sum.binary++;free(hits);return;}
 for(uint32_t i=0;i<count&&live(s);i++){cf_record r={.type=CF_MATCH,.bytes=size,.mtime=(uint64_t)before->st_mtim.tv_sec*10000000+before->st_mtim.tv_nsec/100+116444736000000000ull,.hit=hits[i]};if(!send_record(s,&r,s->path))break;s->sum.matches++;}free(hits);if(more)s->limited=1;
}
static void walk(scan *s,int fd,size_t length,unsigned depth){if(!progress(s)){close(fd);return;}if(depth>=128){failed(s,EOVERFLOW);close(fd);return;}DIR *dir=fdopendir(fd);if(!dir){failed(s,errno);close(fd);return;}
 while(progress(s)){errno=0;struct dirent *entry=readdir(dir);if(!entry){if(errno)failed(s,errno);break;}if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;if(s->sum.scanned>=CF_FILES){s->limited=1;break;}s->sum.scanned++;
  size_t n=strlen(entry->d_name),sep=length?1:0;if(length+sep+n>=sizeof s->path){failed(s,ENAMETOOLONG);continue;}if(sep)s->path[length]='/';memcpy(s->path+length+sep,entry->d_name,n+1);
  int meta=fp_mount_open(fd,entry->d_name,O_PATH);struct stat st;if(meta<0){if(errno==EXDEV||errno==ELOOP)s->sum.excluded++;else failed(s,errno);goto restore;}if(fstat(meta,&st)){failed(s,errno);close(meta);goto restore;}close(meta);
  if(S_ISDIR(st.st_mode)){if(s->options.flags&CF_RECURSE){int child=fp_mount_open(fd,entry->d_name,O_RDONLY|O_DIRECTORY);struct stat actual;if(child<0)failed(s,errno);else if(fstat(child,&actual)){failed(s,errno);close(child);}else if(st.st_dev!=actual.st_dev||st.st_ino!=actual.st_ino){failed(s,ESTALE);close(child);}else walk(s,child,length+sep+n,depth+1);}else s->sum.excluded++;}
  else if(S_ISREG(st.st_mode)){wchar_t name[1024];size_t units;if(!fpe_lx_from_bytes((void*)entry->d_name,n,name,1024,0,0,&units)){failed(s,EILSEQ);goto restore;}
   if(content_name_matches(name,s->options.pattern)){int file=fp_mount_open(fd,entry->d_name,O_RDONLY);struct stat actual;if(file<0)failed(s,errno);else if(fstat(file,&actual)){failed(s,errno);close(file);}else if(!S_ISREG(actual.st_mode)||actual.st_dev!=st.st_dev||actual.st_ino!=st.st_ino){failed(s,ESTALE);close(file);}else{file_search(s,file,&actual);close(file);}}else s->sum.excluded++;
  }else s->sum.excluded++;
restore:s->path[length]=0;
 }closedir(dir);
}
int fp_content_main(void){signal(SIGPIPE,SIG_IGN);setlocale(LC_CTYPE,"C.UTF-8");uint32_t request[3];scan *s=calloc(1,sizeof *s);if(!s)return 1;
 if(!io(0,request,sizeof request,0)||request[0]!=CF_MAGIC||request[1]!=CF_VERSION||!request[2]||request[2]>32768||!io(0,&s->options,sizeof s->options,0)||!content_options_valid(&s->options)){free(s);return 1;}
 char *root=calloc(request[2]+1,1);if(!root||!io(0,root,request[2],0)||root[0]!='/'||memchr(root,0,request[2])){free(root);free(s);return 1;}while(request[2]>1&&root[request[2]-1]=='/')root[--request[2]]=0;
 int fd=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);if(fd<0)failed(s,errno);else walk(s,fd,0,0);
 s->sum.state=s->stopped?CF_STOPPED:s->limited||s->sum.errors||s->sum.large?CF_PARTIAL:CF_COMPLETE;cf_record end={.type=CF_END,.summary=s->sum};send_record(s,&end,NULL);free(root);free(s);return 0;
}
