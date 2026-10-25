/* Native Linux filesystem jobs, in a disposable folderwing-wsl --jobs process.
 * Recursion uses directory FDs and never follows links at a visited leaf.
 * Copies commit a temporary sibling atomically; cancellation removes it.
 * Windows endpoints use their WSL mount, avoiding per-file 9P calls. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "wslproto.h"
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1u
#endif
#include "fpstage.h"
#include "fpmount.h"
#include "fpancestry.h"
#include "fpmetadata.h"
#define PATH_CAP 65536u
#define CHUNK (256u * 1024)
static struct {
    fp_stage stage;
    wslp_job job;
    wslp_progress p;
    char **roots;
    uint32_t *windows;
    unsigned char *valid;
    uint32_t id;
    int cancelled;
    uint64_t last_report;
    unsigned unique;
    dev_t dest_dev;
    ino_t dest_ino;
} J;
static int io(int fd, void *buf, size_t n, int writing)
{
    char *p = buf;
    while (n) {
        ssize_t k = writing ? write(fd,p,n) : read(fd,p,n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        p += k; n -= (size_t)k;
    }
    return 1;
}
static int send(uint32_t type, const void *buf, uint32_t size)
{
    wslp_head h = { type, size, J.id };
    if (!io(1,&h,sizeof h,1) || (size && !io(1,(void *)buf,size,1))) { J.cancelled=1; return 0; }
    return 1;
}
static uint64_t now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000 + (uint64_t)t.tv_nsec/1000000;
}
static int cancel(void)
{
    struct pollfd f = {0,POLLIN,0};
    if (!J.cancelled && poll(&f,1,0)>0) {
        if (f.revents & (POLLHUP|POLLERR)) J.cancelled=1;
        else if (f.revents&POLLIN) {
            wslp_head h;
            if (!io(0,&h,sizeof h,0) || h.type!=WSLP_JOB_CANCEL || h.size || h.id!=J.id) J.cancelled=1;
            else J.cancelled=1;
        }
    }
    return J.cancelled;
}
static void identify(const char *path, uint32_t *root, const char **suffix)
{
    *root=WSLP_NO_ROOT; *suffix=path?path:""; size_t best=0;
    if (!path) return;
    for(uint32_t i=0;i<=J.job.count;i++) {
        if(!J.roots[i] || !J.roots[i][0])continue;
        size_t n=strlen(J.roots[i]);while(n&&J.roots[i][n-1]=='/')n--;
        if(n>=best && !strncmp(path,J.roots[i],n) && (!path[n] || path[n]=='/')) {
            best=n;*root=i;*suffix=path+n;
        }
    }
}
static void progress(const char *path, int force)
{
    uint64_t n=now_ms(); if(!force && n-J.last_report<50)return;J.last_report=n;
    wslp_progress p=J.p; const char *suffix;
    identify(path,&p.root,&suffix); p.path_bytes=(uint32_t)strlen(suffix);
    size_t size=sizeof p+p.path_bytes;
    char *buf=malloc(size);if(!buf){J.cancelled=1;return;}
    memcpy(buf,&p,sizeof p);memcpy(buf+sizeof p,suffix,p.path_bytes);
    send(WSLP_JOB_PROGRESS,buf,(uint32_t)size);free(buf);
}
static void event(int kind,int error,const char *path)
{
    if(kind==0)J.p.errors++,J.p.error=error;else J.p.skipped++;
    wslp_event e={ (uint32_t)kind,error,0,0 };const char *suffix;
    identify(path,&e.root,&suffix);e.path_bytes=(uint32_t)strlen(suffix);
    size_t size=sizeof e+e.path_bytes;char *buf=malloc(size);
    if(!buf){J.cancelled=1;return;}
    memcpy(buf,&e,sizeof e);memcpy(buf+sizeof e,suffix,e.path_bytes);
    send(WSLP_JOB_EVENT,buf,(uint32_t)size);free(buf);progress(path,1);
}
/* DrvFS exposes the default data stream, not a lossless NTFS stream set.
 * Ask the Windows coordinator at each destructive/publication boundary. */
