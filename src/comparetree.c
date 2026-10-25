/* Depth-first comparison. Only guarded directory anchors enter a listing. */
#include "internal.h"

typedef struct tree_scan {
    fpe_comparison *out;int hashes,root;fpe_compare_cb cb;void *user;
    uint32_t entries,capacity;uint64_t text;
} tree_scan;
static int live(tree_scan *s){if(s->cb&&!s->cb(s->user)){s->out->cancelled=1;return 0;}return !s->out->cancelled;}
static wchar_t *relative_join(const wchar_t *parent,const wchar_t *leaf)
{
    size_t a=wcslen(parent),b=wcslen(leaf);if(a+b+2>32700)return NULL;
    wchar_t *p=fpe_alloc((a+b+2)*sizeof *p);if(p){memcpy(p,parent,a*sizeof *p);if(a)p[a++]=L'\\';memcpy(p+a,leaf,(b+1)*sizeof *p);}return p;
}
static fpe_err retain(tree_scan *s,int side,fpe_dir *dir,int index,const wchar_t *prefix,int32_t *out,wchar_t **relative)
{
    *out=-1;*relative=NULL;if(index<0)return 0;
    const fpe_entry *e=fpe_dir_entries(dir)+index;*relative=relative_join(prefix,fpe_dir_name(dir,e));
    if(!*relative)return ERROR_FILENAME_EXCED_RANGE;
    size_t n=wcslen(*relative);if(s->text+(n+1)*sizeof(wchar_t)>64ull*1024*1024)return ERROR_BUFFER_OVERFLOW;
    *out=(int32_t)fpe_dir_count(s->out->dirs[side]);
    fpe_err err=fpe_dir_add(s->out->dirs[side],*relative,(uint32_t)n,e->size,e->mtime,e->ctime,e->attrs,e->reparse_tag);
    fpe_unix u;if(!err&&fpe_dir_unix(dir,e,&u))err=fpe_dir_add_unix(s->out->dirs[side],u.mode,u.uid,u.gid,u.link,u.link?(uint32_t)wcslen(u.link):0);
    if(!err){s->text+=(n+1)*sizeof(wchar_t);}return err;
}
static void scan(tree_scan *s,fpe_path_guard *a,fpe_path_guard *b,const wchar_t *pa,const wchar_t *pb,unsigned depth)
{
    if(!live(s)||s->out->error)return;
    fpe_comparison level={0};fpe_err err;
    /* A missing side is an empty snapshot, not a second read of its peer. */
    err=fpe_compare_level(a?a->path:NULL,b?b->path:NULL,s->hashes,s->root,s->cb,s->user,&level);
    if(err){if(err==ERROR_CANCELLED)s->out->cancelled=1;else s->out->error=err;fpe_compare_free(&level);return;}
    /* Charge every enumerated sibling now: ancestor snapshots stay alive
     * during descent, including rows which have not yet been appended. */
    uint32_t entries=fpe_dir_count(level.dirs[0])+fpe_dir_count(level.dirs[1]);
    if(entries>100000-s->entries){s->out->error=ERROR_BUFFER_OVERFLOW;fpe_compare_free(&level);return;}
    s->entries+=entries;
    for(uint32_t i=0;i<level.count&&live(s)&&!s->out->error;i++){
        fpe_compare_row row=level.rows[i];
        if(s->out->count==s->capacity){uint32_t n=s->capacity?s->capacity*2:256;if(n>100000)n=100000;
            if(n<=s->capacity){s->out->error=ERROR_BUFFER_OVERFLOW;break;}
            void *p=fpe_realloc(s->out->rows,n*sizeof row);if(!p){s->out->error=ERROR_NOT_ENOUGH_MEMORY;break;}s->out->rows=p;s->capacity=n;
        }
        wchar_t *rel[2]={0};int indices[]={row.left,row.right};
        err=retain(s,0,level.dirs[0],indices[0],pa,&row.left,&rel[0]);
        if(!err)err=retain(s,1,level.dirs[1],indices[1],pb,&row.right,&rel[1]);
        if(err){s->out->error=err;fpe_free(rel[0]);fpe_free(rel[1]);break;}
        uint32_t at=s->out->count++;s->out->rows[at]=row;s->out->completed=s->out->count;
        if(row.state==FPE_COMPARE_FOLDER){
            fpe_path_guard child[2]={{0}};fpe_path_guard *parents[]={a,b};
            if(depth>=128)err=ERROR_BUFFER_OVERFLOW;
            for(int side=0;!err&&side<2;side++)if(indices[side]>=0){
                const fpe_entry *e=fpe_dir_entries(level.dirs[side])+indices[side];
                err=fpe_guard_child(parents[side],fpe_dir_name(level.dirs[side],e),0,s->root,s->cb,s->user,&child[side]);
            }
            if(err){
                if(err==ERROR_CANCELLED)s->out->cancelled=1;
                else if(err==ERROR_NOT_SAME_DEVICE||err==ERROR_CANT_ACCESS_FILE)s->out->rows[at].state=FPE_COMPARE_EXCLUDED;
                else{s->out->rows[at].state=FPE_COMPARE_ERROR;s->out->rows[at].error=err;s->out->error=err;}
            }else scan(s,indices[0]>=0?&child[0]:NULL,indices[1]>=0?&child[1]:NULL,rel[0]?rel[0]:pa,rel[1]?rel[1]:pb,depth+1);
            fpe_guard_close(&child[1]);fpe_guard_close(&child[0]);
        }
        fpe_free(rel[0]);fpe_free(rel[1]);
    }
    fpe_compare_free(&level);
}
FPE_API fpe_err fpe_compare_tree(const wchar_t *left,const wchar_t *right,int hashes,int root,fpe_compare_cb cb,void *user,fpe_comparison *out)
{
    if(!out)return ERROR_INVALID_PARAMETER;memset(out,0,sizeof *out);
    if(!left||!*left||!right||!*right)return out->error=ERROR_INVALID_PARAMETER;
    fpe_path_guard g[2]={{0}};const wchar_t *paths[]={left,right};tree_scan s={out,hashes,root,cb,user};
    for(int side=0;side<2;side++){
        out->dirs[side]=fpe_dir_new(paths[side],256);if(!out->dirs[side]){out->error=ERROR_NOT_ENOUGH_MEMORY;break;}
        out->error=fpe_guard_open(paths[side],0,0,0,root,cb,user,&g[side]);if(out->error)break;
    }
    if(!out->error)scan(&s,&g[0],&g[1],L"",L"",0);
    if(out->error==ERROR_CANCELLED){out->cancelled=1;out->error=0;}
    for(int side=0;side<2;side++){if(out->dirs[side])fpe_dir_finish(out->dirs[side]);fpe_guard_close(&g[side]);}
    return out->cancelled?ERROR_CANCELLED:out->error;
}
