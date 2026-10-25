/* Scoped metadata and identity helper: literal inputs, one pinned inode per
 * mutation, no recursion, symlink following, shell commands or chmod after chown. */
#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <pwd.h>
#include <grp.h>
#include "wslproto.h"

_Static_assert(sizeof(wslp_permission_state)==40,"permission state wire layout");
_Static_assert(sizeof(wslp_permission_request)==56,"permission request wire layout");
_Static_assert(sizeof(wslp_permission_result)==304,"permission reply wire layout");
_Static_assert(sizeof(wslp_identity_request)==264,"identity request wire layout");
_Static_assert(sizeof(wslp_identity_result)==280,"identity reply wire layout");
_Static_assert(sizeof(wslp_ownership_request)==56,"ownership request wire layout");
static int io(int fd,void *data,size_t n,int output)
{
    char *p=data;
    while(n){ssize_t k=output?write(fd,p,n):read(fd,p,n);if(k<0&&errno==EINTR)continue;if(k<=0)return 0;p+=k;n-=k;}return 1;
}
static void state(const struct stat *st,wslp_permission_state *s)
{
    *s=(wslp_permission_state){st->st_dev,st->st_ino,st->st_ctim.tv_sec,
        st->st_ctim.tv_nsec,st->st_mode,st->st_uid,st->st_gid};
}
/* O_PATH permits an owner to repair an unreadable (0000) file. fchmod cannot
 * use O_PATH; fchmodat2 or the procfs descriptor alias acts on the pinned inode. */
static int change_mode(int fd,mode_t mode)
{
#ifdef SYS_fchmodat2
    if(syscall(SYS_fchmodat2,fd,"",mode,AT_EMPTY_PATH)==0)return 0;
    if(errno!=ENOSYS && errno!=EINVAL)return -1;
#endif
    char alias[64];snprintf(alias,sizeof alias,"/proc/self/fd/%d",fd);
    return chmod(alias,mode);
}
/* Identity resolution is read-only. Decimal IDs may be unmapped, but -1 is
 * reserved by chown for "leave unchanged" and must never become a target. */
