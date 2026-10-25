/* Private VM fixture, linked to the real Linux writer. Never shipped in folderwing-wsl.
 * Checkpoints notify the supervising guest process and SIGSTOP this worker;
 * only an external, owned-VM power-off supplies VM-interruption evidence. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int fp_jobs_main(void);
int __real_fsync(int);
ssize_t __real_write(int,const void *,size_t);
int __real_unlinkat(int,const char *,int);
static int parent_synced,stage_synced,source_unlinked;
static int path_is(int fd,const char *wanted,int pattern)
{
    char proc[64],path[65536];if(!wanted)return 0;
    snprintf(proc,sizeof proc,"/proc/self/fd/%d",fd);
    ssize_t n=readlink(proc,path,sizeof path-1);if(n<0)return 0;path[n]=0;
    return pattern?!fnmatch(wanted,path,FNM_PATHNAME):!strcmp(wanted,path);
}
static int published(void)
{
    const char *path=getenv("OPVM_TARGET");if(!path)return 0;
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NOATIME);if(fd<0)return 0;
    char prefix[15];ssize_t n=pread(fd,prefix,sizeof prefix,0);close(fd);
    return n==(ssize_t)sizeof prefix&&!memcmp(prefix,"OpenPilotVM-new\0",sizeof prefix);
}
static int removed(void)
{
    const char *path=getenv("OPVM_SOURCE");struct stat st;
    return path&&lstat(path,&st)<0&&errno==ENOENT;
}
static void checkpoint(const char *phase)
{
    const char *wanted=getenv("OPVM_PHASE");if(!wanted||strcmp(wanted,phase))return;
    const char *text=getenv("OPVM_NOTIFY_FD");if(!text||!*text)_exit(88);
    char *end;long value=strtol(text,&end,10);if(*end||value<3||value>65535)_exit(88);
    char event[512];int n=snprintf(event,sizeof event,
        "{\"phase\":\"%s\",\"fixture_version\":1,\"destination_parent_synced\":%d,\"stage_parent_synced\":%d,\"source_unlinked\":%d}\n",
        phase,parent_synced,stage_synced,source_unlinked);
    if(n<0||(size_t)n>=sizeof event)_exit(88);
    for(int sent=0;sent<n;){ssize_t k=__real_write((int)value,event+sent,(size_t)(n-sent));if(k<0&&errno==EINTR)continue;if(k<=0)_exit(88);sent+=(int)k;}
    raise(SIGSTOP);
    _exit(89); /* Never silently continue a checkpointed transfer. */
}
ssize_t __wrap_write(int fd,const void *bytes,size_t count)
{
    if(count>1&&path_is(fd,getenv("OPVM_STAGE_FILE"),1)&&
       getenv("OPVM_PHASE")&&!strcmp(getenv("OPVM_PHASE"),"partial-write")){
        ssize_t n=__real_write(fd,bytes,count/2);
        if(n!=(ssize_t)(count/2))_exit(87);
        checkpoint("partial-write");
    }
    return __real_write(fd,bytes,count);
}
int __wrap_fsync(int fd)
{
    int payload=path_is(fd,getenv("OPVM_STAGE_FILE"),1);
    int destination=path_is(fd,getenv("OPVM_DEST_DIR"),0)&&published();
    if(payload)checkpoint("before-payload-sync");
    if(destination)checkpoint("before-destination-sync");
    int result=__real_fsync(fd);if(result)return result;
    if(payload)checkpoint("after-payload-sync");
    if(destination)parent_synced=1;
    if(parent_synced&&published()&&path_is(fd,getenv("OPVM_STAGE_DIR"),1)){
        stage_synced=1;checkpoint("after-publication-sync");
    }
    if(source_unlinked&&removed()&&path_is(fd,getenv("OPVM_SOURCE_DIR"),0))checkpoint("after-source-sync");
    return result;
}
int __wrap_unlinkat(int fd,const char *name,int flags)
{
    int source=!flags&&!strcmp(name,"source.bin")&&path_is(fd,getenv("OPVM_SOURCE_DIR"),0);
    if(source)checkpoint("before-source-unlink");
    int result=__real_unlinkat(fd,name,flags);
    if(source&&!result){source_unlinked=1;checkpoint("after-source-unlink");}
    return result;
}
int main(int argc,char **argv)
{
    if(argc!=2||strcmp(argv[1],"--jobs"))return 2;
    return fp_jobs_main();
}
