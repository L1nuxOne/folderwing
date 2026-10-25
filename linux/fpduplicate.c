#define _GNU_SOURCE
#include <sys/stat.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "../app/duplicatewire.h"
#include "fpmount.h"
#include "fpsha256.h"
static int dio(int fd,void *data,size_t n,int out){while(n){ssize_t k=out?write(fd,data,n):read(fd,data,n);if(k<0&&errno==EINTR)continue;if(k<=0)return 0;data=(char*)data+k;n-=k;}return 1;}
static int dlive(void){struct pollfd p={0,POLLIN,0};return !(poll(&p,1,0)>0&&(p.revents&(POLLIN|POLLHUP|POLLERR|POLLNVAL)));}
static int dpath(int root,const char *name){char *copy=strdup(name);if(!copy)return -1;int fd=dup(root);char *at=copy;if(!*at||*at=='/'){errno=EINVAL;goto bad;}for(;;){char *end=strchr(at,'/');if(end)*end=0;if(!*at||!strcmp(at,".")||!strcmp(at,"..")){errno=EINVAL;goto bad;}int child=fp_mount_open(fd,at,O_RDONLY|(end?O_DIRECTORY:0));int e=errno;close(fd);fd=child;errno=e;if(fd<0)break;struct stat s;if(fstat(fd,&s)|| (end?!S_ISDIR(s.st_mode):!S_ISREG(s.st_mode))){errno=EINVAL;goto bad;}if(!end)break;at=end+1;}free(copy);return fd;
bad: {int e=errno;if(fd>=0)close(fd);free(copy);errno=e;return -1;}}
static int dstamp(int fd,du_stamp *out){struct stat s;if(fstat(fd,&s))return errno;if(!S_ISREG(s.st_mode)||s.st_size<0)return EINVAL;*out=(du_stamp){s.st_size,s.st_dev,s.st_ino,(uint64_t)s.st_mtim.tv_sec*1000000000+s.st_mtim.tv_nsec,(uint64_t)s.st_ctim.tv_sec*1000000000+s.st_ctim.tv_nsec};return 0;}
static int dcheck(int root,int fd,const char *path,const du_stamp *before){du_stamp a,b;int e=dstamp(fd,&a);if(e)return e;int current=dpath(root,path);if(current<0)return errno;e=dstamp(current,&b);close(current);return e?e:memcmp(before,&a,sizeof a)||memcmp(before,&b,sizeof b)?ESTALE:0;}
int fp_duplicate_main(void){signal(SIGPIPE,SIG_IGN);uint32_t hi[3];if(!dio(0,hi,sizeof hi,0)||hi[0]!=DU_MAGIC||hi[1]!=DU_VERSION||!hi[2]||hi[2]>32768)return 1;char rootpath[32769];if(!dio(0,rootpath,hi[2],0)||memchr(rootpath,0,hi[2])||rootpath[0]!='/')return 1;rootpath[hi[2]]=0;int root=open(rootpath,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);du_reply reply={.error=root<0?(uint32_t)errno:0};if(!dio(1,&reply,sizeof reply,1)||root<0)return 1;
 int fd=-1;char path[32769]={0};du_stamp before={0};uint8_t block[DU_BLOCK];du_request request;
 while(dio(0,&request,sizeof request,0)){reply=(du_reply){0};if(request.op==DU_OPEN){if(fd>=0)close(fd);fd=-1;if(!request.chars||request.chars>32768)break;if(!dio(0,path,request.chars,0)||memchr(path,0,request.chars))break;path[request.chars]=0;fd=dpath(root,path);reply.error=fd<0?errno:dstamp(fd,&before);if(!reply.error&&(before.size!=request.expected.size||before.device!=request.expected.device||before.identity!=request.expected.identity))reply.error=ESTALE;reply.stamp=before;if(reply.error&&fd>=0){close(fd);fd=-1;}}
 else if(request.chars||fd<0)reply.error=EINVAL;
 else if(request.op==DU_READ){while(reply.count<DU_BLOCK){ssize_t n=read(fd,block+reply.count,DU_BLOCK-reply.count);if(n<0&&errno==EINTR)continue;if(n<0){reply.error=errno;break;}if(!n){reply.error=dcheck(root,fd,path,&before);break;}reply.count+=(uint32_t)n;}}
 else if(request.op==DU_HASH){fp_sha256 sha;fp_sha_init(&sha);uint64_t bytes=0,next=16*1024*1024;for(;;){if(!dlive()){reply.error=ECANCELED;break;}ssize_t n=read(fd,block,sizeof block);if(n<0&&errno==EINTR)continue;if(n<0){reply.error=errno;break;}if(!n){reply.error=dcheck(root,fd,path,&before);if(bytes!=before.size&&!reply.error)reply.error=ESTALE;break;}fp_sha_update(&sha,block,n);bytes+=n;if(bytes>=next){du_reply progress={.count=DU_PROGRESS,.stamp.size=bytes};if(!dio(1,&progress,sizeof progress,1)){reply.error=EPIPE;break;}next=bytes+16*1024*1024;}}if(!reply.error)fp_sha_final(&sha,reply.hash);reply.stamp=before;}
 else reply.error=EINVAL;
 if(!dio(1,&reply,sizeof reply,1))break;if(request.op==DU_READ&&!reply.error&&reply.count&&!dio(1,block,reply.count,1))break;
 }if(fd>=0)close(fd);close(root);return 0;}
