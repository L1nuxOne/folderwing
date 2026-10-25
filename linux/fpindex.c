/* Optional per-UID filename index. One selected root per session, private
 * SQLite storage, descriptor-relative scans and recursive inotify coverage.
 * Every restart and observed change reconciles the tree before CURRENT.
 * No daemon survives stdin EOF; no cache is created by a disabled probe. */
#define _GNU_SOURCE
#include <sqlite3.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "wslproto.h"
#include "fpsha256.h"

enum {NONE,SCANNING,CURRENT,STALE,PAUSED,FAILED};
enum {STATUS,ENABLE,PAUSE,RESUME,REBUILD,REMOVE};
#define PATH_CAP 16384
#define ENTRY_MAX 1000000u
#define WATCH_MAX 100000u
typedef struct indexer {
    sqlite3 *db;
    int cache,lock,notify,quit,building,pending,querying,qcancel,root_failed;
    uint32_t pending_id,watches,backoff;
    uint64_t device,inode,mount,cache_device,cache_inode,due,pulsed,root_checked;
    char selected[PATH_CAP+1],asked[PATH_CAP+1],filename[72],relative[PATH_CAP+1];
    wslp_index_status state;
} indexer;

static uint64_t millis(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static int io(int fd,void *data,size_t n,int output)
{char *p=data;while(n){ssize_t k=output?write(fd,p,n):read(fd,p,n);if(k<0&&errno==EINTR)continue;if(k<=0)return 0;p+=k;n-=k;}return 1;}
static int send_frame(indexer *s,uint32_t type,uint32_t id,const void *data,uint32_t n)
{wslp_head h={type,n,id};if(!io(1,&h,sizeof h,1)||(n&&!io(1,(void*)data,n,1))){s->quit=1;return 0;}return 1;}
static void status_reply(indexer *s,uint32_t id)
{
    uint8_t data[sizeof(wslp_index_status)+PATH_CAP];s->state.uid=getuid();s->state.root_bytes=(uint32_t)strlen(s->selected);
    memcpy(data,&s->state,sizeof s->state);memcpy(data+sizeof s->state,s->selected,s->state.root_bytes);
    send_frame(s,WSLP_INDEX_STATUS,id,data,sizeof s->state+s->state.root_bytes);
}
static int sql(indexer *s,const char *text)
{int rc=sqlite3_exec(s->db,text,NULL,NULL,NULL);if(rc!=SQLITE_OK){s->state.error=rc==SQLITE_FULL?ENOSPC:rc==SQLITE_BUSY?EBUSY:EIO;return 0;}return 1;}
static void error(indexer *s,int e){s->state.errors++;if(!s->state.error)s->state.error=e?e:EIO;}
static int valid_path(char *path,size_t n)
{
    if(!n||n>PATH_CAP||path[0]!='/'||memchr(path,0,n))return 0;path[n]=0;
    while(n>1&&path[n-1]=='/')path[--n]=0;
    for(char *p=path+1;*p;){char *end=strchr(p,'/');size_t k=end?(size_t)(end-p):strlen(p);
        if(!k||(k==1&&p[0]=='.')||(k==2&&p[0]=='.'&&p[1]=='.'))return 0;
        if(k>255)return 0;p=end?end+1:p+k;}
    return 1;
}
static int beneath(const char *path,const char *root)
{size_t n=strlen(root);return !strncmp(path,root,n)&&(n==1||!path[n]||path[n]=='/');}
static uint64_t mount_id(int fd)
{
    char p[64],line[256];snprintf(p,sizeof p,"/proc/self/fdinfo/%d",fd);FILE *f=fopen(p,"r");
    if(!f)return 0;unsigned long long id=0;while(fgets(line,sizeof line,f))if(sscanf(line,"mnt_id: %llu",&id)==1)break;fclose(f);return id;
}
/* Reopen every ancestor with current permissions; no symlink or old directory
 * descriptor can grant access to an index result after permissions change. */
static int directory(const char *path,int make)
{
    int fd=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(fd<0)return -1;
    for(const char *p=path+1;*p;){const char *end=strchr(p,'/');size_t n=end?(size_t)(end-p):strlen(p);char name[256];
        if(!n||n>255){close(fd);errno=EINVAL;return -1;}memcpy(name,p,n);name[n]=0;
        if(make&&mkdirat(fd,name,0700)&&errno!=EEXIST){close(fd);return -1;}
        int next=openat(fd,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);close(fd);if(next<0)return -1;fd=next;p=end?end+1:p+n;
    }return fd;
}
static int private_file(int dir,const char *name,int create)
{
    int fd=openat(dir,name,O_RDWR|O_NOFOLLOW|O_CLOEXEC|(create?O_CREAT:0),0600);struct stat st;
    if(fd>=0&&(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&077)||st.st_nlink!=1)){close(fd);errno=EACCES;return -1;}return fd;
}
static int cache_open(indexer *s,int create)
{
    char base[PATH_CAP+1];const char *xdg=getenv("XDG_CACHE_HOME");
    if(xdg&&*xdg){if(strlen(xdg)>PATH_CAP)return ENAMETOOLONG;strcpy(base,xdg);}
    else{struct passwd *pw=getpwuid(getuid());if(!pw||!pw->pw_dir)return ENOENT;
        if(snprintf(base,sizeof base,"%s/.cache",pw->pw_dir)>PATH_CAP)return ENAMETOOLONG;}
    if(!valid_path(base,strlen(base)))return EINVAL;
    int parent=directory(base,create);if(parent<0)return errno;
    struct stat base_stat;if(fstat(parent,&base_stat)||base_stat.st_uid!=getuid()||(base_stat.st_mode&022)){close(parent);return EACCES;}
    if(create&&mkdirat(parent,"openpilot-search-v1",0700)&&errno!=EEXIST){int e=errno;close(parent);return e;}
    s->cache=openat(parent,"openpilot-search-v1",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(parent);if(s->cache<0)return errno;
    struct stat st;if(fstat(s->cache,&st)||st.st_uid!=getuid()||(st.st_mode&077)){close(s->cache);s->cache=-1;return EACCES;}
    s->cache_device=st.st_dev;s->cache_inode=st.st_ino;return 0;
}
static void key(const char *path,char out[72])
{fp_sha256 s;uint8_t hash[32];fp_sha_init(&s);fp_sha_update(&s,path,strlen(path));fp_sha_final(&s,hash);for(unsigned i=0;i<32;i++)sprintf(out+i*2,"%02x",hash[i]);strcat(out,".db");}
static int meta(indexer *s,int paused)
{
    sqlite3_stmt *q=NULL;int ok=sqlite3_prepare_v2(s->db,"UPDATE config SET enabled=1,paused=?",-1,&q,NULL)==SQLITE_OK;
    if(ok){sqlite3_bind_int(q,1,paused);ok=sqlite3_step(q)==SQLITE_DONE;}sqlite3_finalize(q);return ok;
}
static int unlink_database(indexer *s)
{
    if(unlinkat(s->cache,s->filename,0)&&errno!=ENOENT)return errno;char side[80];
    snprintf(side,sizeof side,"%s-wal",s->filename);if(unlinkat(s->cache,side,0)&&errno!=ENOENT)return errno;
    snprintf(side,sizeof side,"%s-shm",s->filename);if(unlinkat(s->cache,side,0)&&errno!=ENOENT)return errno;
    return 0;
}
static int open_database(indexer *s,int action)
{
    int e=cache_open(s,action==ENABLE);if(e)return e;
    strcpy(s->selected,s->asked);
    for(;;){key(s->selected,s->filename);struct stat st;
        if(!fstatat(s->cache,s->filename,&st,AT_SYMLINK_NOFOLLOW))break;
        if(errno!=ENOENT)return errno;
        if(action==ENABLE)return 0;
        char *slash=strrchr(s->selected,'/');if(!slash||slash==s->selected){
            if(!strcmp(s->selected,"/"))return ENOENT;strcpy(s->selected,"/");
        }else *slash=0;
    }return 0;
}
static int database(indexer *s,int action)
{
    int e=open_database(s,action);if(e)return e;
    char anchor[64],cache_path[PATH_CAP+1];snprintf(anchor,sizeof anchor,"/proc/self/fd/%d",s->cache);
    ssize_t cache_n=readlink(anchor,cache_path,PATH_CAP);if(cache_n<=0||cache_n==PATH_CAP)return ENOTSUP;cache_path[cache_n]=0;
    if(beneath(s->asked,cache_path))return EINVAL;
    char lockname[80];snprintf(lockname,sizeof lockname,"%s.lock",s->filename);
    s->lock=private_file(s->cache,lockname,1);if(s->lock<0)return errno;
    if(flock(s->lock,LOCK_EX|LOCK_NB))return EBUSY;
    int fd=private_file(s->cache,s->filename,action==ENABLE);if(fd<0)return errno;
    struct stat st;fstat(fd,&st);int fresh=st.st_size==0;close(fd);
    if(action==REMOVE){e=unlink_database(s);if(e)return e;unlinkat(s->cache,lockname,0);s->state=(wslp_index_status){0};s->quit=1;return 0;}
    if(action==REBUILD){e=unlink_database(s);if(e)return e;fd=private_file(s->cache,s->filename,1);if(fd<0)return errno;close(fd);fresh=1;}
    /* SQLite's NOFOLLOW mode also rejects the /proc/self/fd symlink prefix.
     * Use the resolved private cache path, with a verified owner-only cache
     * directory and a verified regular database leaf. */
    char path[PATH_CAP+80];snprintf(path,sizeof path,"%s/%s",cache_path,s->filename);
    if(sqlite3_open_v2(path,&s->db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_NOFOLLOW,NULL)!=SQLITE_OK)return EIO;
    sqlite3_busy_timeout(s->db,250);sqlite3_limit(s->db,SQLITE_LIMIT_LENGTH,1024*1024);
    sqlite3_db_config(s->db,SQLITE_DBCONFIG_DEFENSIVE,1,NULL);
    if(!sql(s,"PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA cache_size=-16384; PRAGMA mmap_size=0; PRAGMA temp_store=FILE; PRAGMA max_page_count=131072;"))return s->state.error;
    if(fresh){
        if(action!=ENABLE&&action!=REBUILD)return EINVAL;
        if(!sql(s,"BEGIN; PRAGMA application_id=0x4f504958; PRAGMA user_version=1;"
            "CREATE TABLE config(root BLOB NOT NULL,uid INTEGER NOT NULL,enabled INTEGER NOT NULL,paused INTEGER NOT NULL);"
            "CREATE TABLE entries(id INTEGER PRIMARY KEY,path BLOB NOT NULL UNIQUE,name TEXT NOT NULL,nonascii INTEGER NOT NULL);"
            "CREATE INDEX nonascii ON entries(nonascii);"
            "CREATE VIRTUAL TABLE names USING fts5(name,content='entries',content_rowid='id',tokenize='trigram case_sensitive 1',detail=none,columnsize=0);"))return EIO;
        sqlite3_stmt *q=NULL;sqlite3_prepare_v2(s->db,"INSERT INTO config VALUES(?,?,1,0)",-1,&q,NULL);
        sqlite3_bind_blob(q,1,s->selected,(int)strlen(s->selected),SQLITE_STATIC);sqlite3_bind_int64(q,2,getuid());
        int ok=sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);if(!ok||!sql(s,"COMMIT"))return EIO;
    }
    sqlite3_stmt *q=NULL;
    if(sqlite3_prepare_v2(s->db,"PRAGMA user_version",-1,&q,NULL)!=SQLITE_OK)return EINVAL;
    int version=sqlite3_step(q)==SQLITE_ROW?sqlite3_column_int(q,0):0;sqlite3_finalize(q);q=NULL;
    if(version!=1)return EINVAL;
    if(sqlite3_prepare_v2(s->db,"PRAGMA application_id",-1,&q,NULL)!=SQLITE_OK)return EINVAL;
    int app_id=sqlite3_step(q)==SQLITE_ROW?sqlite3_column_int(q,0):0;sqlite3_finalize(q);q=NULL;
    if(app_id!=0x4f504958)return EINVAL;
    if(sqlite3_prepare_v2(s->db,"SELECT root,uid,enabled,paused FROM config",-1,&q,NULL)!=SQLITE_OK)return EINVAL;
    int ok=sqlite3_step(q)==SQLITE_ROW&&sqlite3_column_bytes(q,0)==(int)strlen(s->selected)&&
        !memcmp(sqlite3_column_blob(q,0),s->selected,strlen(s->selected))&&sqlite3_column_int64(q,1)==getuid()&&sqlite3_column_int(q,2)==1;
    int paused=sqlite3_column_int(q,3);sqlite3_finalize(q);if(!ok)return EINVAL;
    s->state.enabled=1;s->state.state=paused?PAUSED:STALE;s->due=millis();
    if(action==ENABLE||action==RESUME||action==REBUILD){if(!meta(s,0))return EIO;s->state.state=STALE;}
    q=NULL;if(sqlite3_prepare_v2(s->db,"SELECT count(*) FROM entries",-1,&q,NULL)==SQLITE_OK&&sqlite3_step(q)==SQLITE_ROW)s->state.entries=sqlite3_column_int64(q,0);sqlite3_finalize(q);
    return 0;
}
static int watch_changes(indexer *s)
{
    if(s->notify<0)return 0;
    char data[65536] __attribute__((aligned(8)));int changed=0;
    for(;;){ssize_t n=read(s->notify,data,sizeof data);if(n<0){if(errno==EINTR)continue;if(errno!=EAGAIN)changed=1;break;}if(!n)break;
        for(size_t at=0;at<(size_t)n;){struct inotify_event *e=(void*)(data+at);if(e->mask)changed=1;at+=sizeof *e+e->len;}
    }return changed;
}
static void stale(indexer *s)
{if(s->state.state==PAUSED||!s->state.enabled)return;s->state.state=STALE;s->due=millis()+500;}
static int root_current(indexer *s)
{
    int fd=directory(s->selected,0);if(fd<0)return 0;struct stat st;
    int ok=!fstat(fd,&st)&&(uint64_t)st.st_dev==s->device&&(uint64_t)st.st_ino==s->inode&&mount_id(fd)==s->mount;
    close(fd);return ok;
}
static void query(indexer *s,uint32_t id,const uint8_t *data,uint32_t n);
static void command(indexer *s)
{
    wslp_head h;uint8_t data[sizeof(wslp_index_query)+PATH_CAP+2048];
    if(!io(0,&h,sizeof h,0)){s->quit=1;return;}
    if(!h.id||h.size>sizeof data||!io(0,data,h.size,0)){s->quit=1;return;}
    if(h.type==WSLP_INDEX_CONTROL&&h.size==4){uint32_t action;memcpy(&action,data,4);
        if(action>REMOVE){s->quit=1;return;}
        if(action==STATUS){
            if(!s->building&&(watch_changes(s)||(s->state.state==CURRENT&&!root_current(s))))stale(s);
            if(s->state.state==FAILED&&s->root_failed){int fd=directory(s->selected,0);if(fd>=0){close(fd);stale(s);}}
            status_reply(s,h.id);
        }
        else{s->pending=(int)action;s->pending_id=h.id;}
    }else if(h.type==WSLP_INDEX_ABORT&&!h.size){if(s->querying)s->qcancel=1;}
    else if(h.type==WSLP_INDEX_QUERY&&!s->querying)query(s,h.id,data,h.size);
    else s->quit=1;
}
static int pulse(indexer *s)
{
    uint64_t now=millis();if(now-s->pulsed<20)return !s->quit&&!s->pending;s->pulsed=now;
    struct pollfd p={0,POLLIN,0};if(poll(&p,1,0)>0){if(p.revents&POLLIN)command(s);else if(p.revents&(POLLHUP|POLLERR))s->quit=1;}
    return !s->quit&&!s->pending;
}
static int sql_pulse(void *user){return !pulse(user);}
static void walk(indexer *s,int fd,size_t length,unsigned depth,sqlite3_stmt *insert)
{
    struct stat st;if(fstat(fd,&st)){error(s,errno);close(fd);return;}
    uint64_t m=mount_id(fd);if(!m){error(s,ENOTSUP);close(fd);return;}
    if((uint64_t)st.st_dev!=s->device||m!=s->mount){s->state.skipped++;close(fd);return;}
    if((uint64_t)st.st_dev==s->cache_device&&(uint64_t)st.st_ino==s->cache_inode){s->state.skipped++;close(fd);return;}
    if(s->watches==WATCH_MAX){error(s,ENOSPC);close(fd);return;}
    char proc[64];snprintf(proc,sizeof proc,"/proc/self/fd/%d",fd);
    if(inotify_add_watch(s->notify,proc,IN_ONLYDIR|IN_CREATE|IN_DELETE|IN_MOVED_FROM|IN_MOVED_TO|IN_ATTRIB|IN_MODIFY|IN_CLOSE_WRITE|IN_DELETE_SELF|IN_MOVE_SELF|IN_UNMOUNT)<0){error(s,errno);close(fd);return;}s->watches++;
    DIR *d=fdopendir(fd);if(!d){error(s,errno);close(fd);return;}
    while(pulse(s)){
        errno=0;struct dirent *e=readdir(d);if(!e){if(errno)error(s,errno);break;}
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        if(s->state.scanned==ENTRY_MAX){error(s,EOVERFLOW);break;}
        if(fstatat(fd,e->d_name,&st,AT_SYMLINK_NOFOLLOW)){error(s,errno);continue;}
        if((uint64_t)st.st_dev==s->cache_device&&(uint64_t)st.st_ino==s->cache_inode){s->state.skipped++;continue;}
        size_t n=strlen(e->d_name),sep=length!=0;if(length+sep+n>PATH_CAP){error(s,ENAMETOOLONG);continue;}
        if(sep)s->relative[length]='/';memcpy(s->relative+length+sep,e->d_name,n+1);size_t bytes=length+sep+n;
        char name[256];int nonascii=0;for(size_t i=0;i<n;i++){unsigned char c=e->d_name[i];if(c>=128)nonascii=1;name[i]=c>='A'&&c<='Z'?c+32:c;}name[n]=0;
        sqlite3_reset(insert);sqlite3_clear_bindings(insert);
        sqlite3_bind_blob(insert,1,s->relative,(int)bytes,SQLITE_STATIC);
        sqlite3_bind_text(insert,2,nonascii?"":name,-1,SQLITE_STATIC);sqlite3_bind_int(insert,3,nonascii);
        if(sqlite3_step(insert)!=SQLITE_DONE){error(s,sqlite3_errcode(s->db)==SQLITE_FULL?ENOSPC:EIO);break;}s->state.scanned++;
        if(S_ISDIR(st.st_mode)){
            if(depth>=127)error(s,EOVERFLOW);
            else{int child=openat(fd,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);if(child<0)error(s,errno);else walk(s,child,bytes,depth+1,insert);}
        }else if(!S_ISREG(st.st_mode))s->state.skipped++;
        s->relative[length]=0;if(s->state.errors||s->quit||s->pending)break;
    }closedir(d);
}
static void reconcile(indexer *s)
{
    s->building=1;s->root_failed=0;s->state.state=SCANNING;s->state.scanned=s->state.errors=s->state.skipped=0;s->state.error=0;s->relative[0]=0;s->watches=0;
    if(s->notify>=0)close(s->notify);s->notify=inotify_init1(IN_NONBLOCK|IN_CLOEXEC);
    int root=directory(s->selected,0);struct stat st;
    if(s->notify<0||root<0){s->root_failed=root<0;error(s,errno);if(root>=0)close(root);goto done;}
    if(fstat(root,&st)||(s->mount=mount_id(root))==0){error(s,errno?errno:ENOTSUP);close(root);goto done;}
    s->device=st.st_dev;s->inode=st.st_ino;
    if(!sql(s,"BEGIN IMMEDIATE; DELETE FROM entries;")){error(s,s->state.error);close(root);goto done;}
    sqlite3_stmt *insert=NULL;
    if(sqlite3_prepare_v2(s->db,"INSERT INTO entries(path,name,nonascii) VALUES(?,?,?)",-1,&insert,NULL)!=SQLITE_OK){error(s,EIO);close(root);}
    else walk(s,root,0,0,insert);sqlite3_finalize(insert);
    if(!s->quit&&!s->pending&&!s->state.errors){
        sqlite3_progress_handler(s->db,10000,sql_pulse,s);
        int ok=sql(s,"INSERT INTO names(names) VALUES('rebuild'); INSERT INTO names(names) VALUES('optimize');");
        sqlite3_progress_handler(s->db,0,NULL,NULL);
        if(!ok&&!s->quit&&!s->pending)error(s,s->state.error);
    }
    if(s->quit||s->pending||s->state.errors||watch_changes(s)||!root_current(s)){
        sql(s,"ROLLBACK");if(!s->state.errors){s->state.state=STALE;s->due=millis()+(s->backoff<5?500u<<s->backoff++:16000);}
    }else if(sql(s,"COMMIT")){
        struct timespec stamp;clock_gettime(CLOCK_REALTIME,&stamp);
        s->state.state=CURRENT;s->state.entries=s->state.scanned;s->state.updated=(uint64_t)stamp.tv_sec*1000000000+stamp.tv_nsec;s->backoff=0;
    }else{sql(s,"ROLLBACK");error(s,EIO);}
done:
    sqlite3_progress_handler(s->db,0,NULL,NULL);
    if(!sqlite3_get_autocommit(s->db))sql(s,"ROLLBACK");
    s->building=0;if(s->state.errors)s->state.state=FAILED;
}
static int candidate(indexer *s,const char *path,struct stat *out)
{
    int fd=directory(s->selected,0);if(fd<0)return 0;struct stat root;
    if(fstat(fd,&root)||(uint64_t)root.st_dev!=s->device||(uint64_t)root.st_ino!=s->inode||mount_id(fd)!=s->mount){close(fd);stale(s);return 0;}
    for(const char *p=path;*p;){const char *end=strchr(p,'/');size_t n=end?(size_t)(end-p):strlen(p);char part[256];
        if(!n||n>255){close(fd);return 0;}memcpy(part,p,n);part[n]=0;
        if(!strcmp(part,".")||!strcmp(part,"..")){close(fd);return 0;}
        if(!end){int ok=!fstatat(fd,part,out,AT_SYMLINK_NOFOLLOW);close(fd);return ok;}
        int next=openat(fd,part,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);close(fd);if(next<0)return 0;fd=next;
        if(mount_id(fd)!=s->mount){close(fd);return 0;}p=end+1;
    }close(fd);return 0;
}
static void query(indexer *s,uint32_t id,const uint8_t *data,uint32_t n)
{
    wslp_search_end end={0};wslp_index_query q;
    if(n<sizeof q){s->quit=1;return;}memcpy(&q,data,sizeof q);
    if(!q.path_bytes||q.path_bytes>PATH_CAP||!q.glob_bytes||q.glob_bytes>2047||n!=sizeof q+q.path_bytes+q.glob_bytes){s->quit=1;return;}
    char scope[PATH_CAP+1],glob[2048];memcpy(scope,data+sizeof q,q.path_bytes);memcpy(glob,data+sizeof q+q.path_bytes,q.glob_bytes);glob[q.glob_bytes]=0;
    if(!valid_path(scope,q.path_bytes)||!beneath(scope,s->selected)||memchr(glob,0,q.glob_bytes)){s->quit=1;return;}
    for(uint32_t i=0;i<q.glob_bytes;i++)if((unsigned char)glob[i]>=128){s->quit=1;return;}
    if(!s->building&&watch_changes(s))stale(s);
    if(s->state.state!=CURRENT){end.error=EAGAIN;end.errors=1;goto done;}
    int access=directory(scope,0);struct stat identity;
    if(access<0){s->state.error=errno;stale(s);end.error=EAGAIN;end.errors=1;goto done;}
    if(mount_id(access)!=s->mount){close(access);end.error=EAGAIN;end.errors=1;goto done;}
    close(access);
    access=directory(s->selected,0);
    if(access<0||fstat(access,&identity)||(uint64_t)identity.st_dev!=s->device||(uint64_t)identity.st_ino!=s->inode||mount_id(access)!=s->mount){
        if(access>=0)close(access);stale(s);end.error=EAGAIN;end.errors=1;goto done;
    }close(access);
    sqlite3_stmt *statement=NULL;
    if(sqlite3_prepare_v2(s->db,"SELECT e.path FROM names n JOIN entries e ON e.id=n.rowid WHERE n.name GLOB ? AND e.nonascii=0 UNION ALL SELECT path FROM entries WHERE nonascii=1",-1,&statement,NULL)!=SQLITE_OK){end.error=EIO;end.errors=1;goto done;}
    sqlite3_bind_text(statement,1,glob,-1,SQLITE_STATIC);
    s->querying=1;s->qcancel=0;
    const char *prefix=scope+strlen(s->selected);if(*prefix=='/')prefix++;size_t prefix_len=strlen(prefix);
    size_t skip=prefix_len?prefix_len+1:0;
    int rc=SQLITE_DONE;
    while(!s->quit&&(rc=sqlite3_step(statement))==SQLITE_ROW){
        if(!pulse(s)||s->qcancel){end.error=ECANCELED;end.errors++;break;}
        const char *bytes=sqlite3_column_blob(statement,0);int size=sqlite3_column_bytes(statement,0);
        if(size<=0||size>PATH_CAP||memchr(bytes,0,(size_t)size)){end.error=EIO;end.errors++;break;}
        if(prefix_len&&((size_t)size<=prefix_len||memcmp(bytes,prefix,prefix_len)||bytes[prefix_len]!='/'))continue;
        char path[PATH_CAP+1];memcpy(path,bytes,(size_t)size);path[size]=0;struct stat st;
        if(!candidate(s,path,&st)){end.skipped++;if(s->state.state!=CURRENT)break;continue;}
        if(end.scanned==100000){end.error=EOVERFLOW;end.errors++;break;}end.scanned++;
        const char *relative=path+skip;size_t length=strlen(relative);
        wslp_search_entry entry={S_ISREG(st.st_mode)&&st.st_size>=0?(uint64_t)st.st_size:0,
            (int64_t)st.st_mtim.tv_sec*10000000+st.st_mtim.tv_nsec/100+116444736000000000LL,
            S_ISDIR(st.st_mode)?0x10u:S_ISREG(st.st_mode)?0x80u:0x400u,(uint32_t)length};
        uint8_t frame[sizeof entry+PATH_CAP];memcpy(frame,&entry,sizeof entry);memcpy(frame+sizeof entry,relative,length);
        if(!send_frame(s,WSLP_SEARCH_ENTRY,id,frame,(uint32_t)(sizeof entry+length)))break;
        if(watch_changes(s)){stale(s);break;}
    }
    if(rc!=SQLITE_DONE&&!end.error&&!s->quit&&s->state.state==CURRENT){end.error=EIO;end.errors++;}
    sqlite3_finalize(statement);
    s->querying=0;
    if(watch_changes(s))stale(s);
    if(s->state.state!=CURRENT){end.error=EAGAIN;end.errors++;}
done:
    status_reply(s,id);send_frame(s,WSLP_SEARCH_END,id,&end,sizeof end);
}
static void apply(indexer *s)
{
    int action=s->pending;uint32_t id=s->pending_id;s->pending=0;
    if(action==REMOVE){
        if(!sql(s,"BEGIN; UPDATE config SET enabled=0; COMMIT")){s->state.state=FAILED;s->state.error=EIO;status_reply(s,id);return;}
        sqlite3_close(s->db);s->db=NULL;int e=unlink_database(s);
        if(e){s->state.state=FAILED;s->state.error=e;s->state.enabled=0;status_reply(s,id);s->quit=1;return;}
        char side[80];snprintf(side,sizeof side,"%s.lock",s->filename);unlinkat(s->cache,side,0);
        s->state=(wslp_index_status){0};if(s->notify>=0){close(s->notify);s->notify=-1;}status_reply(s,id);s->quit=1;return;
    }
    if(!meta(s,action==PAUSE)){s->state.state=FAILED;s->state.error=EIO;}
    else{if(s->notify>=0){close(s->notify);s->notify=-1;}s->state.state=action==PAUSE?PAUSED:STALE;s->due=millis();s->state.error=0;s->state.errors=0;}
    status_reply(s,id);
}
int fp_index_main(void)
{
    signal(SIGPIPE,SIG_IGN);umask(077);indexer s={.cache=-1,.lock=-1,.notify=-1};
    wslp_hello hi={WSLP_MAGIC,WSLP_VERSION,getuid(),getpid()};send_frame(&s,WSLP_INDEX_HELLO,0,&hi,sizeof hi);
    wslp_head h;wslp_index_open q;
    if(!io(0,&h,sizeof h,0)||h.type!=WSLP_INDEX_OPEN||h.id!=1||h.size<sizeof q||h.size>sizeof q+PATH_CAP||!io(0,&q,sizeof q,0)||q.action>REMOVE||q.path_bytes!=h.size-sizeof q||!io(0,s.asked,q.path_bytes,0)||!valid_path(s.asked,q.path_bytes))return 1;
    int e=database(&s,(int)q.action);
    if(e==ENOENT&&q.action==STATUS){s.state=(wslp_index_status){0};s.selected[0]=0;status_reply(&s,1);goto cleanup;}
    if(e){s.state.state=FAILED;s.state.error=e;status_reply(&s,1);goto cleanup;}
    if(s.quit){status_reply(&s,1);goto cleanup;}
    if(q.action==PAUSE||q.action==REMOVE){s.pending=q.action;s.pending_id=1;apply(&s);}else status_reply(&s,1);
    while(!s.quit){
        if(s.pending){apply(&s);continue;}
        struct pollfd p[2]={{0,POLLIN,0},{s.notify,POLLIN,0}};
        int ready=poll(p,2,100);if(ready<0&&errno!=EINTR)break;
        if(p[0].revents&POLLIN){command(&s);if(s.pending||s.quit)continue;}
        if(p[0].revents&(POLLHUP|POLLERR))break;
        if(p[1].revents&&watch_changes(&s))stale(&s);
        if(s.state.state==CURRENT&&millis()-s.root_checked>=1000){s.root_checked=millis();if(!root_current(&s))stale(&s);}
        if(s.state.state==STALE&&millis()>=s.due)reconcile(&s);
    }
cleanup:
    if(s.notify>=0)close(s.notify);if(s.db)sqlite3_close(s.db);if(s.lock>=0)close(s.lock);if(s.cache>=0)close(s.cache);return 0;
}