static int windows_source(const char *path)
{uint32_t root;const char *suffix;identify(path,&root,&suffix);return root!=WSLP_NO_ROOT&&J.windows[root];}
static int windows_source_check(const char *path)
{
    const char *suffix;wslp_source_check q={0};identify(path,&q.root,&suffix);
    if(q.root==WSLP_NO_ROOT||!J.windows[q.root])return 1;
    if(cancel())return 0;
    q.path_bytes=(uint32_t)strlen(suffix);size_t size=sizeof q+q.path_bytes;
    char *buf=malloc(size);if(!buf){event(0,ENOMEM,path);return 0;}
    memcpy(buf,&q,sizeof q);memcpy(buf+sizeof q,suffix,q.path_bytes);
    int sent=send(WSLP_SOURCE_CHECK,buf,(uint32_t)size);free(buf);if(!sent)return 0;
    wslp_head h;int32_t error;
    if(!io(0,&h,sizeof h,0)||h.id!=J.id){J.cancelled=1;return 0;}
    if(h.type==WSLP_JOB_CANCEL&&!h.size){J.cancelled=1;return 0;}
    if(h.type!=WSLP_SOURCE_ACK||h.size!=sizeof error||!io(0,&error,sizeof error,0)){J.cancelled=1;return 0;}
    if(error==ECANCELED){J.cancelled=1;return 0;}
    if(error){event(0,error,path);return 0;}return !cancel();
}
static char *join(const char *a,const char *b)
{
    size_t n=strlen(a),m=strlen(b);if(n+m+2>PATH_CAP){errno=ENAMETOOLONG;return NULL;}
    char *p=malloc(n+m+2);if(!p){errno=ENOMEM;return NULL;}
    memcpy(p,a,n);if(n && a[n-1]!='/')p[n++]='/';memcpy(p+n,b,m+1);return p;
}
static int rename_new(int from,const char *src,int to,const char *dest,int persist)
{
    int r=(int)syscall(SYS_renameat2,from,src,to,dest,RENAME_NOREPLACE);
    if(!r || (errno!=EINVAL && errno!=ENOSYS && errno!=ENOTSUP))return r;
    /* DrvFS rejects RENAME_NOREPLACE. A hard link commits regular files and
     * symlink leaves without overwriting an existing destination. Directories
     * take the copy path, whose mkdir is already exclusive. */
    struct stat st;if(fstatat(from,src,&st,AT_SYMLINK_NOFOLLOW))return -1;
    if(S_ISDIR(st.st_mode)){errno=EXDEV;return -1;}
    if(linkat(from,src,to,dest,0))return -1;
    /* The fallback has two mutations, unlike atomic rename. Persist the new
     * link before deleting the old one; on error keep both names. */
    if(persist){int error=fp_stage_sync(to);if(error){errno=error;return -1;}}
    if(unlinkat(from,src,0))return -1;
    return 0;
}
static char *resolve_windows_path(const char *path)
{
    int p[2]; if(pipe2(p,O_CLOEXEC))return NULL;
    pid_t pid=fork();
    if(!pid) {
        dup2(p[1],1);int null=open("/dev/null",O_WRONLY);if(null>=0)dup2(null,2);
        close(p[0]);close(p[1]);
        execl("/usr/bin/wslpath","wslpath","-u",path,(char *)NULL);_exit(127);
    }
    close(p[1]);if(pid<0){close(p[0]);return NULL;}
    char *s=malloc(PATH_CAP);size_t n=0;
    while(s && n<PATH_CAP-1){ssize_t k=read(p[0],s+n,PATH_CAP-1-n);if(k<0 && errno==EINTR)continue;if(k<=0)break;n+=(size_t)k;}
    close(p[0]);int status=0;while(waitpid(pid,&status,0)<0 && errno==EINTR){}
    if(!s || !WIFEXITED(status)||WEXITSTATUS(status)||!n||n==PATH_CAP-1){free(s);errno=ENOTSUP;return NULL;}
    if(s[n-1]=='\n')n--;s[n]=0;
    if(s[0]!='/'){free(s);errno=EINVAL;return NULL;}return s;
}
/* Resolve each Windows drive once per job, not once per selected file. */
char *fp_windows_path(const char *path)
{
    static char *drives[26];
    if (((path[0]|32)>='a' && (path[0]|32)<='z') && path[1]==':' && (path[2]=='\\'||path[2]=='/')) {
        unsigned at=(unsigned)((path[0]|32)-'a');
        if(!drives[at]) { char drive[4]={path[0],':','\\',0};drives[at]=resolve_windows_path(drive); }
        if(!drives[at])return NULL;
        char *s=join(drives[at],path+3);
        if(s)for(char *p=s;*p;p++)if(*p=='\\')*p='/';
        return s;
    }
    return resolve_windows_path(path);
}
static int parent_flags(const char *path,char **name,int flags)
{
    char *s=strdup(path);if(!s)return -1;
    size_t n=strlen(s);while(n>1 && s[n-1]=='/')s[--n]=0;
    char *slash=strrchr(s,'/');
    if(!slash || !slash[1] || !strcmp(slash+1,".") || !strcmp(slash+1,"..")){free(s);errno=EINVAL;return -1;}
    *name=strdup(slash+1);if(!*name){free(s);return -1;}
    if(slash==s)slash[1]=0;else *slash=0;
    int fd=open(s,flags|O_DIRECTORY|O_CLOEXEC);free(s);
    if(fd<0){free(*name);*name=NULL;}return fd;
}
static int parent(const char *path,char **name){return parent_flags(path,name,O_RDONLY);}
static int unique(int fd,const char *name,int directory,char **out)
{
    const char *dot=directory?NULL:strrchr(name,'.');if(dot==name)dot=NULL;
    size_t stem=dot?(size_t)(dot-name):strlen(name);const char *ext=dot?dot:"";
    for(unsigned i=2;i<100000;i++){
        size_t cap=strlen(name)+32;char *s=malloc(cap);if(!s)return -1;
        snprintf(s,cap,"%.*s (%u)%s",(int)stem,name,i,ext);
        struct stat st;if(fstatat(fd,s,&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT){*out=s;return 0;}free(s);
    }
    errno=EEXIST;return -1;
}
static int same_node(const struct stat *a,const struct stat *b){return a->st_dev==b->st_dev && a->st_ino==b->st_ino;}
static int resolve(int fd,char **name,const struct stat *src,unsigned policy,const char *shown,struct stat *previous)
{
    struct stat dst;
    if(fstatat(fd,*name,&dst,AT_SYMLINK_NOFOLLOW)<0){if(errno==ENOENT)return 1;event(0,errno,shown);return 0;}
    if(policy==0){char *s=NULL;if(unique(fd,*name,S_ISDIR(src->st_mode),&s)){event(0,errno,shown);return 0;}free(*name);*name=s;return 1;}
    if(policy==1){if(S_ISDIR(src->st_mode)&&S_ISDIR(dst.st_mode))return 2;event(1,EEXIST,shown);return 0;}
    if(policy==2){
        if(S_ISDIR(src->st_mode)&&S_ISDIR(dst.st_mode))return 2;
        if((S_ISREG(src->st_mode)&&S_ISREG(dst.st_mode)) || (S_ISLNK(src->st_mode)&&S_ISLNK(dst.st_mode))){*previous=dst;return 3;}
        event(0,S_ISLNK(dst.st_mode)?ELOOP:EEXIST,shown);return 0;
    }
    event(0,EEXIST,shown);return 0;
}
static void scan(int fd,const char *name,const char *path,unsigned depth)
{
    if(cancel())return;struct stat st;
    if(fstatat(fd,name,&st,AT_SYMLINK_NOFOLLOW)){return;}
    J.p.items_total++;
    if(fp_mount_check(fd,name))return;
    if(S_ISREG(st.st_mode))J.p.bytes_total+=(uint64_t)st.st_size;
    progress(path,0);
    if(!S_ISDIR(st.st_mode))return;
    if(depth>256)return;
    int d=fp_mount_open(fd,name,O_RDONLY|O_DIRECTORY);if(d<0)return;
    DIR *dir=fdopendir(d);if(!dir){close(d);return;}
    struct dirent *e;while(!cancel() && (e=readdir(dir))) {
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        char *p=join(path,e->d_name);if(p){scan(d,e->d_name,p,depth+1);free(p);}
    }closedir(dir);progress(path,0);
}
/* A copied regular file is a version, not merely a device/inode pair.
 * Directories change as we remove their own children, so only identity is
 * compared for them. The held descriptor also prevents inode reuse. */
static int same_version(const struct stat *a,const struct stat *b)
{
    return same_node(a,b)&&a->st_size==b->st_size&&a->st_mode==b->st_mode&&a->st_uid==b->st_uid&&a->st_gid==b->st_gid&&
        a->st_mtim.tv_sec==b->st_mtim.tv_sec&&a->st_mtim.tv_nsec==b->st_mtim.tv_nsec&&
        a->st_ctim.tv_sec==b->st_ctim.tv_sec&&a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;
}
static int source_intact(int parent,const char *name,int held,const struct stat *original,int directory)
{
    struct stat fd,path;
    if(held<0||fstat(held,&fd)||fstatat(parent,name,&path,AT_SYMLINK_NOFOLLOW)||
        !(directory?same_node(original,&fd)&&same_node(original,&path):same_version(original,&fd)&&same_version(original,&path))){errno=ESTALE;return 0;}
    return 1;
}
static int remove_copied(int fd,const char *name,int held,const struct stat *original,const char *path,int flags,const fp_meta *meta)
{
    if(cancel()||!windows_source_check(path))return 0;
    int metadata_error=meta&&!windows_source(path)?fp_meta_unchanged(held,meta):0;
    if(metadata_error){event(0,metadata_error,path);return 0;}
    if(!source_intact(fd,name,held,original,flags==AT_REMOVEDIR)){event(0,ESTALE,path);return 0;}
    /* POSIX has no atomic unlink-if-version-matches: a same-UID/root writer
     * can still race the final check and unlink. No stronger claim is made. */
    if(unlinkat(fd,name,flags)){event(0,errno,path);return 0;}
    int error=fp_stage_sync(fd);if(error){event(0,error,path);return 0;}return 1;
}
static int copy_node(int sf,const char *sn,int df,char *dn,const char *sp,const char *dp,unsigned policy,int move,int windows,unsigned depth);
static int delete_node(int fd,const char *name,const char *path,unsigned depth)
{
    if(cancel())return 0;
    int boundary=fp_mount_check(fd,name);
    if(boundary){event(0,boundary,path);J.p.items_done++;return 0;}
    struct stat st;if(fstatat(fd,name,&st,AT_SYMLINK_NOFOLLOW)){event(0,errno,path);return 0;}
    int ok=1;
    if(S_ISDIR(st.st_mode)){
        if(depth>256){event(0,ELOOP,path);return 0;}
        int d=fp_mount_open(fd,name,O_RDONLY|O_DIRECTORY);
        if(d<0){event(0,errno,path);return 0;}
        struct stat opened;if(fstat(d,&opened)||!same_node(&st,&opened)){close(d);event(0,ESTALE,path);return 0;}
        DIR *dir=fdopendir(d);if(!dir){close(d);event(0,errno,path);return 0;}
        struct dirent *e;errno=0;
        while(!cancel() && (e=readdir(dir))) {
            if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
            char *p=join(path,e->d_name);if(!p){event(0,errno,path);ok=0;continue;}
            if(!delete_node(d,e->d_name,p,depth+1))ok=0;free(p);errno=0;
        }
        if(!cancel() && errno){event(0,errno,path);ok=0;}
        struct stat current;if(fstatat(fd,name,&current,AT_SYMLINK_NOFOLLOW)||!same_node(&st,&current))ok=0;
        closedir(dir);
        if(!cancel() && ok && unlinkat(fd,name,AT_REMOVEDIR)){event(0,errno,path);ok=0;}
    }else if(unlinkat(fd,name,0)){event(0,errno,path);ok=0;}
    if(!cancel())J.p.items_done++;progress(path,0);return ok && !cancel();
}
static int preserve(int fd,const struct stat *st,int windows,const char *path,const fp_meta *meta)
{
    if(geteuid()==0 && fchown(fd,st->st_uid,st->st_gid)){event(0,errno,path);return 0;}
    int error=fp_meta_apply(fd,meta,0);if(error){event(0,error,path);return 0;}
    struct timespec ts[2]={st->st_atim,st->st_mtim};mode_t mode=(st->st_mode&07777)|(windows==2?S_IWUSR:0);
    if(fchmod(fd,mode)||futimens(fd,ts)){event(0,errno,path);return 0;}
    error=fp_meta_verify(fd,meta,0);struct stat actual;
    if(!error&&fstat(fd,&actual))error=errno;
    if(!error&&((actual.st_mode&07777)!=mode||actual.st_mtim.tv_sec!=ts[1].tv_sec||actual.st_mtim.tv_nsec!=ts[1].tv_nsec||
        actual.st_atim.tv_sec!=ts[0].tv_sec||actual.st_atim.tv_nsec!=ts[0].tv_nsec||(geteuid()==0&&(actual.st_uid!=st->st_uid||actual.st_gid!=st->st_gid))))error=EIO;
    if(error)event(0,error,path);return !error;
}
/* Windows directories are created/updated through DrvFS, but their checked
 * persistence barrier belongs to the Windows worker. Never descend/remove a
 * source until it acknowledges the directory and its in-job parent. */
#include "fpwinprepare.h"
static int windows_directory_sync(const char *path,const struct stat *metadata)
{
    if(!win_prepare_drain())return 0;
    if(cancel())return 0;
    const char *suffix;wslp_directory d={0};identify(path,&d.root,&suffix);
    if(metadata){d.metadata=1;d.atime=(int64_t)metadata->st_atim.tv_sec*1000000000+metadata->st_atim.tv_nsec;d.mtime=(int64_t)metadata->st_mtim.tv_sec*1000000000+metadata->st_mtim.tv_nsec;}
    d.path_bytes=(uint32_t)strlen(suffix);size_t n=sizeof d+d.path_bytes;
    char *buf=malloc(n);if(!buf){event(0,ENOMEM,path);return 0;}
    memcpy(buf,&d,sizeof d);memcpy(buf+sizeof d,suffix,d.path_bytes);
    int sent=send(WSLP_DIRECTORY_SYNC,buf,(uint32_t)n);free(buf);if(!sent)return 0;
    wslp_head h;int32_t error=0;
    if(!io(0,&h,sizeof h,0)||h.id!=J.id){J.cancelled=1;return 0;}
    if(h.type==WSLP_JOB_CANCEL&&!h.size){J.cancelled=1;return 0;}
    if(h.type!=WSLP_DIRECTORY_ACK||h.size!=sizeof error||!io(0,&error,sizeof error,0)){J.cancelled=1;return 0;}
    if(error==ECANCELED){J.cancelled=1;return 0;}
    if(error){event(0,error,path);return 0;}return !cancel();
}
static int export_file(int in,int sf,const char *sn,const struct stat *st,int replace,const char *sp,const char *dp)
{
    const char *suffix;wslp_file file={.atime=(int64_t)st->st_atim.tv_sec*1000000000+st->st_atim.tv_nsec,
        .mtime=(int64_t)st->st_mtim.tv_sec*1000000000+st->st_mtim.tv_nsec,.replace=(uint32_t)replace};
    identify(dp,&file.root,&suffix);file.path_bytes=(uint32_t)strlen(suffix);
    if(file.root!=0){event(0,EINVAL,dp);return 0;}
    size_t size=sizeof file+file.path_bytes;char *begin=malloc(size),*buf=malloc(65536);
    if(!begin||!buf){free(begin);free(buf);event(0,ENOMEM,dp);return 0;}
    memcpy(begin,&file,sizeof file);memcpy(begin+sizeof file,suffix,file.path_bytes);
    int ok=send(WSLP_FILE_BEGIN,begin,(uint32_t)size),err=0;free(begin);
    uint64_t base=J.p.bytes_done;
    while(ok&&!cancel()){
        ssize_t n=read(in,buf,65536);if(n<0&&errno==EINTR)continue;
        if(n<0){err=errno;ok=0;break;}if(!n)break;
        if(!send(WSLP_FILE_DATA,buf,(uint32_t)n)){ok=0;break;}
        J.p.bytes_done+=(uint64_t)n;progress(sp,0);
    }
    free(buf);
    if(!err&&!cancel()&&!source_intact(sf,sn,in,st,0)){err=ESTALE;ok=0;}
    int32_t result=cancel()?ECANCELED:err;
    if(!send(WSLP_FILE_END,&result,sizeof result))ok=0;
    /* ACK is consumed here, not by the cancellation poll. Even a cancelled
     * stream is drained and aborted by Windows before this process exits. */
    wslp_head ack;int32_t host_error=0;
    if(!io(0,&ack,sizeof ack,0)||ack.id!=J.id){J.cancelled=1;ok=0;}
    else if(ack.type==WSLP_JOB_CANCEL&&!ack.size){J.cancelled=1;ok=0;}
    else if(ack.type!=WSLP_FILE_ACK||ack.size!=sizeof host_error||!io(0,&host_error,sizeof host_error,0)){J.cancelled=1;ok=0;}
    else if(host_error){ok=0;if(!err)err=host_error;}
    if(!ok||J.cancelled){J.p.bytes_done=base;if(err&&!J.cancelled)event(0,err,dp);return 0;}
    return 1;
}
/* Experimental native-destination copy queue. The owner alone touches J,
 * stdin/stdout, source/version guards and publication. Workers only fence a
 * private payload, or fence both namespaces and finish an already published
 * payload. Moves, empty/unallocated files and large files stay serial.
 * OPENPILOT_LINUX_COMMIT_WIDTH=2/4/8 opts in; unset/1 stays serial. */
#define COMMIT_MAX 8
#define COMMIT_FILE_MAX (1024u * 1024)
enum { CQ_IDLE, CQ_PREPARING, CQ_PAYLOAD, CQ_READY, CQ_NAMESPACE, CQ_DONE, CQ_STOP };
typedef struct commit_slot {
    pthread_t thread;
    fp_stage stage;
    fp_meta meta;
    struct stat source, previous;
    int state, error, sf, in, held_previous, replace;
    char *sn, *dn, *sp, *dp;
    uint64_t bytes;
} commit_slot;
static struct {
    pthread_mutex_t lock;
    pthread_cond_t work;
    commit_slot slots[COMMIT_MAX];
    unsigned width, used;
    int notify[2], failed, initialized;
} CQ;
static void *commit_worker(void *arg)
{
    commit_slot *s=arg;
    pthread_mutex_lock(&CQ.lock);
    for(;;){
        while(s->state!=CQ_PAYLOAD&&s->state!=CQ_NAMESPACE&&s->state!=CQ_STOP)
            pthread_cond_wait(&CQ.work,&CQ.lock);
        int phase=s->state;if(phase==CQ_STOP)break;
        pthread_mutex_unlock(&CQ.lock);
        int error=phase==CQ_PAYLOAD?fp_stage_sync_payload(&s->stage):
            fp_stage_sync_pair(s->stage.parent,s->stage.dir);
        if(phase==CQ_NAMESPACE){int cleanup=fp_stage_finish(&s->stage);if(!error)error=cleanup;}
        pthread_mutex_lock(&CQ.lock);
        s->error=error;s->state=phase==CQ_PAYLOAD?CQ_READY:CQ_DONE;
        /* Nonblocking notifications are level-triggered hints. A full pipe
         * already wakes the owner, which scans every slot under this lock. */
        char wake=0;ssize_t n;do n=write(CQ.notify[1],&wake,1);while(n<0&&errno==EINTR);
    }
    pthread_mutex_unlock(&CQ.lock);return NULL;
}
static void commit_release(commit_slot *s)
{
    fp_meta_free(&s->meta);
    if(s->sf>=0)close(s->sf);if(s->in>=0)close(s->in);
    if(s->held_previous>=0)close(s->held_previous);
    free(s->sn);free(s->dn);free(s->sp);free(s->dp);
    s->sn=s->dn=s->sp=s->dp=NULL;s->sf=s->in=s->held_previous=-1;
    pthread_mutex_lock(&CQ.lock);s->state=CQ_IDLE;pthread_mutex_unlock(&CQ.lock);
}
/* Called only on the job owner; no worker owns READY or DONE records. */
static void commit_service(void)
{
    for(unsigned i=0;i<CQ.width;i++){
        commit_slot *s=&CQ.slots[i];
        pthread_mutex_lock(&CQ.lock);int phase=s->state;pthread_mutex_unlock(&CQ.lock);
        if(phase!=CQ_READY&&phase!=CQ_DONE)continue;
        int error=s->error;
        if(phase==CQ_READY){
            if(!error&&!cancel()&&!windows_source_check(s->sp))error=ECANCELED;
            if(!error&&!cancel()&&!windows_source(s->sp))error=fp_meta_unchanged(s->in,&s->meta);
            if(!error&&!cancel()&&!source_intact(s->sf,s->sn,s->in,&s->source,0))error=ESTALE;
            if(!error&&!cancel()&&s->replace&&!source_intact(s->stage.parent,s->dn,s->held_previous,&s->previous,0))error=ESTALE;
            if(!error&&!cancel())error=fp_stage_rename(&s->stage,s->dn,s->replace);
            if(!error&&!cancel()&&!s->stage.published)error=ECANCELED;
            /* A cancellation after rename still owes both namespace fences. */
            if(s->stage.published){
                pthread_mutex_lock(&CQ.lock);s->state=CQ_NAMESPACE;pthread_cond_broadcast(&CQ.work);pthread_mutex_unlock(&CQ.lock);
                continue;
            }
            int cleanup=fp_stage_finish(&s->stage);if(!error)error=cleanup;
        }
        if(error||cancel()){
            CQ.failed=1;
            /* A rejected Windows source ACK has already emitted its event. */
            if(error&&error!=ECANCELED&&!J.cancelled)event(0,error,s->dp);
        }else{J.p.bytes_done+=s->bytes;J.p.items_done++;progress(s->sp,0);}
        if(error&&!cancel())J.p.items_done++;
        CQ.used--;commit_release(s);
    }
}
static void commit_wait(void)
{
    char bytes[64];while(read(CQ.notify[0],bytes,sizeof bytes)>0){}
    commit_service();if(!CQ.used)return;
    struct pollfd f[2]={{CQ.notify[0],POLLIN,0},{0,POLLIN,0}};
    /* cancel() is the sole reader of job control messages while waiting. */
    int n;do n=poll(f,J.cancelled?1:2,-1);while(n<0&&errno==EINTR);
    if(n<0){J.cancelled=1;return;}cancel();commit_service();
}
static int commit_drain(void)
{
    while(CQ.used){commit_service();if(CQ.used)commit_wait();}
    return !CQ.failed;
}
/* Cached workspaces must be removed before directory timestamps/ACLs are
 * finalized, just as the serial workspace is retired before that boundary. */
static int commit_retire(void)
{
    int ok=commit_drain();
    for(unsigned i=0;i<CQ.width;i++){
        int error=fp_stage_dispose(&CQ.slots[i].stage);
        if(error){CQ.failed=1;ok=0;if(!cancel())event(0,error,J.roots[0]);}
    }
    return ok;
}
static int commit_start(void)
{
    const char *value=getenv("OPENPILOT_LINUX_COMMIT_WIDTH");
    unsigned width=value&&!strcmp(value,"2")?2:value&&!strcmp(value,"4")?4:value&&!strcmp(value,"8")?8:0;
    if(!width||J.windows[0]||(J.job.op!=0&&J.job.op!=WSLP_OP_COPY_FILE))return 0;
    int error=pthread_mutex_init(&CQ.lock,NULL);if(error)return error;
    error=pthread_cond_init(&CQ.work,NULL);if(error){pthread_mutex_destroy(&CQ.lock);return error;}
    if(pipe2(CQ.notify,O_CLOEXEC|O_NONBLOCK)){error=errno;pthread_cond_destroy(&CQ.work);pthread_mutex_destroy(&CQ.lock);return error;}
    CQ.initialized=1;
    for(unsigned i=0;i<width;i++){
        commit_slot *s=&CQ.slots[i];fp_stage_init(&s->stage);s->sf=s->in=s->held_previous=-1;
        error=pthread_create(&s->thread,NULL,commit_worker,s);if(error)break;CQ.width++;
    }
    return error;
}
static void commit_stop(void)
{
    if(!CQ.initialized)return;
    commit_drain();
    pthread_mutex_lock(&CQ.lock);for(unsigned i=0;i<CQ.width;i++)CQ.slots[i].state=CQ_STOP;
    pthread_cond_broadcast(&CQ.work);pthread_mutex_unlock(&CQ.lock);
    for(unsigned i=0;i<CQ.width;i++){
        commit_slot *s=&CQ.slots[i];pthread_join(s->thread,NULL);
        int error=fp_stage_dispose(&s->stage);if(error&&!cancel())event(0,error,J.roots[0]);
    }
    CQ.width=0;CQ.initialized=0;close(CQ.notify[0]);close(CQ.notify[1]);
    pthread_cond_destroy(&CQ.work);pthread_mutex_destroy(&CQ.lock);
}
static commit_slot *commit_reserve(int sf,const char *sn,int df,const char *dn,const struct stat *st,int previous,const struct stat *dst,int replace,const char *sp,const char *dp)
{
    commit_slot *s=NULL;
    while(!s&&!cancel()){
        commit_service();if(cancel())break;
        pthread_mutex_lock(&CQ.lock);
        for(unsigned i=0;i<CQ.width;i++)if(CQ.slots[i].state==CQ_IDLE){s=&CQ.slots[i];s->state=CQ_PREPARING;break;}
        pthread_mutex_unlock(&CQ.lock);
        if(!s)commit_wait();
    }
    if(!s){errno=ECANCELED;return NULL;}
    s->error=0;s->bytes=0;s->replace=replace;s->source=*st;s->previous=*dst;
    s->sn=strdup(sn);s->dn=strdup(dn);s->sp=strdup(sp);s->dp=strdup(dp);s->sf=fcntl(sf,F_DUPFD_CLOEXEC,0);
    if(previous>=0)s->held_previous=fcntl(previous,F_DUPFD_CLOEXEC,0);
    int error=!s->sn||!s->dn||!s->sp||!s->dp?ENOMEM:s->sf<0||(previous>=0&&s->held_previous<0)?errno:0;
    if(!error)error=fp_stage_reuse(&s->stage,df,NULL);
    if(error){fp_stage_dispose(&s->stage);commit_release(s);errno=error;return NULL;}return s;
}
/* Pending publications must become visible before resolving another name in
 * the same directory; duplicate selections and auto-numbering remain serial. */
static void commit_conflict(int df,const char *name)
{
    if(!CQ.used)return;
    struct stat parent;if(fstat(df,&parent)){commit_drain();return;}
    for(unsigned i=0;i<CQ.width;i++){
        commit_slot *s=&CQ.slots[i];
        pthread_mutex_lock(&CQ.lock);int phase=s->state;pthread_mutex_unlock(&CQ.lock);
        if(phase!=CQ_IDLE&&s->dn&&!strcmp(name,s->dn)){
            struct stat held;if(fstat(s->stage.parent,&held)||fp_stage_same(&parent,&held)){commit_drain();return;}
        }
    }
}
static int copy_file(int sf,const char *sn,int df,const char *dn,const struct stat *st,int in,int previous,const struct stat *dst,int replace,int windows,const char *sp,const char *dp,int *retained,fp_meta *retained_meta)
{
    if(retained)*retained=-1;
    if(in<0)in=fp_mount_open(sf,sn,O_RDONLY);if(in<0){event(0,errno,sp);return 0;}
    if(!source_intact(sf,sn,in,st,0)){close(in);event(0,ESTALE,sp);return 0;}
    fp_meta meta={0};int err=windows_source(sp)?0:fp_meta_fd(in,&meta);
    if(!err)err=fp_meta_valid(meta.data,meta.size,0);
    if(!err&&windows==1&&meta.size)err=ENOTSUP;
    if(err){fp_meta_free(&meta);close(in);event(0,err,sp);return 0;}
    if(windows==1){
        int queued=WQ.width&&!retained&&!replace&&!windows_source(sp)&&st->st_blocks>0&&st->st_size>0&&
            (uint64_t)st->st_size<=WSLP_PREPARE_BYTES&&win_prepare_ascii(dn);
        int copied=queued?win_prepare_file(in,sf,sn,dn,st,sp,dp):
            win_prepare_drain()?export_file(in,sf,sn,st,replace,sp,dp):0;
        fp_meta_free(&meta);if(copied&&retained)*retained=in;else close(in);return copied;
    }
    int queued=CQ.width&&!retained&&windows==0&&st->st_blocks>0&&st->st_size>0&&(uint64_t)st->st_size<=COMMIT_FILE_MAX;
    commit_slot *slot=queued?commit_reserve(sf,sn,df,dn,st,previous,dst,replace,sp,dp):NULL;
    fp_stage *stage=queued?(slot?&slot->stage:NULL):&J.stage;
    err=queued?(slot?0:errno):fp_stage_reuse(stage,df,NULL);
    if(err){fp_meta_free(&meta);close(in);event(0,err,dp);return 0;}
    char *buf=malloc(CHUNK);int ok=buf!=NULL;if(!ok)err=ENOMEM;uint64_t base=J.p.bytes_done;
    /* Windows regular-file lengths are authoritative for this held version.
     * Avoid a separate DrvFS EOF round trip; the unchanged source guards below
     * still reject growth, truncation, replacement or edits before publication.
     * Native Linux pseudo-files keep their streaming-to-EOF behavior. */
    int bounded=windows_source(sp)&&st->st_size>=0;uint64_t remaining=(uint64_t)st->st_size;
    while(ok && (!bounded||remaining) && !cancel()) {
        size_t size=bounded&&remaining<CHUNK?(size_t)remaining:CHUNK;
        ssize_t n=read(in,buf,size);if(n<0 && errno==EINTR)continue;
        if(n<0){err=errno;ok=0;break;}if(!n){if(bounded&&remaining){err=ESTALE;ok=0;}break;}
        if(bounded)remaining-=(uint64_t)n;
        if(queued&&slot->bytes+(uint64_t)n>COMMIT_FILE_MAX){err=EFBIG;ok=0;break;}
        if(!io(stage->file,buf,(size_t)n,1)){err=errno;ok=0;break;}
        if(queued)slot->bytes+=(uint64_t)n;else J.p.bytes_done+=(uint64_t)n;progress(sp,0);
    }
    free(buf);
    if(ok&&!cancel()&&!source_intact(sf,sn,in,st,0)){err=ESTALE;ok=0;}
    if(ok && !cancel())ok=preserve(stage->file,st,windows,dp,&meta);
    if(queued&&ok&&!cancel()){
        slot->in=in;slot->meta=meta;CQ.used++;
        pthread_mutex_lock(&CQ.lock);slot->state=CQ_PAYLOAD;pthread_cond_broadcast(&CQ.work);pthread_mutex_unlock(&CQ.lock);
        return 2; /* Admitted, not completed: owner accounts after all barriers. */
    }
    if(queued){
        int cleanup=fp_stage_finish(stage);if(!err)err=cleanup;
        commit_release(slot);fp_meta_free(&meta);close(in);
        if(err&&!cancel())event(0,err,dp);return 0;
    }
    if(ok&&!cancel()){err=fp_stage_sync_payload(stage);if(err)ok=0;}
    if(ok&&!cancel()&&!windows_source_check(sp))ok=0;
    if(ok&&!cancel()&&!windows_source(sp)){err=fp_meta_unchanged(in,&meta);if(err)ok=0;}
    if(ok&&!cancel()&&!source_intact(sf,sn,in,st,0)){err=ESTALE;ok=0;}
    /* The flush can block while another writer edits or replaces the target.
     * Keep the originally resolved inode alive and validate its version after
     * every blocking preparation step, immediately before publication. */
    if(ok&&!cancel()&&replace&&!source_intact(df,dn,previous,dst,0)){err=ESTALE;ok=0;}
    if(ok&&!cancel()){err=fp_stage_publish(stage,dn,replace);if(err)ok=0;}
    int cleanup=fp_stage_finish(stage);if(cleanup&&ok){err=cleanup;ok=0;}
    if(!ok||cancel()){
        fp_meta_free(&meta);close(in);J.p.bytes_done=base;
        if(err&&!cancel())event(0,err,dp);return 0;
    }
    if(retained_meta)*retained_meta=meta;else fp_meta_free(&meta);
    if(retained)*retained=in;else close(in);
    return 1;
}
static int copy_node_opened(int sf,const char *sn,int df,char *requested,const char *sp,const char *dest_path,unsigned policy,int move,int windows,unsigned depth,struct stat st,int *opened)
{
    if(WQ.failed)return 0;
    if(WQ.used&&(!S_ISREG(st.st_mode)||st.st_size<=0||(uint64_t)st.st_size>WSLP_PREPARE_BYTES||!st.st_blocks))win_prepare_drain();
    if(J.job.op==WSLP_OP_COPY_FILE&&!S_ISREG(st.st_mode)){event(0,ENOTSUP,sp);return 0;}
    if(!windows_source_check(sp)){J.p.items_done++;return 0;}
    if(depth>256 || (S_ISDIR(st.st_mode)&&st.st_dev==J.dest_dev&&st.st_ino==J.dest_ino)){event(0,ELOOP,sp);return 0;}
    char *dn=strdup(requested);if(!dn){event(0,ENOMEM,sp);return 0;}
    struct stat ds;if(!fstatat(df,dn,&ds,AT_SYMLINK_NOFOLLOW) && same_node(&st,&ds)) {
        if(move){event(1,EEXIST,sp);free(dn);return 0;}policy=0;
    }
    char *candidate=join(dest_path,dn);if(!candidate){free(dn);event(0,errno,dest_path);return 0;}
    commit_conflict(df,dn);
    win_prepare_conflict(dn);
    if(CQ.used&&policy==0&&!fstatat(df,dn,&ds,AT_SYMLINK_NOFOLLOW))commit_drain();
    if(WQ.used&&policy==0&&!fstatat(df,dn,&ds,AT_SYMLINK_NOFOLLOW))win_prepare_drain();
    if(WQ.failed){free(dn);free(candidate);return 0;}
    struct stat previous_stat={0};int previous=-1;
    int r=resolve(df,&dn,&st,policy,candidate,&previous_stat);free(candidate);
    char *dp=join(dest_path,dn);if(!dp){free(dn);event(0,errno,dest_path);return 0;}
    if(!r){J.p.items_done++;if(S_ISREG(st.st_mode))J.p.bytes_done+=(uint64_t)st.st_size;free(dn);free(dp);return 0;}
    if(r==3){
        previous=fp_mount_open(df,dn,O_PATH);
        if(!source_intact(df,dn,previous,&previous_stat,0)){
            if(previous>=0)close(previous);event(0,ESTALE,dp);free(dn);free(dp);return 0;
        }
    }
    if(move && r!=2) {
        int renamed=r==3?renameat(sf,sn,df,dn):rename_new(sf,sn,df,dn,1);
        if(!renamed){
            /* Atomic same-volume moves fence namespace changes only; they do
             * not recursively flush pre-existing file contents. */
            int error=fp_stage_sync_pair(df,sf);if(error)event(0,error,dp);
            J.p.items_done++;if(S_ISREG(st.st_mode))J.p.bytes_done+=(uint64_t)st.st_size;if(previous>=0)close(previous);free(dn);free(dp);progress(sp,0);return !error;
        }
        if(errno!=EXDEV){event(0,errno,sp);if(previous>=0)close(previous);free(dn);free(dp);return 0;}
    }
    int ok=1;fp_meta meta={0};
    if(S_ISDIR(st.st_mode)) {
        if(!commit_retire())ok=0;
        int sd=fp_mount_open(sf,sn,O_RDONLY|O_DIRECTORY);
        if(sd<0){event(0,errno,sp);ok=0;goto done;}
        int metadata_error=windows_source(sp)?0:fp_meta_fd(sd,&meta);
        if(!metadata_error)metadata_error=fp_meta_valid(meta.data,meta.size,1);
        if(!metadata_error&&windows==1&&meta.size)metadata_error=ENOTSUP;
        if(metadata_error){event(0,metadata_error,sp);close(sd);ok=0;goto done;}
        if(r!=2 && mkdirat(df,dn,0700)){event(0,errno,dp);close(sd);ok=0;goto done;}
        int dd=fp_mount_open(df,dn,O_RDONLY|O_DIRECTORY);
        if(dd<0){event(0,errno,dp);close(sd);ok=0;goto done;}
        struct stat opened;if(fstat(sd,&opened)||!same_node(&st,&opened)){event(0,ESTALE,sp);close(sd);close(dd);ok=0;goto done;}
        if(windows!=1){
            metadata_error=0;
            if(r!=2)metadata_error=fp_meta_apply(dd,&meta,1);
            else if(meta.size){fp_meta existing={0};metadata_error=fp_meta_fd(dd,&existing);if(!metadata_error&&!fp_meta_equal(&meta,&existing))metadata_error=ENOTSUP;fp_meta_free(&existing);}
            if(metadata_error){event(0,metadata_error,dp);close(sd);close(dd);ok=0;goto done;}
        }
        /* A child can lose its source name before this directory finishes.
         * Existing merge entries may also be recent: fence every entered
         * destination directory and parent before recursion. */
        if(windows!=1){int error=fp_stage_sync_pair(dd,df);if(error){event(0,error,dp);close(sd);close(dd);ok=0;goto done;}}
        if(windows==1&&!windows_directory_sync(dp,NULL)){close(sd);close(dd);ok=0;goto done;}
        if(!depth){struct stat t;if(!fstat(dd,&t)){J.dest_dev=t.st_dev;J.dest_ino=t.st_ino;}}
        DIR *dir=fdopendir(sd);if(!dir){event(0,errno,sp);close(sd);close(dd);ok=0;goto done;}
        struct dirent *e;errno=0;
        while(!cancel() && (e=readdir(dir))) {
            if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
            char *a=join(sp,e->d_name);if(!a){event(0,errno,sp);ok=0;continue;}
            if(!copy_node(sd,e->d_name,dd,e->d_name,a,dp,J.job.policy,move,windows,depth+1))ok=0;
            free(a);errno=0;
        }
        if(!cancel() && errno){event(0,errno,sp);ok=0;}
        if(!commit_retire())ok=0;
        int cleanup=fp_stage_dispose(&J.stage);if(cleanup){event(0,cleanup,dp);ok=0;}
        if(!cancel()&&!windows_source_check(sp))ok=0;
        if(!cancel()&&!windows_source(sp)){metadata_error=fp_meta_unchanged(sd,&meta);if(metadata_error){event(0,metadata_error,sp);ok=0;}}
        if(r!=2 && !cancel() && windows!=1 && !preserve(dd,&st,windows,dp,&meta))ok=0;
        if(r==2&&!cancel()&&windows!=1&&meta.size){metadata_error=fp_meta_verify(dd,&meta,0);if(metadata_error){event(0,metadata_error,dp);ok=0;}}
        if(!cancel() && windows!=1){int error=fp_stage_sync_pair(dd,df);if(error){event(0,error,dp);ok=0;}}
        /* Close DrvFS before Windows applies the final timestamps: its
         * futimens path can truncate fractional seconds. The Windows worker
         * applies/verifies metadata and flushes before the removal ACK. */
        if(close(dd)&&windows==1){event(0,errno,dp);ok=0;}
        if(!cancel() && windows==1&&!windows_directory_sync(dp,r!=2?&st:NULL))ok=0;
        if(move && !cancel() && ok)ok=remove_copied(sf,sn,sd,&st,sp,AT_REMOVEDIR,&meta);
        closedir(dir);
    } else if(S_ISREG(st.st_mode)) {
        int held=-1,input=*opened;*opened=-1;ok=copy_file(sf,sn,df,dn,&st,input,previous,&previous_stat,r==3,windows,sp,dp,move?&held:NULL,move?&meta:NULL);
        if(ok && move)ok=remove_copied(sf,sn,held,&st,sp,0,&meta);
        if(held>=0)close(held);
    } else if(S_ISLNK(st.st_mode)) {
        commit_drain();
        int held=fp_mount_open(sf,sn,O_PATH);
        if(!source_intact(sf,sn,held,&st,0)){event(0,ESTALE,sp);if(held>=0)close(held);ok=0;goto done;}
        int metadata_error=windows_source(sp)?0:fp_meta_link(sf,sn,&meta);
        if(metadata_error){event(0,metadata_error,sp);close(held);ok=0;goto done;}
        char link[PATH_CAP];ssize_t n=readlinkat(sf,sn,link,sizeof link-1);
        if(n<0){event(0,errno,sp);ok=0;}else {
            link[n]=0;fp_stage *stage=&J.stage;int err=fp_stage_reuse(stage,df,link);
            if(!err){
                struct timespec ts[2]={st.st_atim,st.st_mtim};
                if((windows!=1 && geteuid()==0 && fchownat(stage->dir,"payload",st.st_uid,st.st_gid,AT_SYMLINK_NOFOLLOW))||
                    utimensat(stage->dir,"payload",ts,AT_SYMLINK_NOFOLLOW))err=errno;
                if(!err)err=fp_stage_sync_payload(stage);
                if(!err&&!source_intact(sf,sn,held,&st,0))err=ESTALE;
                if(!err&&r==3&&!source_intact(df,dn,previous,&previous_stat,0))err=ESTALE;
                if(!err)err=fp_stage_publish(stage,dn,r==3);
                int cleanup=fp_stage_finish(stage);if(!err)err=cleanup;
            }
            if(err){event(0,err,dp);ok=0;}
            else {
                if(windows==1)ok=windows_directory_sync(dest_path,NULL);
                if(ok&&move)ok=remove_copied(sf,sn,held,&st,sp,0,NULL);
            }
        }
        close(held);
    } else {event(0,ENOTSUP,sp);ok=0;}
done:
    fp_meta_free(&meta);if(previous>=0)close(previous);
    if(ok!=2&&!cancel())J.p.items_done++;progress(sp,0);free(dn);free(dp);return ok && !cancel();
}

static int copy_node(int sf,const char *sn,int df,char *requested,const char *sp,const char *dest_path,unsigned policy,int move,int windows,unsigned depth)
{
    if(cancel())return 0;
    struct stat st;if(fstatat(sf,sn,&st,AT_SYMLINK_NOFOLLOW)){event(0,errno,sp);return 0;}
    /* A successful checked read open also performs the leaf mount check.
     * Retain it for the copy instead of closing an O_PATH probe and opening
     * the same source again. Moves retain their pre-rename mount check. */
    /* Skip policy must not eagerly request data access for existing targets. */
    int opened=!move&&policy!=1&&S_ISREG(st.st_mode)?fp_mount_open(sf,sn,O_RDONLY):-1;
    int boundary=opened>=0?0:fp_mount_check(sf,sn);
    if(boundary){event(0,boundary,sp);J.p.items_done++;return 0;}
    /* If a read open failed, preserve the old mount/error/skip ordering and
     * let the copy path attempt the read only after conflict resolution. */
    int ok=copy_node_opened(sf,sn,df,requested,sp,dest_path,policy,move,windows,depth,st,&opened);
    if(opened>=0)close(opened);return ok;
}
/* User-owned trash directories; never fall back to permanent deletion. */
static int private_dir(const char *path)
{
    if(mkdir(path,0700) && errno!=EEXIST)return 0;
    struct stat st;
    if(lstat(path,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&0077)){errno=EACCES;return 0;}
    return 1;
}
static int parents(const char *path)
{
    char *s=strdup(path);if(!s)return 0;
    for(char *p=s+1;*p;p++)if(*p=='/'){
        *p=0;if(mkdir(s,0700)&&errno!=EEXIST){free(s);return 0;}*p='/';
    }
    free(s);return 1;
}
static char *trash_root(const char *source,int sf,int *relative)
{
    struct stat src; if(fstat(sf,&src))return NULL;
    const char *data=getenv("XDG_DATA_HOME"),*home=getenv("HOME");
    char *base=data && data[0]=='/'?strdup(data):home && home[0]=='/'?join(home,".local/share"):NULL;
    char *root=base?join(base,"Trash"):NULL;free(base);
    if(root && parents(root) && private_dir(root)) {
        struct stat st;if(!stat(root,&st)&&st.st_dev==src.st_dev){*relative=0;return root;}
    }free(root);
    /* Find this volume's top directory, as opposed to the filesystem's /.
     * A private .Trash-uid is accepted only on the source volume. */
    char *name=NULL;int fd=parent(source,&name);free(name);if(fd<0)return NULL;
    char proc[64];snprintf(proc,sizeof proc,"/proc/self/fd/%d",fd);
    char *top=realpath(proc,NULL);close(fd);if(!top)return NULL;
    for(;;){
        char *up=join(top,"..");char *resolved=up?realpath(up,NULL):NULL;free(up);
        if(!resolved)break;struct stat st;
        if(!strcmp(top,resolved)||stat(resolved,&st)||st.st_dev!=src.st_dev){free(resolved);break;}
        free(top);top=resolved;
    }
    char suffix[48];snprintf(suffix,sizeof suffix,".Trash-%u",(unsigned)getuid());
    root=join(top,suffix);free(top);
    if(!root||!private_dir(root)){free(root);return NULL;}
    struct stat st;if(stat(root,&st)||st.st_dev!=src.st_dev){free(root);errno=EXDEV;return NULL;}
    *relative=1;return root;
}
static char *encode_path(const char *path)
{
    static const char hex[]="0123456789ABCDEF";
    size_t n=strlen(path);char *s=malloc(n*3+1),*p=s;if(!s)return NULL;
    for(const unsigned char *c=(const unsigned char *)path;*c;c++){
        if((*c>='a'&&*c<='z')||(*c>='A'&&*c<='Z')||(*c>='0'&&*c<='9')||strchr("/-._~",*c))*p++=(char)*c;
        else {*p++='%';*p++=hex[*c>>4];*p++=hex[*c&15];}
    }*p=0;return s;
}
static int recycle(int sf,const char *name,const char *path)
{
    if(cancel())return 0;
    int relative=0;char *root=trash_root(path,sf,&relative);
    if(!root){event(0,errno?errno:EACCES,path);return 0;}
    char *files=join(root,"files"),*infos=join(root,"info");
    int ff=-1,inf=-1,ok=0;
    if(!files||!infos||!private_dir(files)||!private_dir(infos))goto done;
    ff=open(files,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);inf=open(infos,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(ff<0||inf<0)goto done;
    const char *stored=path;
    if(relative){
        char *slash=strrchr(root,'/');size_t n=slash?(size_t)(slash-root):0;
        if(!strncmp(path,root,n)&&path[n]=='/')stored=path+n+1;
    }
    char *encoded=encode_path(stored);if(!encoded)goto done;
    for(unsigned i=0;i<100000 && !cancel();i++){
        char dest[256],meta[272];
        snprintf(dest,sizeof dest,"%.200s%s%u",name,i?".":".",i+1);
        snprintf(meta,sizeof meta,"%s.trashinfo",dest);
        struct stat st;if(!fstatat(ff,dest,&st,AT_SYMLINK_NOFOLLOW))continue;
        int fd=openat(inf,meta,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(fd<0){if(errno==EEXIST)continue;break;}
        time_t t=time(NULL);struct tm tm;localtime_r(&t,&tm);char date[32];strftime(date,sizeof date,"%Y-%m-%dT%H:%M:%S",&tm);
        size_t cap=strlen(encoded)+100;char *text=malloc(cap);
        int written=text?snprintf(text,cap,"[Trash Info]\nPath=%s\nDeletionDate=%s\n",encoded,date):-1;
        int err=written<0?ENOMEM:!io(fd,text,(size_t)written,1)?errno:fsync(fd)?errno:0;
        free(text);if(close(fd)&&!err)err=errno;
        if(!err && !cancel() && !rename_new(sf,name,ff,dest,0)){ok=1;break;}
        if(!err)err=errno;unlinkat(inf,meta,0);errno=err;
        if(err!=EEXIST || cancel())break;
    }free(encoded);
done:
    if(!ok && !cancel())event(0,errno?errno:EACCES,path);
    if(ff>=0)close(ff);if(inf>=0)close(inf);
    free(root);free(files);free(infos);
    if(!cancel())J.p.items_done++;progress(path,1);return ok;
}
static int within(const char *child,const char *ancestor)
{
    size_t n=strlen(ancestor);return !strncmp(child,ancestor,n) && (!child[n]||child[n]=='/'||!strcmp(ancestor,"/"));
}
static int protect_destination(const char *source,const char *dest)
{
    struct stat st;if(lstat(source,&st)||!S_ISDIR(st.st_mode))return 1;
    char *a=realpath(source,NULL),*b=realpath(dest,NULL);
    int safe=a&&b&&!within(b,a);free(a);free(b);
    if(!safe)errno=EINVAL;return safe;
}
static int take_path(const uint8_t **p,size_t *left,uint32_t at)
{
    wslp_path h;if(*left<sizeof h)return 0;memcpy(&h,*p,sizeof h);*p+=sizeof h;*left-=sizeof h;
    if(h.bytes>=PATH_CAP||h.bytes>*left||h.flags>WSLP_PATH_WINDOWS||memchr(*p,0,h.bytes))return 0;
    char *s=malloc((size_t)h.bytes+1);if(!s)return 0;memcpy(s,*p,h.bytes);s[h.bytes]=0;
    *p+=h.bytes;*left-=h.bytes;J.windows[at]=h.flags;
    if(at && h.flags && h.bytes==3 && s[1]==':' && (s[2]=='\\'||s[2]=='/')){free(s);errno=EINVAL;return 0;}
    if(h.bytes && h.flags){J.roots[at]=fp_windows_path(s);free(s);if(!J.roots[at])return 0;}
    else J.roots[at]=s;
    if(h.bytes && J.roots[at][0]!='/'){errno=EINVAL;return 0;}return 1;
}
static char *physical(int fd)
{
    char alias[64],*s=malloc(PATH_CAP);if(!s){errno=ENOMEM;return NULL;}
    snprintf(alias,sizeof alias,"/proc/self/fd/%d",fd);ssize_t n=readlink(alias,s,PATH_CAP-1);
    if(n<0||n>=PATH_CAP-1){free(s);if(n>=0)errno=ENAMETOOLONG;return NULL;}s[n]=0;
    /* A detached parent has no usable pathname for a symbolic link. */
    struct stat a,b;
    if(s[0]!='/'||fstat(fd,&a)||stat(s,&b)||a.st_dev!=b.st_dev||a.st_ino!=b.st_ino){free(s);errno=ESTALE;return NULL;}
    return s;
}
static char *relative_target(const char *target,const char *directory)
{
    /* Both inputs are physical absolute paths with no dot segments. Match
     * entire, case-sensitive components, never just a textual prefix. */
    size_t common=1,i=1;
    while(target[i]&&directory[i]&&target[i]==directory[i]){if(target[i]=='/')common=i+1;i++;}
    if(!directory[i]&&(target[i]=='/'||!target[i]))common=i+(target[i]=='/');
    else if(!target[i]&&directory[i]=='/')common=i;
    unsigned up=0;const char *rest=directory+common;
    if(common>strlen(directory))rest=directory+strlen(directory);
    for(const char *p=rest;*p;p++)if(*p!='/'&&(p==rest||p[-1]=='/'))up++;
    const char *tail=target+common;size_t size=(size_t)up*3+strlen(tail)+2;
    if(size>PATH_CAP){errno=ENAMETOOLONG;return NULL;}
    char *s=malloc(size);if(!s){errno=ENOMEM;return NULL;}char *p=s;
    for(unsigned n=0;n<up;n++){memcpy(p,"../",3);p+=3;}
    if(!*tail&&up)p[-1]=0;else strcpy(p,*tail?tail:".");return s;
}
static void run_link(void)
{
    char *source=NULL,*name=NULL,*from=NULL,*to=NULL,*absolute=NULL,*target=NULL;
    int sf=-1,df=-1;struct stat st;
    J.p.state=1;J.p.items_total=1;progress(NULL,1);
    if(J.windows[0]){event(0,ENOTSUP,J.roots[0]);goto finish;}
    sf=parent_flags(J.roots[1],&source,O_PATH);
    if(sf<0){event(0,errno,J.roots[1]);goto finish;}
    df=parent_flags(J.roots[0],&name,O_PATH);
    if(df<0){event(0,errno,J.roots[0]);goto finish;}
    if(fstatat(sf,source,&st,AT_SYMLINK_NOFOLLOW)){event(0,errno,J.roots[1]);goto finish;}
    if(!S_ISREG(st.st_mode)&&!S_ISDIR(st.st_mode)&&!S_ISLNK(st.st_mode)){event(0,ENOTSUP,J.roots[1]);goto finish;}
    from=physical(sf);if(!from){event(0,errno,J.roots[1]);goto finish;}
    to=physical(df);if(!to){event(0,errno,J.roots[0]);goto finish;}
    absolute=join(from,source);
    if(!absolute){event(0,ENOMEM,J.roots[1]);goto finish;}
    target=J.job.op==WSLP_OP_SYMLINK_RELATIVE?relative_target(absolute,to):strdup(absolute);
    if(!target){event(0,errno?errno:ENOMEM,J.roots[0]);goto finish;}
    J.p.state=2;progress(NULL,1);
    wslp_head ready;
    if(!io(0,&ready,sizeof ready,0)||ready.id!=J.id||ready.type!=WSLP_JOB_CONTINUE||ready.size)J.cancelled=1;
    if(!cancel()){
        /* Atomic nonreplacement, including an existing broken symlink. */
        if(symlinkat(target,df,name))event(0,errno,J.roots[0]);else J.p.items_done=1;
    }
finish:
    if(sf>=0)close(sf);if(df>=0)close(df);
    free(source);free(name);free(from);free(to);free(absolute);free(target);
    J.p.state=cancel()?4:3;progress(NULL,1);
}
static void run(void)
{
    unsigned op=J.job.op==WSLP_OP_COPY_FILE?0:J.job.op; int df=-1;struct stat ds={0};
    if(op>=WSLP_OP_SYMLINK_RELATIVE){run_link();return;}
    if(op<=1){
        df=open(J.roots[0],O_RDONLY|O_DIRECTORY|O_CLOEXEC);
        if(df<0){event(0,errno,J.roots[0]);J.p.state=5;progress(NULL,1);return;}
        fstat(df,&ds);
    }
    J.p.state=1;progress(NULL,1);
    for(uint32_t i=1;i<=J.job.count && !cancel();i++){
        char *name=NULL;int sf=parent(J.roots[i],&name);
        if(sf<0){event(0,errno,J.roots[i]);J.p.items_total++;J.p.items_done++;continue;}
        if(op<=1 && !protect_destination(J.roots[i],J.roots[0])){
            event(0,errno,J.roots[i]);J.p.items_total++;J.p.items_done++;close(sf);free(name);continue;
        }
        struct stat selected;
        if(op<5 && fstatat(sf,name,&selected,AT_SYMLINK_NOFOLLOW)){
            event(0,errno,J.roots[i]);J.p.items_total++;J.p.items_done++;close(sf);free(name);continue;
        }
        if(J.job.op==WSLP_OP_COPY_FILE&&!S_ISREG(selected.st_mode)){event(0,ENOTSUP,J.roots[i]);J.p.items_total++;J.p.items_done++;close(sf);free(name);continue;}
        J.valid[i]=1;
        if(op==3 || op>=4){J.p.items_total++;}
        else {
            struct stat st,target;
            int fast=op==1 && !fstatat(sf,name,&st,AT_SYMLINK_NOFOLLOW) && st.st_dev==ds.st_dev;
            int merge=fast && S_ISDIR(st.st_mode) && (J.job.policy==1||J.job.policy==2) &&
                      !fstatat(df,name,&target,AT_SYMLINK_NOFOLLOW) && S_ISDIR(target.st_mode);
            if(fast && !merge){J.p.items_total++;if(S_ISREG(st.st_mode))J.p.bytes_total+=(uint64_t)st.st_size;}
            else if(J.job.op==WSLP_OP_COPY_FILE){J.p.items_total++;J.p.bytes_total+=(uint64_t)selected.st_size;}
            else scan(sf,name,J.roots[i],0);
        }
        close(sf);free(name);
    }
    J.p.state=2;progress(NULL,1);
    if(!J.cancelled) {
        wslp_head ready;
        if(!io(0,&ready,sizeof ready,0)||ready.id!=J.id||ready.type!=WSLP_JOB_CONTINUE||ready.size)J.cancelled=1;
    }
    /* Windows destinations use checked Windows barriers in the coordinator.
     * Native Linux endpoints fence existing ancestry before any source can
     * move, including the same-volume atomic-rename fast path. Selected leaves
     * inherit the coordinator's checked root ancestry and live guards. */
    if(op<=1&&J.job.op!=WSLP_OP_COPY_FILE&&!J.windows[0]&&!cancel()){
        int error=fp_ancestry_sync(df,cancel);
        if(error){if(error==ECANCELED)J.cancelled=1;else event(0,error,J.roots[0]);goto completion;}
    }
    WQ.width=J.job.prepare_width&~WSLP_GROUP_FLAG;WQ.group=!!(J.job.prepare_width&WSLP_GROUP_FLAG);
    {int error=commit_start();if(error){event(0,error,J.roots[0]);goto completion;}}
    for(uint32_t i=1;i<=J.job.count && !cancel();i++){
        if(!J.valid[i])continue;
        char *name=NULL;int sf=parent(J.roots[i],&name);
        if(sf<0){event(0,errno,J.roots[i]);continue;}
        if(op<=1){
            if(protect_destination(J.roots[i],J.roots[0])){
                J.dest_dev=0;J.dest_ino=0;
                copy_node(sf,name,df,name,J.roots[i],J.roots[0],J.job.policy,op==1,J.windows[0],0);
            }else{
                event(0,errno,J.roots[i]);J.p.items_done++;
            }
        } else if(op==2)delete_node(sf,name,J.roots[i],0);
        else if(op==3){
            if(J.windows[i])event(0,ENOTSUP,J.roots[i]);else recycle(sf,name,J.roots[i]);
        }else if(op==4){
            char *to=NULL;int fd=parent(J.roots[0],&to);
            if(fd<0||rename_new(sf,name,fd,to,0))event(0,errno,J.roots[i]);
            else J.p.items_done++;
            if(fd>=0)close(fd);free(to);
        }else if(op==5){if(mkdirat(sf,name,0777))event(0,errno,J.roots[i]);else J.p.items_done++;}
        else if(op==6){
            if(J.roots[0][0]){
                char *from=NULL;int fd=parent(J.roots[0],&from);
                char *folder=strdup(J.roots[i]);char *slash=folder?strrchr(folder,'/'):NULL;
                if(slash){if(slash==folder)slash[1]=0;else *slash=0;}
                if(fd<0||!folder){event(0,fd<0?errno:ENOMEM,J.roots[i]);}
                else {
                    struct stat template;
                    if(fstatat(fd,from,&template,AT_SYMLINK_NOFOLLOW)||!S_ISREG(template.st_mode)) event(0,ENOTSUP,J.roots[0]);
                    else copy_node(fd,from,sf,name,J.roots[0],folder,3,0,2,0);
                }
                if(fd>=0)close(fd);free(from);free(folder);
            }else {
                int fd=openat(sf,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0666);
                if(fd<0)event(0,errno,J.roots[i]);else {if(close(fd))event(0,errno,J.roots[i]);J.p.items_done++;}
            }
        }
        close(sf);free(name);progress(J.roots[i],0);
    }
completion:;
    win_prepare_drain();
    commit_stop();
    int cleanup=fp_stage_dispose(&J.stage);if(cleanup&&!cancel())event(0,cleanup,J.roots[0]);
    if(df>=0)close(df);
    J.p.state=cancel()?4:3;
    /* Same-volume renames count the selected directory, not its descendants. */
    if(J.p.items_done>J.p.items_total)J.p.items_total=J.p.items_done;
    progress(NULL,1);
}
int fp_jobs_main(void)
{
    fp_stage_init(&J.stage);
    signal(SIGPIPE,SIG_IGN);
    wslp_hello hello={WSLP_MAGIC,WSLP_VERSION,(uint32_t)getuid(),(uint32_t)getpid()};send(WSLP_HELLO,&hello,sizeof hello);
    wslp_head h;
    if(!io(0,&h,sizeof h,0)||h.type!=WSLP_JOB||h.size<sizeof(wslp_job)||h.size>WSLP_JOB_MAX)return 1;
    J.id=h.id;uint8_t *buf=malloc(h.size);if(!buf || !io(0,buf,h.size,0)){free(buf);return 1;}
    memcpy(&J.job,buf,sizeof J.job);
    if(J.job.op>WSLP_OP_COPY_FILE||J.job.policy>3||!J.job.count||J.job.count>100000||
       ((J.job.op==WSLP_OP_SYMLINK_RELATIVE||J.job.op==WSLP_OP_SYMLINK_ABSOLUTE)&&(J.job.count!=1||J.job.policy!=3))){free(buf);return 1;}
    J.roots=calloc((size_t)J.job.count+1,sizeof(char *));J.windows=calloc((size_t)J.job.count+1,sizeof(uint32_t));
    J.valid=calloc((size_t)J.job.count+1,1);
    const uint8_t *p=buf+sizeof J.job;size_t left=h.size-sizeof J.job;int valid=J.roots&&J.windows&&J.valid;
    for(uint32_t i=0;valid && i<=J.job.count;i++)valid=take_path(&p,&left,i);
    valid=valid && !left && (!J.job.prepare_width||
        (((J.job.prepare_width&~WSLP_GROUP_FLAG)==2||(J.job.prepare_width&~WSLP_GROUP_FLAG)==4||(J.job.prepare_width&~WSLP_GROUP_FLAG)==8)&&J.job.op==0&&J.windows[0]));
    if(!valid){J.p.state=5;event(0,errno?errno:EINVAL,NULL);progress(NULL,1);}
    else run();
    if(J.roots)for(uint32_t i=0;i<=J.job.count;i++)free(J.roots[i]);
    free(J.roots);free(J.windows);free(J.valid);free(buf);return 0;
}
