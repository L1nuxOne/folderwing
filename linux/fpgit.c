#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/wait.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include "../app/gitwire.h"
#include "fpmount.h"
extern char **environ;
static int gio(int fd,void *p,size_t n,int write_){while(n){ssize_t k=write_?write(fd,p,n):read(fd,p,n);if(k<0&&errno==EINTR)continue;if(k<=0)return 0;p=(char*)p+k;n-=k;}return 1;}
static uint64_t gnow(void){struct timespec s;clock_gettime(CLOCK_MONOTONIC,&s);return (uint64_t)s.tv_sec*1000+s.tv_nsec/1000000;}
static int glive(void){struct pollfd p={0,POLLIN,0};return !(poll(&p,1,0)>0&&(p.revents&(POLLIN|POLLHUP|POLLERR|POLLNVAL)));}
static int goid(const char *p){size_t n=strlen(p);if(n!=40&&n!=64)return 0;for(size_t i=0;i<n;i++)if(!((p[i]>='0'&&p[i]<='9')||(p[i]>='a'&&p[i]<='f')))return 0;return 1;}
static int gpath(const char *p){if(!*p||*p=='/')return 0;while(*p){const char *e=strchr(p,'/');size_t n=e?(size_t)(e-p):strlen(p);if(!n||(n==1&&*p=='.')||(n==2&&p[0]=='.'&&p[1]=='.'))return 0;if(!e)return 1;p=e+1;}return 0;}
static int gexec(const char *root,const char *const *tail,uint8_t **out,uint32_t *length,uint32_t cap,uint32_t *code){int pipes[2];if(pipe2(pipes,O_CLOEXEC))return errno;pid_t pid=fork();if(pid<0){int e=errno;close(pipes[0]);close(pipes[1]);return e;}if(!pid){setpgid(0,0);dup2(pipes[1],1);int nul=open("/dev/null",O_RDWR);dup2(nul,0);dup2(nul,2);close(pipes[0]);close(pipes[1]);/* Remove inherited Git routing/config overrides, retaining normal user config. */
 for(;;){char name[256];int found=0;for(char **p=environ;*p;p++)if(!strncmp(*p,"GIT_",4)){const char *eq=strchr(*p,'=');size_t n=eq?(size_t)(eq-*p):0;if(n&&n<sizeof name){memcpy(name,*p,n);name[n]=0;found=1;}break;}if(!found)break;unsetenv(name);}setenv("GIT_OPTIONAL_LOCKS","0",1);setenv("GIT_TERMINAL_PROMPT","0",1);setenv("GIT_NO_LAZY_FETCH","1",1);setenv("LC_ALL","C",1);
 const char *args[32]={"git","--no-pager","--no-optional-locks","-c","core.fsmonitor=false","-c","core.quotePath=false","-c","color.ui=false","-C",root};unsigned i=11;for(unsigned j=0;tail[j]&&i<31;j++)args[i++]=tail[j];args[i]=NULL;execvp("git",(char *const*)args);_exit(errno==ENOENT?127:126);}
 setpgid(pid,pid);close(pipes[1]);fcntl(pipes[0],F_SETFL,O_NONBLOCK);uint8_t *data=malloc((size_t)cap+1);int error=data?0:ENOMEM,status=0;uint32_t used=0;uint64_t until=gnow()+30000;int eof=0;
 while(!error&&!eof){if(!glive()){error=ECANCELED;break;}if(gnow()>=until){error=ETIMEDOUT;break;}uint8_t buf[65536];ssize_t n=read(pipes[0],buf,sizeof buf);if(n>0){if((uint32_t)n>cap-used){error=EFBIG;break;}memcpy(data+used,buf,n);used+=n;}else if(!n)eof=1;else if(errno!=EAGAIN&&errno!=EINTR){error=errno;break;}else{struct pollfd p={pipes[0],POLLIN,0};poll(&p,1,5);}}
 close(pipes[0]);if(!error){for(;;){pid_t done=waitpid(pid,&status,WNOHANG);if(done==pid)break;if(done<0&&errno!=EINTR){error=ECHILD;break;}if(!glive()){error=ECANCELED;break;}if(gnow()>=until){error=ETIMEDOUT;break;}struct timespec pause={0,5000000};nanosleep(&pause,NULL);}}if(error){kill(-pid,SIGKILL);while(waitpid(pid,&status,0)<0&&errno==EINTR){}}*code=WIFEXITED(status)?WEXITSTATUS(status):128;if(!error&&*code)error=*code==127?ENOENT:EIO;if(error){free(data);return error;}data[used]=0;*out=data;*length=used;return 0;}