static int identity(const char *name,int user,uint32_t *id,char resolved[128])
{
    if(!*name)return EINVAL;
    int numeric=1;uint64_t value=0;
    for(const unsigned char *p=(const unsigned char*)name;*p;p++){
        if(*p<32||*p==127)return EINVAL;
        if(*p<'0'||*p>'9')numeric=0;
    }
    if(numeric){
        for(const char *p=name;*p;p++){value=value*10+*p-'0';if(value>=UINT32_MAX)return EINVAL;}
        *id=(uint32_t)value;
    }else if(user){struct passwd *u=getpwnam(name);if(!u)return errno?errno:ENOENT;*id=u->pw_uid;}
    else{struct group *g=getgrnam(name);if(!g)return errno?errno:ENOENT;*id=g->gr_gid;}
    if(*id==UINT32_MAX)return EINVAL;
    const char *canonical=NULL;
    if(user){struct passwd *u=getpwuid(*id);if(u)canonical=u->pw_name;}
    else{struct group *g=getgrgid(*id);if(g)canonical=g->gr_name;}
    if(canonical){if(strlen(canonical)>=128)return EOVERFLOW;strcpy(resolved,canonical);}
    return 0;
}
static int resolve_identity(wslp_head h)
{
    wslp_identity_request q;wslp_identity_result r={0};
    if(h.size!=sizeof q||!io(0,&q,sizeof q,0))return 1;
    r.fields=q.fields;
    if(!q.fields||(q.fields&~3u)||q.pad||q.owner[127]||q.group[127]||
       (!(q.fields&WSLP_ID_USER)&&q.owner[0])||(!(q.fields&WSLP_ID_GROUP)&&q.group[0]))r.err=EINVAL;
    else{
        if(q.fields&WSLP_ID_USER){errno=0;r.err=identity(q.owner,1,&r.uid,r.owner);if(r.err)r.bad_field=WSLP_ID_USER;}
        if(!r.err&&(q.fields&WSLP_ID_GROUP)){errno=0;r.err=identity(q.group,0,&r.gid,r.group);if(r.err)r.bad_field=WSLP_ID_GROUP;}
    }
    h=(wslp_head){WSLP_IDENTITY_RESULT,sizeof r,h.id};return io(1,&h,sizeof h,1)&&io(1,&r,sizeof r,1)?0:1;
}
static void names(wslp_permission_result *r)
{
    struct passwd *u=getpwuid(r->state.uid);if(u)snprintf(r->owner,sizeof r->owner,"%s",u->pw_name);
    struct group *g=getgrgid(r->state.gid);if(g)snprintf(r->group,sizeof r->group,"%s",g->gr_name);
}
static int change_ownership(wslp_head h)
{
    wslp_ownership_request q;wslp_permission_result r={0};char path[16385];
    if(h.size<sizeof q||h.size>sizeof q+16384||!io(0,&q,sizeof q,0)||!q.path_bytes||
       q.path_bytes>16384||h.size!=sizeof q+q.path_bytes||!io(0,path,q.path_bytes,0)||memchr(path,0,q.path_bytes))return 1;
    path[q.path_bytes]=0;size_t n=q.path_bytes;while(n>1&&path[n-1]=='/')path[--n]=0;
    int fd=-1;struct stat st;
    if(path[0]!='/'||!q.fields||(q.fields&~3u)||
       ((q.fields&WSLP_ID_USER)&&q.uid==UINT32_MAX)||((q.fields&WSLP_ID_GROUP)&&q.gid==UINT32_MAX)||
       (!(q.fields&WSLP_ID_USER)&&q.uid)||(!(q.fields&WSLP_ID_GROUP)&&q.gid))r.err=EINVAL;
    else if((fd=open(path,O_PATH|O_NOFOLLOW|O_CLOEXEC))<0)r.err=errno;
    else if(fstat(fd,&st))r.err=errno;
    else if(!S_ISREG(st.st_mode)&&!S_ISDIR(st.st_mode))r.err=EOPNOTSUPP;
    else{
        state(&st,&r.state);
        uint32_t uid=q.fields&WSLP_ID_USER?q.uid:st.st_uid,gid=q.fields&WSLP_ID_GROUP?q.gid:st.st_gid;
        if(memcmp(&q.expected,&r.state,sizeof r.state))r.err=ESTALE;
        /* A no-op must not touch ctime, capability xattrs or special bits. */
        else if(uid!=st.st_uid||gid!=st.st_gid){
            if(fchownat(fd,"",q.fields&WSLP_ID_USER?(uid_t)uid:(uid_t)-1,
                        q.fields&WSLP_ID_GROUP?(gid_t)gid:(gid_t)-1,AT_EMPTY_PATH))r.err=errno;
            else{
                r.changed=1;
                if(fstat(fd,&st))r.err=errno;
                else{
                    state(&st,&r.state);
                    /* Report the kernel's actual mode. Allow cleared setid
                     * bits, never silently restore them or other metadata. */
                    if(st.st_uid!=uid||st.st_gid!=gid||
                       (st.st_mode&~06000u)!=(q.expected.mode&~06000u)||
                       ((st.st_mode&06000u)&~q.expected.mode))r.err=EOPNOTSUPP;
                    struct stat at;
                    if(!r.err&&(lstat(path,&at)||at.st_dev!=st.st_dev||at.st_ino!=st.st_ino))r.err=ESTALE;
                }
            }
        }
        names(&r);
    }
    if(fd>=0)close(fd);h=(wslp_head){WSLP_OWNERSHIP_RESULT,sizeof r,h.id};
    return io(1,&h,sizeof h,1)&&io(1,&r,sizeof r,1)?0:1;
}
static int permission_request(wslp_head h)
{
    wslp_permission_request q;wslp_permission_result r={0};char path[16385];
    if(h.type==WSLP_IDENTITY_RESOLVE)return resolve_identity(h);
    if(h.type==WSLP_OWNERSHIP)return change_ownership(h);
    if(h.type!=WSLP_PERMISSIONS||h.size<sizeof q||h.size>sizeof q+16384||!io(0,&q,sizeof q,0))return 1;
    if(!q.path_bytes||q.path_bytes>16384||h.size!=sizeof q+q.path_bytes||!io(0,path,q.path_bytes,0)||memchr(path,0,q.path_bytes))return 1;
    path[q.path_bytes]=0;
    /* A trailing slash would make open follow a directory symlink despite
     * O_NOFOLLOW. Normalize redundant final slashes without resolving links. */
    size_t n=q.path_bytes;while(n>1&&path[n-1]=='/')path[--n]=0;
    int fd=-1;struct stat st;
    if(path[0]!='/'||q.apply>1||q.pad||q.mode>07777)r.err=EINVAL;
    else if((fd=open(path,O_PATH|O_NOFOLLOW|O_CLOEXEC))<0)r.err=errno;
    else if(fstat(fd,&st))r.err=errno;
    else if(!S_ISREG(st.st_mode)&&!S_ISDIR(st.st_mode))r.err=EOPNOTSUPP;
    else {
        state(&st,&r.state);
        if(q.apply) {
            if(memcmp(&q.expected,&r.state,sizeof r.state))r.err=ESTALE;
            else if(change_mode(fd,q.mode))r.err=errno;
            else {r.changed=1;if(fstat(fd,&st))r.err=errno;else {
                state(&st,&r.state);if((st.st_mode&07777)!=q.mode)r.err=EOPNOTSUPP;
                struct stat at;
                if(!r.err&&(lstat(path,&at)||at.st_dev!=st.st_dev||at.st_ino!=st.st_ino))r.err=ESTALE;
            }}
        }
        struct passwd *u=getpwuid(r.state.uid);if(u)snprintf(r.owner,sizeof r.owner,"%s",u->pw_name);
        struct group *g=getgrgid(r.state.gid);if(g)snprintf(r.group,sizeof r.group,"%s",g->gr_name);
    }
    if(fd>=0)close(fd);
    h=(wslp_head){WSLP_PERMISSION_RESULT,sizeof r,h.id};
    return io(1,&h,sizeof h,1)&&io(1,&r,sizeof r,1)?0:1;
}

/* A session belongs to one Windows worker and captured WSL user. Each frame
 * is independent: fresh metadata and pinned-inode guards are never cached.
 * Clean EOF ends the session; a partial/invalid frame terminates it. */
int fp_permission_main(void)
{
    wslp_head h={WSLP_HELLO,sizeof(wslp_hello),0};
    wslp_hello hi={WSLP_MAGIC,WSLP_VERSION,getuid(),getpid()};
    if(!io(1,&h,sizeof h,1)||!io(1,&hi,sizeof hi,1))return 1;
    for(;;){
        ssize_t n;
        do {n=read(0,&h,1);} while(n<0&&errno==EINTR);
        if(!n)return 0;
        if(n<0||!io(0,(char*)&h+1,sizeof h-1,0)||permission_request(h))return 1;
    }
}
