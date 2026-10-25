/* Link-time fault fixture only. This file is never linked into folderwing-wsl. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int fp_jobs_main(void);
int fp_transfer_main(int importing);
int __real_fsync(int fd);

static int present(const char *key)
{
    const char *path=getenv(key);struct stat st;
    return path&&!lstat(path,&st);
}
static void mutate_target(const char *path)
{
    const char *mode=getenv("FP_DURABILITY_MUTATE"),*target=getenv("FP_DURABILITY_TARGET_STATE");
    const char *pattern=getenv("FP_DURABILITY_MUTATE_MATCH"),*log=getenv("FP_DURABILITY_MUTATION_LOG");
    static int fired;
    if(fired||!mode||!target||!pattern||fnmatch(pattern,path,FNM_PATHNAME))return;
    fired=1;
    if(!strcmp(mode,"delete")||!strcmp(mode,"replace")||!strcmp(mode,"link")||!strcmp(mode,"directory")){
        if(unlink(target))_exit(92);
    }
    if(!strcmp(mode,"link")){if(symlink("concurrent-link",target))_exit(93);}
    else if(!strcmp(mode,"directory")){if(mkdir(target,0700))_exit(94);}
    else if(!strcmp(mode,"metadata")){if(chmod(target,0400))_exit(95);}
    else if(strcmp(mode,"delete")){
        int editing=!strcmp(mode,"edit");
        int out=open(target,O_WRONLY|O_NOFOLLOW|(editing?0:O_CREAT|O_EXCL),0600);struct stat before;
        if(out<0||fstat(out,&before))_exit(96);
        const char bytes[]="late edit!";
        if(write(out,bytes,sizeof bytes-1)!=sizeof bytes-1||ftruncate(out,sizeof bytes-1))_exit(97);
        /* Same-size edits with restored mtime must still fail the ctime guard. */
        struct timespec times[2]={before.st_atim,before.st_mtim};
        if(editing&&futimens(out,times))_exit(98);
        if(close(out))_exit(99);
    }
    int marker=log?open(log,O_WRONLY|O_CREAT|O_EXCL,0600):-1;
    if(marker<0||close(marker))_exit(100);
}
int __wrap_fsync(int fd)
{
    char proc[64],path[65536];snprintf(proc,sizeof proc,"/proc/self/fd/%d",fd);
    ssize_t n=readlink(proc,path,sizeof path-1);if(n<0)return __real_fsync(fd);path[n]=0;
    const char *pattern=getenv("FP_DURABILITY_MATCH"),*nth=getenv("FP_DURABILITY_NTH");
    const char *failure=getenv("FP_DURABILITY_ERRNO"),*log=getenv("FP_DURABILITY_LOG");
    static unsigned matched;int error=0,rc;
    if(pattern&&!fnmatch(pattern,path,FNM_PATHNAME)&&++matched==(unsigned)(nth?atoi(nth):1))error=failure?atoi(failure):EIO;
    if(error){rc=-1;errno=error;}else {rc=__real_fsync(fd);if(rc<0)error=errno;}
    if(!rc)mutate_target(path);
    if(log){
        int out=open(log,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0600);
        if(out<0)_exit(90);
        char record[66000];int size=snprintf(record,sizeof record,"%d\t%d\t%d\t%s\n",error,
            present("FP_DURABILITY_SOURCE_STATE"),present("FP_DURABILITY_TARGET_STATE"),path);
        if(size<0||(size_t)size>=sizeof record||write(out,record,(size_t)size)!=size)_exit(91);
        close(out);
    }
    /* Pause only this owned fixture process after the requested barrier. */
    if(pattern&&!fnmatch(pattern,path,FNM_PATHNAME)&&matched==(unsigned)(nth?atoi(nth):1)&&getenv("FP_DURABILITY_STOP"))raise(SIGSTOP);
    errno=error;return rc;
}
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    if(!strcmp(argv[1],"--jobs"))return fp_jobs_main();
    if(!strcmp(argv[1],"--import"))return fp_transfer_main(1);
    if(!strcmp(argv[1],"--export"))return fp_transfer_main(0);
    return 2;
}