static int gwork(const char *root,const char *path,uint8_t **out,uint32_t *length){if(!gpath(path))return EINVAL;int parent=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);if(parent<0)return errno;char *copy=strdup(path);if(!copy){close(parent);return ENOMEM;}char *at=copy;int fd=-1,error=0;for(;;){char *end=strchr(at,'/');if(end)*end=0;fd=fp_mount_open(parent,at,O_RDONLY|(end?O_DIRECTORY:0));if(fd<0){error=errno;break;}close(parent);parent=-1;struct stat s;if(fstat(fd,&s)||(end?!S_ISDIR(s.st_mode):!S_ISREG(s.st_mode))){error=EINVAL;break;}if(!end)break;parent=fd;fd=-1;at=end+1;}
 free(copy);if(parent>=0)close(parent);struct stat before,after;if(!error&&fstat(fd,&before))error=errno;if(!error&&(before.st_size<0||(uint64_t)before.st_size>GR_BLOB_LIMIT))error=EFBIG;uint8_t *bytes=NULL;uint32_t n=0;if(!error){bytes=malloc((size_t)before.st_size+1);if(!bytes)error=ENOMEM;}
 while(!error&&n<(uint64_t)before.st_size){if(!glive()){error=ECANCELED;break;}ssize_t got=read(fd,bytes+n,(size_t)before.st_size-n);if(got<0&&errno==EINTR)continue;if(got<=0){error=ESTALE;break;}n+=got;}if(!error&&(fstat(fd,&after)||before.st_dev!=after.st_dev||before.st_ino!=after.st_ino||before.st_size!=after.st_size||before.st_mtim.tv_sec!=after.st_mtim.tv_sec||before.st_mtim.tv_nsec!=after.st_mtim.tv_nsec||before.st_ctim.tv_sec!=after.st_ctim.tv_sec||before.st_ctim.tv_nsec!=after.st_ctim.tv_nsec))error=ESTALE;
 if(fd>=0)close(fd);if(error){free(bytes);return error;}*out=bytes;*length=n;return 0;}
int fp_git_main(void){signal(SIGPIPE,SIG_IGN);gr_request q;if(!gio(0,&q,sizeof q,0)||q.magic!=GR_MAGIC||q.version!=GR_VERSION||!q.root_bytes||q.root_bytes>32768||q.arg_bytes>32768||q.op<GR_STATUS||q.op>GR_WORK)return 1;char root[32769],arg[32769];if(!gio(0,root,q.root_bytes,0)||!gio(0,arg,q.arg_bytes,0)||memchr(root,0,q.root_bytes)||memchr(arg,0,q.arg_bytes))return 1;root[q.root_bytes]=0;arg[q.arg_bytes]=0;if(root[0]!='/')return 1;gr_reply reply={.magic=GR_MAGIC};uint8_t *data=NULL,*repo=NULL;
 if(q.op==GR_STATUS){const char *discover[]={"rev-parse","--show-toplevel",NULL};reply.error=gexec(root,discover,&repo,&reply.root_bytes,131072,&reply.code);if(!reply.error){if(!reply.root_bytes||repo[reply.root_bytes-1]!='\n')reply.error=EINVAL;else{repo[--reply.root_bytes]=0;const char *status[]={"status","--porcelain=v2","--branch","-z","--untracked-files=all","--ignore-submodules=none",NULL};reply.error=gexec((char*)repo,status,&data,&reply.bytes,GR_STATUS_LIMIT,&reply.code);}}}
 else if(q.op==GR_BLOB){if(!goid(arg))reply.error=EINVAL;else{const char *blob[]={"cat-file","blob",arg,NULL};reply.error=gexec(root,blob,&data,&reply.bytes,GR_BLOB_LIMIT,&reply.code);}}
 else reply.error=gwork(root,arg,&data,&reply.bytes);
 if(reply.error){reply.root_bytes=reply.bytes=0;}int ok=gio(1,&reply,sizeof reply,1)&&(!reply.root_bytes||gio(1,repo,reply.root_bytes,1))&&(!reply.bytes||gio(1,data,reply.bytes,1));free(repo);free(data);return ok?0:1;}
