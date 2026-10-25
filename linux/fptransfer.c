/* Cross-distro streams. Only Linux helpers touch Linux files. A file commits
 * after source identity checks, SHA-256 readback and metadata verification;
 * the exporter removes a moved source only after that commit ACK. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>
#include "wslproto.h"
#include "fpsha256.h"
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1u
#endif
#include "fpstage.h"
#include "fpmount.h"
#include "fpancestry.h"
#include "fpmetadata.h"
#define PATH_CAP 65536u
static struct {uint32_t id,op,policy,serial;int cancelled;char *path;wslp_progress p;uint64_t reported;} X;
static int io(int fd,void *data,size_t n,int writing){uint8_t *p=data;while(n){ssize_t k=writing?write(fd,p,n):read(fd,p,n);if(k<0&&errno==EINTR)continue;if(k<=0)return 0;p+=k;n-=(size_t)k;}return 1;}
static int send_packet(uint32_t type,const void *data,uint32_t n){wslp_head h={type,n,X.id};if(!io(1,&h,sizeof h,1)||(n&&!io(1,(void *)data,n,1))){X.cancelled=1;return 0;}return 1;}
static int read_packet(wslp_head *h,void *data,uint32_t cap){return io(0,h,sizeof *h,0)&&h->id==X.id&&h->size<=cap&&(!h->size||io(0,data,h->size,0));}
static int cancel(void){struct pollfd p={0,POLLIN,0};if(!X.cancelled&&poll(&p,1,0)>0){wslp_head h;if((p.revents&(POLLHUP|POLLERR))||!io(0,&h,sizeof h,0)||h.id!=X.id||h.type!=WSLP_JOB_CANCEL||h.size)X.cancelled=1;else X.cancelled=1;}return X.cancelled;}
static int same(const struct stat *a,const struct stat *b){return a->st_dev==b->st_dev&&a->st_ino==b->st_ino;}
static uint64_t filesystem(int fd){struct statfs fs;uint64_t id=0;if(!fstatfs(fd,&fs))memcpy(&id,&fs.f_fsid,sizeof id);return id;}
static int same_export(int fd,const struct stat *st,uint64_t dev,uint64_t ino,uint64_t fs){return (uint64_t)st->st_ino==ino&&((fs&&filesystem(fd)==fs)||(uint64_t)st->st_dev==dev);}
static int stable(const struct stat *a,const struct stat *b){return same(a,b)&&a->st_size==b->st_size&&a->st_mode==b->st_mode&&a->st_uid==b->st_uid&&a->st_gid==b->st_gid&&a->st_mtim.tv_sec==b->st_mtim.tv_sec&&a->st_mtim.tv_nsec==b->st_mtim.tv_nsec&&a->st_ctim.tv_sec==b->st_ctim.tv_sec&&a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;}
static int guard(int parent,const char *name,int fd,const struct stat *st,int directory){struct stat a,b;if(fstat(fd,&a)||fstatat(parent,name,&b,AT_SYMLINK_NOFOLLOW)||!(directory?same(st,&a)&&same(st,&b):stable(st,&a)&&stable(st,&b))){errno=ESTALE;return 0;}return 1;}
static char *join(const char *a,const char *b){size_t n=strlen(a),m=strlen(b);if(n+m+2>PATH_CAP){errno=ENAMETOOLONG;return NULL;}char *p=malloc(n+m+2);if(!p){errno=ENOMEM;return NULL;}memcpy(p,a,n);if(n&&p[n-1]!='/')p[n++]='/';memcpy(p+n,b,m+1);return p;}
static int parent(const char *path,char **name){char *p=strdup(path);if(!p)return -1;size_t n=strlen(p);while(n>1&&p[n-1]=='/')p[--n]=0;char *s=strrchr(p,'/');if(!s||!s[1]||!strcmp(s+1,".")||!strcmp(s+1,"..")){free(p);errno=EINVAL;return -1;}*name=strdup(s+1);if(!*name){free(p);return -1;}if(s==p)s[1]=0;else *s=0;int fd=open(p,O_RDONLY|O_DIRECTORY|O_CLOEXEC);free(p);if(fd<0){free(*name);*name=NULL;}return fd;}
static uint64_t now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return(uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
static const char *suffix(const char *path){size_t n=strlen(X.path);return path&&!strncmp(path,X.path,n)&&(!path[n]||path[n]=='/')?path+n:"";}
static void progress(const char *path,int force){uint64_t t=now();if(!force&&t-X.reported<50)return;X.reported=t;const char *s=suffix(path);wslp_progress p=X.p;p.root=path?1:WSLP_NO_ROOT;p.path_bytes=(uint32_t)strlen(s);uint8_t *b=malloc(sizeof p+p.path_bytes);if(!b){X.cancelled=1;return;}memcpy(b,&p,sizeof p);memcpy(b+sizeof p,s,p.path_bytes);send_packet(WSLP_JOB_PROGRESS,b,sizeof p+p.path_bytes);free(b);}
static void event(int err,int skipped,const char *path){if(skipped)X.p.skipped++;else X.p.errors++,X.p.error=err;const char *s=suffix(path);wslp_event e={(uint32_t)skipped,err,1,(uint32_t)strlen(s)};uint8_t *b=malloc(sizeof e+e.path_bytes);if(!b){X.cancelled=1;return;}memcpy(b,&e,sizeof e);memcpy(b+sizeof e,s,e.path_bytes);send_packet(WSLP_JOB_EVENT,b,sizeof e+e.path_bytes);free(b);progress(path,1);}
static wslp_transfer_ack wait_ack(void){wslp_head h;wslp_transfer_ack a={.err=EPIPE};if(!read_packet(&h,&a,sizeof a)){X.cancelled=1;return a;}if(h.type==WSLP_JOB_CANCEL&&!h.size){X.cancelled=1;return(wslp_transfer_ack){.err=ECANCELED};}if(h.type!=WSLP_TRANSFER_ACK||h.size!=sizeof a||a.skipped>1||a.committed>1){X.cancelled=1;a.err=EPROTO;}if(a.err==ECANCELED)X.cancelled=1;return a;}
static int acknowledge(wslp_transfer_ack a,const char *path){if(a.err||a.skipped){if(!X.cancelled)event(a.err?a.err:EEXIST,a.skipped,path);return 0;}return !X.cancelled;}
static void scan(int pf,const char *name,const char *path,unsigned depth){if(cancel())return;struct stat st;if(fstatat(pf,name,&st,AT_SYMLINK_NOFOLLOW))return;X.p.items_total++;if(fp_mount_check(pf,name))return;if(S_ISREG(st.st_mode))X.p.bytes_total+=(uint64_t)st.st_size;progress(path,0);if(!S_ISDIR(st.st_mode)||depth>=WSLP_TRANSFER_DEPTH)return;int fd=fp_mount_open(pf,name,O_RDONLY|O_DIRECTORY);if(fd<0)return;DIR *d=fdopendir(fd);if(!d){close(fd);return;}struct dirent *e;while(!cancel()&&(e=readdir(d))){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;char *p=join(path,e->d_name);if(p){scan(fd,e->d_name,p,depth+1);free(p);}}closedir(d);}
static int remove_source(int pf,const char *name,int fd,const struct stat *st,const char *path,int directory,const fp_meta *meta){if(cancel())return 0;int metadata_error=meta?fp_meta_unchanged(fd,meta):0;if(metadata_error){event(metadata_error,0,path);return 0;}if(!guard(pf,name,fd,st,directory)||unlinkat(pf,name,directory?AT_REMOVEDIR:0)){event(errno,0,path);return 0;}int error=fp_stage_sync(pf);if(error){event(error,0,path);return 0;}return 1;}
static int export_node(int pf,const char *name,const char *path,unsigned depth){
    if(cancel())return 0;struct stat st;int fd=-1,ok=0;
    if(fstatat(pf,name,&st,AT_SYMLINK_NOFOLLOW)){X.p.items_done++;event(errno,0,path);return 0;}
    if(depth>=WSLP_TRANSFER_DEPTH){X.p.items_done++;event(ELOOP,0,path);return 0;}
    int directory=S_ISDIR(st.st_mode),file=S_ISREG(st.st_mode),link=S_ISLNK(st.st_mode);
    if((X.op==WSLP_OP_COPY_FILE&&!file)||(!directory&&!file&&!link)){event(ENOTSUP,0,path);X.p.items_done++;return 0;}
    fd=fp_mount_open(pf,name,(link?O_PATH:O_RDONLY)|(directory?O_DIRECTORY:0));
    if(fd<0||!guard(pf,name,fd,&st,0)){X.p.items_done++;event(errno,0,path);if(fd>=0)close(fd);return 0;}
    fp_meta attrs={0};int metadata_error=link?fp_meta_link(pf,name,&attrs):fp_meta_fd(fd,&attrs);
    if(!metadata_error)metadata_error=fp_meta_valid(attrs.data,attrs.size,directory);
    if(metadata_error){fp_meta_free(&attrs);X.p.items_done++;event(metadata_error,0,path);close(fd);return 0;}
    char *target=NULL;ssize_t n=0;if(link){target=malloc(PATH_CAP);if(!target){X.p.items_done++;event(ENOMEM,0,path);fp_meta_free(&attrs);close(fd);return 0;}n=readlinkat(pf,name,target,PATH_CAP);if(n<0||n==(ssize_t)PATH_CAP){X.p.items_done++;event(n<0?errno:ENAMETOOLONG,0,path);free(target);fp_meta_free(&attrs);close(fd);return 0;}}
    wslp_node node={.size=(uint64_t)st.st_size,.device=(uint64_t)st.st_dev,.inode=(uint64_t)st.st_ino,.filesystem=filesystem(fd),
        .atime=(int64_t)st.st_atim.tv_sec*1000000000+st.st_atim.tv_nsec,.mtime=(int64_t)st.st_mtim.tv_sec*1000000000+st.st_mtim.tv_nsec,
        .mode=st.st_mode,.uid=st.st_uid,.gid=st.st_gid,.name_bytes=(uint32_t)strlen(name),.link_bytes=(uint32_t)n,.metadata_bytes=attrs.size};
    size_t bytes=sizeof node+node.name_bytes+node.link_bytes+attrs.size;
    if(bytes>128u*1024u){X.p.items_done++;event(E2BIG,0,path);free(target);fp_meta_free(&attrs);close(fd);return 0;}
    uint8_t *b=malloc(bytes);if(!b){X.p.items_done++;event(ENOMEM,0,path);free(target);fp_meta_free(&attrs);close(fd);return 0;}
    memcpy(b,&node,sizeof node);memcpy(b+sizeof node,name,node.name_bytes);if(node.link_bytes)memcpy(b+sizeof node+node.name_bytes,target,node.link_bytes);
    if(attrs.size)memcpy(b+sizeof node+node.name_bytes+node.link_bytes,attrs.data,attrs.size);free(target);
    int sent=send_packet(WSLP_TRANSFER_NODE,b,(uint32_t)bytes);free(b);if(!sent){fp_meta_free(&attrs);close(fd);return 0;}
    wslp_transfer_ack ack=wait_ack();if(!acknowledge(ack,path)){X.p.items_done++;fp_meta_free(&attrs);close(fd);return 0;}
    if(directory){
        DIR *dir=fdopendir(fd);if(!dir){X.p.items_done++;event(errno,0,path);send_packet(WSLP_TRANSFER_DIR_END,NULL,0);wait_ack();fp_meta_free(&attrs);close(fd);return 0;}
        ok=1;struct dirent *e;errno=0;
        while(!cancel()&&(e=readdir(dir))){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;char *p=join(path,e->d_name);if(!p){event(errno,0,path);ok=0;continue;}if(!export_node(fd,e->d_name,p,depth+1))ok=0;free(p);errno=0;}
        if(!X.cancelled&&errno){event(errno,0,path);ok=0;}
        if(!X.cancelled){metadata_error=fp_meta_unchanged(fd,&attrs);if(metadata_error){event(metadata_error,0,path);ok=0;}}
        if(!X.cancelled){send_packet(WSLP_TRANSFER_DIR_END,NULL,0);ack=wait_ack();if(!acknowledge(ack,path))ok=0;else if(!ack.committed){event(EIO,0,path);ok=0;}}
        if(ok&&X.op==1)ok=remove_source(pf,name,fd,&st,path,1,&attrs);
        closedir(dir);fd=-1;
    }else if(file){
        uint8_t *buf=malloc(WSLP_TRANSFER_CHUNK);wslp_transfer_end end={0};fp_sha256 sha;fp_sha_init(&sha);uint64_t base=X.p.bytes_done;
        if(!buf)end.err=ENOMEM;
        while(buf&&!end.err&&!cancel()){
            ssize_t got=read(fd,buf,WSLP_TRANSFER_CHUNK);if(got<0&&errno==EINTR)continue;if(got<0){end.err=errno;break;}if(!got)break;
            fp_sha_update(&sha,buf,(size_t)got);end.bytes+=(uint64_t)got;
            if(!send_packet(WSLP_TRANSFER_DATA,buf,(uint32_t)got))break;
            X.p.bytes_done+=(uint64_t)got;progress(path,0);
        }
        free(buf);if(X.cancelled)end.err=ECANCELED;
        if(!end.err)end.err=fp_meta_unchanged(fd,&attrs);
        if(!end.err&&(end.bytes!=(uint64_t)st.st_size||!guard(pf,name,fd,&st,0)))end.err=ESTALE;
        if(attrs.size)fp_sha_update(&sha,attrs.data,attrs.size);
        fp_sha_final(&sha,end.sha256);send_packet(WSLP_TRANSFER_END,&end,sizeof end);ack=wait_ack();
        ok=acknowledge(ack,path);
        /* A prepared file is not a committed transfer. Preserve source errors
         * even if the peer's final acknowledgement claims no error. */
        if(ok&&(end.err||!ack.committed)){event(end.err?end.err:EIO,0,path);ok=0;}
        if(ok&&X.op==1)ok=remove_source(pf,name,fd,&st,path,0,&attrs);
        if(!ok)X.p.bytes_done=base;
    }else{
        ok=ack.committed&&guard(pf,name,fd,&st,0);if(!ok&&!X.cancelled)event(ESTALE,0,path);
        if(ok&&X.op==1)ok=remove_source(pf,name,fd,&st,path,0,NULL);
    }
    fp_meta_free(&attrs);if(fd>=0)close(fd);if(!X.cancelled)X.p.items_done++;progress(path,0);return ok&&!X.cancelled;
}
/* Importer state is bounded by the recursion cap, one temp file and two
 * fixed-size chunk buffers during readback. Components never contain '/'. */
