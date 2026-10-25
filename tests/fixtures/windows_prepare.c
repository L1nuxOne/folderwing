/* Protocol-only fixture: resolve an owned Unix directory as the fake Windows
 * endpoint. Actual NTFS writer/ancestry behavior belongs to native tests. */
#define _GNU_SOURCE
#include <unistd.h>
#include <string.h>
#include <errno.h>
static int prepare_execl(const char *path,const char *arg,...);
#define execl prepare_execl
#include "../../linux/fpjobs.c"
#undef execl
#include <stdarg.h>
static int prepare_execl(const char *path,const char *arg,...)
{
    if(strcmp(path,"/usr/bin/wslpath")){errno=ENOTSUP;return -1;}
    va_list args;va_start(args,arg);const char *option=va_arg(args,const char *),*owned=va_arg(args,const char *);va_end(args);
    if(!option||strcmp(option,"-u")||!owned||owned[0]!='/')_exit(81);
    size_t n=strlen(owned);if(write(1,owned,n)!=(ssize_t)n||write(1,"\n",1)!=1)_exit(82);_exit(0);
}
int main(void){return fp_jobs_main();}
