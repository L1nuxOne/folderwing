/* Acceptance-only transport fault: consume a complete rename request, then
 * disconnect without replying. Only an owned diagnostic trace is written;
 * selected source/target files are never accessed. */
#define _GNU_SOURCE
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include "wslproto.h"
static void note(const char *tag,int value)
{
    int fd=open("reply-loss-trace",O_WRONLY|O_CREAT|O_APPEND,0600);if(fd<0)return;
    char line[120];int n=snprintf(line,sizeof line,"%s=%d\n",tag,value);write(fd,line,n);close(fd);
}
static int io(int fd,void *data,unsigned n,int output)
{
    char *p=data;while(n){ssize_t k=output?write(fd,p,n):read(fd,p,n);if(k<0&&errno==EINTR)continue;
        if(k<=0){note(output?"write_error":"read_error",k<0?errno:0);return 0;}p+=k;n-=(unsigned)k;}return 1;
}
int main(void)
{
    signal(SIGPIPE,SIG_IGN);note("started",fcntl(0,F_GETFL));wslp_hello hello={WSLP_MAGIC,WSLP_VERSION,getuid(),getpid()};
    wslp_head head={WSLP_HELLO,sizeof hello,0};
    if(!io(1,&head,sizeof head,1)||!io(1,&hello,sizeof hello,1))return 1;note("hello_sent",1);
    if(!io(0,&head,sizeof head,0))return 1;note("request_type",head.type);note("request_size",head.size);
    if(head.type!=WSLP_RENAME_BATCH||head.size>WSLP_MAX)return 1;
    char buffer[4096];while(head.size){unsigned n=head.size>sizeof buffer?sizeof buffer:head.size;if(!io(0,buffer,n,0))return 1;head.size-=n;}
    note("request_consumed",1);return 0;
}
