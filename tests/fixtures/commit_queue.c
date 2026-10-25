/* Private queue boundary fixture. Never linked into the shipped helper. */
#define _GNU_SOURCE
#include <stdatomic.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int queue_fsync(int fd);
static int queue_thread(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
#define fsync queue_fsync
#define pthread_create queue_thread
#include "../../linux/fpjobs.c"
#undef fsync
#undef pthread_create
static atomic_uint active, peak, hits, threads, payloads, parent_calls, stages;
static pthread_t owner;
static int queue_thread(pthread_t *t,const pthread_attr_t *a,void *(*fn)(void *),void *arg)
{
    unsigned n=atomic_fetch_add(&threads,1)+1;
    const char *fail=getenv("CQ_THREAD_FAIL");
    return fail&&n==(unsigned)atoi(fail)?EAGAIN:pthread_create(t,a,fn,arg);
}
static int queue_fsync(int fd)
{
    char proc[64],path[65536];snprintf(proc,sizeof proc,"/proc/self/fd/%d",fd);
    ssize_t n=readlink(proc,path,sizeof path-1);if(n<0)return fsync(fd);path[n]=0;
    size_t len=(size_t)n;int payload=len>=8&&!strcmp(path+len-8,"/payload");
    if(payload)atomic_fetch_add(&payloads,1);
    else if(strstr(path,"/.openpilot-stage-"))atomic_fetch_add(&stages,1);
    else if(getenv("CQ_DEST")&&!strcmp(path,getenv("CQ_DEST")))atomic_fetch_add(&parent_calls,1);
    unsigned now=atomic_fetch_add(&active,1)+1,old=atomic_load(&peak);
    while(now>old&&!atomic_compare_exchange_weak(&peak,&old,now)){}
    const char *delay=getenv("CQ_DELAY_US");if(payload&&delay)usleep((unsigned)atoi(delay));
    int rc=fsync(fd),error=rc<0?errno:0;
    const char *boundary=getenv("CQ_BOUNDARY");
    int match=boundary&&((!strcmp(boundary,"payload")&&payload)||
        (!strcmp(boundary,"stage")&&!payload&&strstr(path,"/.openpilot-stage-"))||
        (!strcmp(boundary,"parent")&&!pthread_equal(owner,pthread_self())&&!strcmp(path,getenv("CQ_DEST")?getenv("CQ_DEST"):"")));
    if(match&&atomic_fetch_add(&hits,1)==0){
        const char *mutation=getenv("CQ_MUTATE"),*target=getenv("CQ_TARGET");
        if(mutation&&target){
            if(!strcmp(mutation,"replace")){
                char saved[65536];snprintf(saved,sizeof saved,"%s.saved",target);
                if(rename(target,saved))_exit(92);
                int out=open(target,O_WRONLY|O_CREAT|O_EXCL,0600);
                if(out<0||write(out,"foreign bytes",13)!=13||close(out))_exit(93);
            }else if(!strcmp(mutation,"metadata")){
                if(chmod(target,0400))_exit(94);
            }else{
                int out=open(target,O_WRONLY|O_TRUNC|O_NOFOLLOW);
                if(out<0||write(out,"late edit",9)!=9||close(out))_exit(95);
            }
        }
        const char *mark=getenv("CQ_MARK"),*release=getenv("CQ_RELEASE");
        if(mark){int out=open(mark,O_WRONLY|O_CREAT|O_EXCL,0600);if(out<0||close(out))_exit(96);}
        if(release){while(access(release,F_OK))usleep(1000);}
        if(getenv("CQ_FAIL")){error=EIO;rc=-1;}
    }
    atomic_fetch_sub(&active,1);errno=error;return rc;
}
int main(void)
{
    owner=pthread_self();int rc=fp_jobs_main();
    fprintf(stderr,"{\"peak_flushes\":%u,\"threads\":%u,\"matched\":%u,\"payloads\":%u,\"parents\":%u,\"stages\":%u}\n",atomic_load(&peak),atomic_load(&threads),atomic_load(&hits),atomic_load(&payloads),atomic_load(&parent_calls),atomic_load(&stages));
    return rc;
}
