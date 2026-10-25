/* One-shot, read-only directory resolution. No shell evaluation of paths. */
#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <pwd.h>
#include "wslproto.h"
#define MAX_PATH_BYTES 16384
static int io(int fd,void *buffer,size_t size,int output)
{
    char *p=buffer;
    while(size){ssize_t n=output?write(fd,p,size):read(fd,p,size);if(n<0&&errno==EINTR)continue;if(n<=0)return 0;p+=n;size-=(size_t)n;}
    return 1;
}
int fp_path_main(void)
{
    wslp_head head;char path[MAX_PATH_BYTES+1],expanded[MAX_PATH_BYTES+1],cwd[MAX_PATH_BYTES+1];
    if(!io(0,&head,sizeof head,0)||head.type!=WSLP_RESOLVE||!head.size||head.size>MAX_PATH_BYTES||!io(0,path,head.size,0)||memchr(path,0,head.size))return 1;
    path[head.size]=0;wslp_resolved reply={0};const char *target=path;
    if(path[0]=='~' && (!path[1]||path[1]=='/')) {
        struct passwd *user=getpwuid(getuid());const char *home=user?user->pw_dir:NULL;
        if(!home||home[0]!='/')reply.err=ENOENT;
        else if(strlen(home)+strlen(path+1)>=sizeof expanded)reply.err=ENAMETOOLONG;
        else {strcpy(expanded,home);strcat(expanded,path+1);target=expanded;}
    }else if(path[0]!='/')reply.err=EINVAL;
    const char *distro=getenv("WSL_DISTRO_NAME");if(!distro||!distro[0]||strlen(distro)>=64)reply.err=EINVAL;
    if(!reply.err && chdir(target)!=0)reply.err=errno;
    if(!reply.err && !getcwd(cwd,sizeof cwd))reply.err=errno;
    if(!reply.err){reply.distro_len=(uint32_t)strlen(distro);reply.path_len=(uint32_t)strlen(cwd);}
    head=(wslp_head){WSLP_RESOLVED,(uint32_t)sizeof reply+reply.distro_len+reply.path_len,head.id};
    if(!io(1,&head,sizeof head,1)||!io(1,&reply,sizeof reply,1))return 1;
    if(reply.distro_len && (!io(1,(void*)distro,reply.distro_len,1)||!io(1,cwd,reply.path_len,1)))return 1;
    return 0;
}