typedef struct directory {int fd,parent,created;char *name;struct stat identity;wslp_node meta;fp_meta attrs;} directory;
static struct {int root,depth,err,replace;fp_stage stage;char *name;struct stat previous;int existed;uint64_t bytes;wslp_node meta;fp_meta attrs;fp_sha256 sha;directory dirs[WSLP_TRANSFER_DEPTH];} I;
static int component(const uint8_t *p,uint32_t n){return n&&n<=255&&!memchr(p,0,n)&&!memchr(p,'/',n)&&!(n==1&&p[0]=='.')&&!(n==2&&p[0]=='.'&&p[1]=='.');}
static int current(void){return I.depth?I.dirs[I.depth-1].fd:I.root;}
static int intact(void){struct stat s,t;if(stat(X.path,&s)||fstat(I.root,&t)||!same(&s,&t)){errno=ESTALE;return 0;}for(int n=0;n<I.depth;n++){directory *d=I.dirs+n;if(fstatat(d->parent,d->name,&s,AT_SYMLINK_NOFOLLOW)||!same(&s,&d->identity)){errno=ESTALE;return 0;}}return 1;}
static int ancestor(int fd,uint64_t dev,uint64_t ino,uint64_t fs){int d=dup(fd);if(d<0)return -1;for(unsigned n=0;n<1024;n++){struct stat s,t;if(fstat(d,&s)){close(d);return -1;}if(same_export(d,&s,dev,ino,fs)){close(d);errno=ELOOP;return 1;}int p=openat(d,"..",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(p<0||fstat(p,&t)){if(p>=0)close(p);close(d);return -1;}if(same(&s,&t)){close(p);close(d);return 0;}close(d);d=p;}close(d);errno=ELOOP;return -1;}
static int choose(int fd,char **name,const wslp_node *node,int *merge,int *replace,struct stat *previous,int *existed){
    struct stat st;if(fstatat(fd,*name,&st,AT_SYMLINK_NOFOLLOW)){if(errno==ENOENT){*existed=0;return 0;}return errno;}
    int leaf=openat(fd,*name,O_PATH|O_NOFOLLOW|O_CLOEXEC);int alias=leaf>=0&&same_export(leaf,&st,node->device,node->inode,node->filesystem);if(leaf>=0)close(leaf);if(alias)return ELOOP;
    *previous=st;*existed=1;
    if(X.policy==0){const char *dot=S_ISDIR(node->mode)?NULL:strrchr(*name,'.');if(dot==*name)dot=NULL;size_t stem=dot?(size_t)(dot-*name):strlen(*name);const char *ext=dot?dot:"";
        for(unsigned n=2;n<100000;n++){char candidate[300];int k=snprintf(candidate,sizeof candidate,"%.*s (%u)%s",(int)stem,*name,n,ext);if(k<0||k>255)return ENAMETOOLONG;if(fstatat(fd,candidate,&st,AT_SYMLINK_NOFOLLOW)){if(errno!=ENOENT)return errno;char *p=strdup(candidate);if(!p)return ENOMEM;free(*name);*name=p;*existed=0;return 0;}}return EEXIST;}
    if(S_ISDIR(node->mode)&&S_ISDIR(st.st_mode)&&(X.policy==1||X.policy==2)){*merge=1;return 0;}
    if(X.policy==1)return EEXIST;
    if(X.policy==2&&((S_ISREG(node->mode)&&S_ISREG(st.st_mode))||(S_ISLNK(node->mode)&&S_ISLNK(st.st_mode)))){*replace=1;return 0;}
    return S_ISLNK(st.st_mode)?ELOOP:EEXIST;
}
static struct timespec stamp(int64_t ns){int64_t sec=ns/1000000000,rem=ns%1000000000;if(rem<0){sec--;rem+=1000000000;}return(struct timespec){(time_t)sec,(long)rem};}
static int metadata(int fd,const wslp_node *n,const fp_meta *attrs){
    if(geteuid()==0&&fchown(fd,n->uid,n->gid))return errno;
    int error=fp_meta_apply(fd,attrs,0);if(error)return error;
    struct timespec ts[2]={stamp(n->atime),stamp(n->mtime)};
    if(fchmod(fd,n->mode&07777)||futimens(fd,ts))return errno;
    error=fp_meta_verify(fd,attrs,0);if(error)return error;
    struct stat st;if(fstat(fd,&st))return errno;
    if((st.st_mode&07777)!=(n->mode&07777)||st.st_mtim.tv_sec!=ts[1].tv_sec||st.st_mtim.tv_nsec!=ts[1].tv_nsec||st.st_atim.tv_sec!=ts[0].tv_sec||st.st_atim.tv_nsec!=ts[0].tv_nsec||(geteuid()==0&&(st.st_uid!=n->uid||st.st_gid!=n->gid)))return EIO;
    return 0;
}
static int commit(fp_stage *stage,const char *name,int replace,int existed,const struct stat *previous){
    int error=fp_stage_sync_payload(stage);if(error)return error;
    if(!intact())return errno;
    if(existed){struct stat st;if(fstatat(stage->parent,name,&st,AT_SYMLINK_NOFOLLOW)||!stable(previous,&st))return ESTALE;}
    return fp_stage_publish(stage,name,replace);
}
static int abort_file(void){int error=fp_stage_finish(&I.stage);free(I.name);I.name=NULL;fp_meta_free(&I.attrs);I.err=I.bytes=0;return error;}
static wslp_transfer_ack begin(const uint8_t *buf,uint32_t size){
    wslp_transfer_ack ack={0};wslp_node n;if(size<sizeof n){ack.err=EINVAL;return ack;}memcpy(&n,buf,sizeof n);
    if(n.name_bytes>size-sizeof n||n.link_bytes>size-sizeof n-n.name_bytes||n.metadata_bytes!=size-sizeof n-n.name_bytes-n.link_bytes||
       !component(buf+sizeof n,n.name_bytes)||n.link_bytes>=PATH_CAP||memchr(buf+sizeof n+n.name_bytes,0,n.link_bytes)||
       (!S_ISLNK(n.mode)&&n.link_bytes)||(S_ISLNK(n.mode)&&n.metadata_bytes)||(!S_ISREG(n.mode)&&!S_ISDIR(n.mode)&&!S_ISLNK(n.mode))||I.stage.file>=0){ack.err=EINVAL;return ack;}
    fp_meta attrs={(uint8_t *)buf+sizeof n+n.name_bytes+n.link_bytes,n.metadata_bytes};
    ack.err=fp_meta_valid(attrs.data,attrs.size,S_ISDIR(n.mode));if(ack.err)return ack;
    if(!intact()){ack.err=errno;return ack;}
    int df=current(),merge=0,replace=0,existed=0;struct stat previous;
    char *name=strndup((const char *)buf+sizeof n,n.name_bytes);if(!name){ack.err=ENOMEM;return ack;}
    if(S_ISDIR(n.mode)){int a=ancestor(df,n.device,n.inode,n.filesystem);if(a){ack.err=errno?errno:ELOOP;free(name);return ack;}}
    ack.err=choose(df,&name,&n,&merge,&replace,&previous,&existed);
    if(ack.err){ack.skipped=X.policy==1&&ack.err==EEXIST;free(name);return ack;}
    if(S_ISDIR(n.mode)){
        if(I.depth>=WSLP_TRANSFER_DEPTH){ack.err=ELOOP;free(name);return ack;}
        if(!merge&&mkdirat(df,name,0700)){ack.err=errno;free(name);return ack;}
        int fd=fp_mount_open(df,name,O_RDONLY|O_DIRECTORY);struct stat st;
        if(fd<0||fstat(fd,&st)||(merge&&!same(&previous,&st))){ack.err=fd<0?errno:ESTALE;if(fd>=0)close(fd);free(name);return ack;}
        if(!merge)ack.err=fp_meta_apply(fd,&attrs,1);
        else if(attrs.size){fp_meta existing={0};ack.err=fp_meta_read(fd,NULL,0,&existing);if(!ack.err&&!fp_meta_equal(&attrs,&existing))ack.err=ENOTSUP;fp_meta_free(&existing);}
        fp_meta saved={0};
        if(!ack.err&&attrs.size){saved.data=malloc(attrs.size);if(!saved.data)ack.err=ENOMEM;else {memcpy(saved.data,attrs.data,attrs.size);saved.size=attrs.size;}}
        if(ack.err){close(fd);free(name);return ack;}
        /* Fence every entered path, including existing merges, before an
         * acceptance ACK can authorize child commits and source removal. */
        if((ack.err=fp_stage_sync_pair(fd,df))){fp_meta_free(&saved);close(fd);free(name);return ack;}
        I.dirs[I.depth++]=(directory){fd,df,!merge,name,st,n,saved};return ack;
    }
    if(S_ISLNK(n.mode)){
        char *target=strndup((const char *)buf+sizeof n+n.name_bytes,n.link_bytes);if(!target){ack.err=ENOMEM;free(name);return ack;}
        fp_stage *stage=&I.stage;ack.err=fp_stage_reuse(stage,df,target);
        if(!ack.err){
            struct timespec ts[2]={stamp(n.atime),stamp(n.mtime)};char check[PATH_CAP];
            if(geteuid()==0&&fchownat(stage->dir,"payload",n.uid,n.gid,AT_SYMLINK_NOFOLLOW))ack.err=errno;
            ssize_t k=readlinkat(stage->dir,"payload",check,sizeof check);if(k!=(ssize_t)n.link_bytes||memcmp(check,target,n.link_bytes))ack.err=EIO;
            if(!ack.err&&utimensat(stage->dir,"payload",ts,AT_SYMLINK_NOFOLLOW))ack.err=errno;
            if(!ack.err)ack.err=commit(stage,name,replace,existed,&previous);
            if(!ack.err)ack.committed=1;
            int cleanup=fp_stage_finish(stage);if(!ack.err)ack.err=cleanup;
        }
        free(target);free(name);return ack;
    }
    if(attrs.size){I.attrs.data=malloc(attrs.size);if(!I.attrs.data){ack.err=ENOMEM;free(name);return ack;}memcpy(I.attrs.data,attrs.data,attrs.size);I.attrs.size=attrs.size;}
    I.name=name;I.meta=n;I.replace=replace;I.previous=previous;I.existed=existed;I.err=0;I.bytes=0;fp_sha_init(&I.sha);
    ack.err=fp_stage_reuse(&I.stage,df,NULL);
    if(ack.err){free(I.name);I.name=NULL;fp_meta_free(&I.attrs);}
    return ack;
}
static wslp_transfer_ack end_file(const wslp_transfer_end *end){
    wslp_transfer_ack ack={.err=I.stage.file<0?EINVAL:I.err};if(I.stage.file<0)return ack;
    uint8_t digest[32];if(I.attrs.size)fp_sha_update(&I.sha,I.attrs.data,I.attrs.size);fp_sha_final(&I.sha,digest);
    if(!ack.err)ack.err=end->err?end->err:end->pad||I.bytes!=end->bytes||I.bytes!=I.meta.size||memcmp(digest,end->sha256,32)?EIO:0;
    if(!ack.err)ack.err=fp_stage_sync(I.stage.file);
    if(!ack.err&&lseek(I.stage.file,0,SEEK_SET)<0)ack.err=errno;
    uint8_t *buf=malloc(WSLP_TRANSFER_CHUNK);fp_sha256 sha;fp_sha_init(&sha);uint64_t read_bytes=0;
    if(!buf&&!ack.err)ack.err=ENOMEM;
    while(buf&&!ack.err){ssize_t n=read(I.stage.file,buf,WSLP_TRANSFER_CHUNK);if(n<0&&errno==EINTR)continue;if(n<0){ack.err=errno;break;}if(!n)break;read_bytes+=(uint64_t)n;fp_sha_update(&sha,buf,(size_t)n);}free(buf);if(I.attrs.size)fp_sha_update(&sha,I.attrs.data,I.attrs.size);fp_sha_final(&sha,digest);
    if(!ack.err&&(read_bytes!=end->bytes||memcmp(digest,end->sha256,32)))ack.err=EIO;
    if(!ack.err)ack.err=metadata(I.stage.file,&I.meta,&I.attrs);
    if(!ack.err)ack.err=commit(&I.stage,I.name,I.replace,I.existed,&I.previous);
    if(!ack.err)ack.committed=1;
    int cleanup=abort_file();if(!ack.err)ack.err=cleanup;return ack;
}
static wslp_transfer_ack end_directory(void){
    wslp_transfer_ack a={0};if(!I.depth){a.err=EINVAL;return a;}
    directory *d=I.dirs+I.depth-1;a.err=fp_stage_dispose(&I.stage);
    if(!a.err&&!intact())a.err=errno;
    if(!a.err&&d->created)a.err=metadata(d->fd,&d->meta,&d->attrs);
    if(!a.err&&!d->created&&d->attrs.size)a.err=fp_meta_verify(d->fd,&d->attrs,0);
    if(!a.err)a.err=fp_stage_sync_pair(d->fd,d->parent);
    if(!a.err&&!intact())a.err=errno;
    if(!a.err)a.committed=1;
    close(d->fd);free(d->name);fp_meta_free(&d->attrs);I.depth--;return a;
}
static int import_stream(void){
    fp_stage_init(&I.stage);I.root=open(X.path,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    wslp_transfer_ack ack={.err=I.root<0?errno:0};
    /* Selected regular-only leaves inherit the coordinator's already-fenced
     * root and live guard ancestry; do not repeat root barriers per file. */
    if(!ack.err&&X.op!=WSLP_OP_COPY_FILE)ack.err=fp_ancestry_sync(I.root,NULL);
    send_packet(WSLP_TRANSFER_ACK,&ack,sizeof ack);if(ack.err){if(I.root>=0)close(I.root);return 0;}
    uint8_t *buf=malloc(WSLP_MAX);if(!buf){close(I.root);return 1;}wslp_head h;
    while(read_packet(&h,buf,WSLP_MAX)){
        if(h.type==WSLP_TRANSFER_QUIT&&!h.size)break;
        if(h.type==WSLP_JOB_CANCEL&&!h.size)break;
        if(h.type==WSLP_TRANSFER_NODE){ack=begin(buf,h.size);send_packet(WSLP_TRANSFER_ACK,&ack,sizeof ack);}
        else if(h.type==WSLP_TRANSFER_DATA){
            if(I.stage.file<0||!h.size||h.size>WSLP_TRANSFER_CHUNK||I.bytes>I.meta.size||h.size>I.meta.size-I.bytes){if(I.stage.file<0)break;I.err=EINVAL;}
            else {if(!I.err&&!io(I.stage.file,buf,h.size,1))I.err=errno?errno:EIO;fp_sha_update(&I.sha,buf,h.size);I.bytes+=h.size;}
        }else if(h.type==WSLP_TRANSFER_END&&h.size==sizeof(wslp_transfer_end)){wslp_transfer_end end;memcpy(&end,buf,sizeof end);ack=end_file(&end);send_packet(WSLP_TRANSFER_ACK,&ack,sizeof ack);}
        else if(h.type==WSLP_TRANSFER_DIR_END&&!h.size){ack=end_directory();send_packet(WSLP_TRANSFER_ACK,&ack,sizeof ack);}
        else break;
        if(X.cancelled)break;
    }
    abort_file();fp_stage_dispose(&I.stage);while(I.depth){directory *d=I.dirs+--I.depth;close(d->fd);free(d->name);fp_meta_free(&d->attrs);}close(I.root);free(buf);return 0;
}
int fp_transfer_main(int importing){
    signal(SIGPIPE,SIG_IGN);wslp_hello hello={WSLP_MAGIC,WSLP_VERSION,(uint32_t)getuid(),(uint32_t)getpid()};send_packet(WSLP_HELLO,&hello,sizeof hello);
    wslp_head h;wslp_transfer request;
    if(!io(0,&h,sizeof h,0)||h.type!=WSLP_TRANSFER||h.size<sizeof request||h.size>sizeof request+PATH_CAP)return 1;
    X.id=h.id;if(!io(0,&request,sizeof request,0)||(request.op>1&&request.op!=WSLP_OP_COPY_FILE)||request.policy>3||request.pad||!request.path_bytes||request.path_bytes!=h.size-sizeof request||request.path_bytes>=PATH_CAP)return 1;
    X.path=malloc(request.path_bytes+1);if(!X.path||!io(0,X.path,request.path_bytes,0))return 1;X.path[request.path_bytes]=0;
    if(X.path[0]!='/'||memchr(X.path,0,request.path_bytes)){free(X.path);return 1;}X.op=request.op;X.policy=request.policy;
    if(importing){int r=import_stream();free(X.path);return r;}
    X.p.state=1;progress(NULL,1);char *name=NULL;int pf=parent(X.path,&name);struct stat st;int valid=pf>=0&&!fstatat(pf,name,&st,AT_SYMLINK_NOFOLLOW);
    if(!valid){event(errno,0,X.path);X.p.items_total=1;X.p.items_done=1;}
    else if(X.op==WSLP_OP_COPY_FILE&&!S_ISREG(st.st_mode)){event(ENOTSUP,0,X.path);X.p.items_total=1;X.p.items_done=1;valid=0;}
    else if(X.op==WSLP_OP_COPY_FILE){X.p.items_total=1;X.p.bytes_total=(uint64_t)st.st_size;}
    else scan(pf,name,X.path,0);
    X.p.state=2;progress(NULL,1);wslp_head ready;
    if(!io(0,&ready,sizeof ready,0)||ready.id!=X.id||ready.size||ready.type!=WSLP_JOB_CONTINUE)X.cancelled=1;
    if(!X.cancelled&&valid)export_node(pf,name,X.path,0);
    if(pf>=0)close(pf);free(name);X.p.state=X.cancelled?4:3;progress(NULL,1);free(X.path);return 0;
}
