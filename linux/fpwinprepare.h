/* Copy-only Windows preparation credits. Included after source/metadata guards.
 * One owner performs reads, wire I/O, validation and ordered commits. A credit
 * means only that a private Windows stage exists; sources stay held until a
 * correlated durable result. No source removal or worker thread is involved. */
#ifndef FPWINPREPARE_H
#define FPWINPREPARE_H
#include <strings.h>
typedef struct win_pending {
    int in,parent;
    char *name,*source,*dest,*leaf;
    struct stat version;
    uint64_t token,bytes;
} win_pending;
static struct {
    win_pending slots[WSLP_PREPARE_MAX];
    unsigned width,used,group;
    uint64_t serial;
    int failed,broken;
} WQ;
static int win_prepare_reply(uint32_t type,uint64_t token,int *error)
{
    wslp_head h;wslp_file_result result;
    for(;;){
        if(!io(0,&h,sizeof h,0)||h.id!=J.id)break;
        if(h.type==WSLP_JOB_CANCEL&&!h.size){J.cancelled=1;continue;}
        if(h.type!=type||h.size!=sizeof result||!io(0,&result,sizeof result,0)||
           result.token!=token||result.pad||result.error<0||result.error>4095)break;
        *error=result.error;return 1;
    }
    WQ.broken=WQ.failed=1;J.cancelled=1;return 0;
}
static void win_pending_close(win_pending *p)
{
    if(p->in>=0)close(p->in);if(p->parent>=0)close(p->parent);
    free(p->name);free(p->source);free(p->dest);free(p->leaf);memset(p,0,sizeof *p);p->in=p->parent=-1;
}
static int win_prepare_group(void);
static int win_prepare_drain(void)
{
    if(WQ.group&&WQ.used)return win_prepare_group();
    for(unsigned i=0;i<WQ.used;i++){
        win_pending *p=&WQ.slots[i];int error=0;
        if(cancel())error=ECANCELED;else if(WQ.failed)error=EIO;
        if(!error&&!source_intact(p->parent,p->name,p->in,&p->version,0))error=ESTALE;
        fp_meta empty={0};if(!error)error=fp_meta_unchanged(p->in,&empty);
        wslp_file_result command={p->token,error,0};int host_error=0;
        if(!WQ.broken){
            if(!send(WSLP_FILE_COMMIT,&command,sizeof command))WQ.broken=WQ.failed=1;
            else if(win_prepare_reply(WSLP_FILE_COMMITTED,p->token,&host_error)&&!error)error=host_error;
        }
        if(WQ.broken&&!error)error=EPIPE;
        if(error){WQ.failed=1;if(!J.cancelled)event(0,error,p->dest);}
        else {J.p.bytes_done+=p->bytes;J.p.items_done++;progress(p->source,0);}
        if(error&&!J.cancelled)J.p.items_done++;
        win_pending_close(p);
    }
    WQ.used=0;return !WQ.failed;
}
static int win_prepare_ascii(const char *name)
{for(const unsigned char *p=(const unsigned char*)name;*p;p++)if(*p>=128)return 0;return 1;}
/* Pending leaves are ASCII only, in the current directory. Drain before case
 * aliases, autonumbering against an existing target, or unsupported names. */
static void win_prepare_conflict(const char *name)
{
    if(!WQ.used)return;
    if(!win_prepare_ascii(name)){win_prepare_drain();return;}
    for(unsigned i=0;i<WQ.used;i++)if(!strcasecmp(name,WQ.slots[i].leaf)){win_prepare_drain();return;}
}
static int win_prepare_file(int in,int sf,const char *sn,const char *dn,const struct stat *st,const char *sp,const char *dp)
{
    if(WQ.used==WQ.width&&!win_prepare_drain())return 0;
    if(WQ.failed||cancel())return 0;
    win_pending *p=&WQ.slots[WQ.used];memset(p,0,sizeof *p);p->in=p->parent=-1;
    p->name=strdup(sn);p->source=strdup(sp);p->dest=strdup(dp);p->leaf=strdup(dn);
    p->parent=fcntl(sf,F_DUPFD_CLOEXEC,0);p->in=fcntl(in,F_DUPFD_CLOEXEC,0);p->version=*st;
    int error=!p->name||!p->source||!p->dest||!p->leaf?ENOMEM:p->parent<0||p->in<0?errno:0;
    if(error){win_pending_close(p);event(0,error,sp);WQ.failed=1;return 0;}
    p->token=++WQ.serial;p->bytes=(uint64_t)st->st_size;
    const char *suffix;wslp_prepared_file f={.file={.atime=(int64_t)st->st_atim.tv_sec*1000000000+st->st_atim.tv_nsec,
        .mtime=(int64_t)st->st_mtim.tv_sec*1000000000+st->st_mtim.tv_nsec},.token=p->token,.bytes=p->bytes};
    identify(dp,&f.file.root,&suffix);f.file.path_bytes=(uint32_t)strlen(suffix);
    size_t size=sizeof f+f.file.path_bytes;char *packet=malloc(size),*buf=malloc(65536);
    if(!packet||!buf){free(packet);free(buf);win_pending_close(p);event(0,ENOMEM,sp);WQ.failed=1;return 0;}
    memcpy(packet,&f,sizeof f);memcpy(packet+sizeof f,suffix,f.file.path_bytes);
    int sent=send(WSLP_FILE_PREPARE,packet,(uint32_t)size);free(packet);
    uint64_t remaining=p->bytes;
    while(sent&&remaining&&!cancel()){
        size_t need=remaining<65536?(size_t)remaining:65536;ssize_t n=read(in,buf,need);
        if(n<0&&errno==EINTR)continue;
        if(n<=0){error=n<0?errno:ESTALE;break;}
        sent=send(WSLP_FILE_DATA,buf,(uint32_t)n);remaining-=(uint64_t)n;progress(sp,0);
    }
    free(buf);
    if(!error&&!cancel()&&!source_intact(sf,sn,in,st,0))error=ESTALE;
    fp_meta empty={0};if(!error&&!cancel())error=fp_meta_unchanged(in,&empty);
    int32_t end=cancel()?ECANCELED:error;int host_error=0;
    if(sent)sent=send(WSLP_FILE_END,&end,sizeof end);
    if(!sent){WQ.broken=WQ.failed=1;J.cancelled=1;}
    else if(win_prepare_reply(WSLP_FILE_PREPARED,p->token,&host_error)&&!error)error=host_error;
    if(WQ.broken||error){
        if(error&&!J.cancelled)event(0,error,dp);win_pending_close(p);WQ.failed=1;return 0;
    }
    WQ.used++;return 2; /* deferred accounting, never a durable success */
}
#include "fpwingroup.h"
#endif
