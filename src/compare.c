/* Current-level comparison: immutable snapshots, bounded rows, no UI work. */
#include "internal.h"
#include "../linux/fpsha256.h"
#include <stdlib.h>

FPE_API fpe_err fpe_file_hash(const wchar_t *path,int root,fpe_compare_cb cb,void *user,fpe_hash *out)
{
    if(!out)return ERROR_INVALID_PARAMETER;memset(out,0,sizeof *out);
    if(!path||!path[0])return ERROR_INVALID_PARAMETER;
    if(cb&&!cb(user))return ERROR_CANCELLED;
    if(fpe_wsl_path_kind(path))return fpe_wsl_hash(path,root,cb,user,out);
    wchar_t *nt=NULL;fpe_err err=fpe_path_nt(path,&nt);if(err)return err;
    HANDLE h=CreateFileW(nt,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_SEQUENTIAL_SCAN,NULL);fpe_free(nt);
    if(h==INVALID_HANDLE_VALUE)return GetLastError();
    BY_HANDLE_FILE_INFORMATION before,after;uint8_t *buf=NULL;fp_sha256 sha;fp_sha_init(&sha);uint64_t bytes=0;
    if(GetFileType(h)!=FILE_TYPE_DISK)err=ERROR_NOT_SUPPORTED;
    else if(!GetFileInformationByHandle(h,&before))err=GetLastError();
    else if(before.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_DEVICE))err=ERROR_NOT_SUPPORTED;
    else if(!(buf=fpe_alloc(65536)))err=ERROR_NOT_ENOUGH_MEMORY;
    while(!err){
        if(cb&&!cb(user)){err=ERROR_CANCELLED;break;}
        DWORD n;if(!ReadFile(h,buf,65536,&n,NULL)){err=GetLastError();break;}if(!n)break;
        fp_sha_update(&sha,buf,n);bytes+=n;
    }
    if(!err){
        if(!GetFileInformationByHandle(h,&after))err=GetLastError();
        else if(before.nFileSizeHigh!=after.nFileSizeHigh||before.nFileSizeLow!=after.nFileSizeLow||
            memcmp(&before.ftLastWriteTime,&after.ftLastWriteTime,sizeof(FILETIME))||bytes!=(((uint64_t)after.nFileSizeHigh<<32)|after.nFileSizeLow))err=ERROR_FILE_INVALID;
        else{out->size=bytes;out->mtime=((int64_t)after.ftLastWriteTime.dwHighDateTime<<32)|after.ftLastWriteTime.dwLowDateTime;fp_sha_final(&sha,out->sha256);}
    }
    fpe_free(buf);CloseHandle(h);return err;
}
typedef struct keyed {const wchar_t *name;int32_t index;int side;} keyed;
static int keycmp(void *context,const void *a,const void *b)
{
    const keyed *x=a,*y=b;return CompareStringOrdinal(x->name,-1,y->name,-1,*(int*)context)==CSTR_LESS_THAN?-1:
        CompareStringOrdinal(x->name,-1,y->name,-1,*(int*)context)==CSTR_GREATER_THAN?1:0;
}
static int regular(fpe_dir *dir,const fpe_entry *entry)
{
    if(entry->attrs&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DEVICE))return 0;
    fpe_unix u;if(fpe_dir_unix(dir,entry,&u)&&(u.mode&0170000)!=0100000)return 0;return 1;
}
static wchar_t *full(const wchar_t *dir,const wchar_t *name)
{
    size_t d=wcslen(dir),n=wcslen(name);if(d+n+2>FPE_MAX_PATH)return NULL;
    wchar_t *p=fpe_alloc((d+n+2)*sizeof *p);if(p){memcpy(p,dir,d*sizeof *p);p[d]=L'\\';memcpy(p+d+1,name,(n+1)*sizeof *p);}return p;
}
FPE_API void fpe_compare_free(fpe_comparison *r)
{
    if(!r)return;fpe_dir_release(r->dirs[0]);fpe_dir_release(r->dirs[1]);fpe_free(r->rows);memset(r,0,sizeof *r);
}
fpe_err fpe_compare_level(const wchar_t *left,const wchar_t *right,int hashes,int root,fpe_compare_cb cb,void *user,fpe_comparison *out)
{
    if(!out)return ERROR_INVALID_PARAMETER;memset(out,0,sizeof *out);
    if((!left&&!right)||(left&&!left[0])||(right&&!right[0]))return out->error=ERROR_INVALID_PARAMETER;
    const wchar_t *paths[]={left,right};keyed *keys=NULL;uint32_t n=0;
    for(int side=0;side<2;side++){
        if(cb&&!cb(user)){out->cancelled=1;goto done;}
        if(paths[side])out->error=fpe_wsl_path_kind(paths[side])?fpe_wsl_read(paths[side],&out->dirs[side]):fpe_dir_read(paths[side],&out->dirs[side]);
        else{out->dirs[side]=fpe_dir_new(L"",256);if(out->dirs[side])fpe_dir_finish(out->dirs[side]);else out->error=ERROR_NOT_ENOUGH_MEMORY;}
        if(out->error)goto done;
        uint32_t count=fpe_dir_count(out->dirs[side]);if(count>100000-n){out->error=ERROR_BUFFER_OVERFLOW;goto done;}n+=count;
    }
    if(cb&&!cb(user)){out->cancelled=1;goto done;}
    keys=fpe_alloc((n?n:1)*sizeof *keys);out->rows=fpe_zalloc((n?n:1)*sizeof *out->rows);
    if(!keys||!out->rows){out->error=ERROR_NOT_ENOUGH_MEMORY;goto done;}
    uint32_t at=0;
    for(int side=0;side<2;side++)for(uint32_t i=0;i<fpe_dir_count(out->dirs[side]);i++){
        keys[at++]=(keyed){fpe_dir_name(out->dirs[side],fpe_dir_entries(out->dirs[side])+i),(int32_t)i,side};
    }
    int fold=!fpe_dir_is_linux(out->dirs[0])&&!fpe_dir_is_linux(out->dirs[1]);qsort_s(keys,n,sizeof *keys,keycmp,&fold);
    for(uint32_t i=0;i<n;){
        if(cb&&!cb(user)){out->cancelled=1;goto done;}
        uint32_t end=i+1;while(end<n&&!keycmp(&fold,&keys[i],&keys[end]))end++;
        int counts[2]={0};for(uint32_t k=i;k<end;k++)counts[keys[k].side]++;
        if(counts[0]>1||counts[1]>1){
            for(uint32_t k=i;k<end;k++){fpe_compare_row *r=out->rows+out->count++;*r=(fpe_compare_row){-1,-1,FPE_COMPARE_AMBIGUOUS,0};if(keys[k].side)r->right=keys[k].index;else r->left=keys[k].index;}
            out->completed=out->count;i=end;continue;
        }
        fpe_compare_row *r=out->rows+out->count++;*r=(fpe_compare_row){-1,-1,0,0};
        for(uint32_t k=i;k<end;k++)if(keys[k].side)r->right=keys[k].index;else r->left=keys[k].index;
        const fpe_entry *a=r->left>=0?fpe_dir_entries(out->dirs[0])+r->left:NULL,*b=r->right>=0?fpe_dir_entries(out->dirs[1])+r->right:NULL;
        int af=a&&regular(out->dirs[0],a),bf=b&&regular(out->dirs[1],b);
        if((a&&!af&&!fpe_entry_is_dir(a))||(b&&!bf&&!fpe_entry_is_dir(b))||
            (a&&(a->attrs&FILE_ATTRIBUTE_REPARSE_POINT))||(b&&(b->attrs&FILE_ATTRIBUTE_REPARSE_POINT)))r->state=FPE_COMPARE_EXCLUDED;
        else if(a&&b&&fpe_entry_is_dir(a)!=fpe_entry_is_dir(b))r->state=FPE_COMPARE_TYPE;
        else if((a&&fpe_entry_is_dir(a))||(b&&fpe_entry_is_dir(b)))r->state=FPE_COMPARE_FOLDER;
        else if(!a)r->state=FPE_COMPARE_RIGHT;
        else if(!b)r->state=FPE_COMPARE_LEFT;
        else if(a->size!=b->size)r->state=FPE_COMPARE_SIZE;
        else if(!hashes)r->state=a->mtime==b->mtime?FPE_COMPARE_METADATA:FPE_COMPARE_DATE;
        else{
            fpe_hash h[2];for(int side=0;side<2;side++){
                const keyed *k=&keys[i];if(k->side!=side)k=&keys[i+1];wchar_t *path=full(paths[side],k->name);
                r->error=path?fpe_file_hash(path,root,cb,user,&h[side]):ERROR_FILENAME_EXCED_RANGE;fpe_free(path);
                if(r->error)break;
                const fpe_entry *expected=side?b:a;
                if(h[side].size!=expected->size||h[side].mtime!=expected->mtime){r->error=ERROR_FILE_INVALID;break;}
            }
            if(r->error){r->state=FPE_COMPARE_ERROR;if(r->error==ERROR_CANCELLED){out->cancelled=1;goto done;}}
            else r->state=memcmp(h[0].sha256,h[1].sha256,32)?FPE_COMPARE_CONTENT:FPE_COMPARE_EQUAL;
        }
        out->completed=out->count;i=end;
    }
done:
    fpe_free(keys);if(out->cancelled)return ERROR_CANCELLED;return out->error;
}

FPE_API fpe_err fpe_compare_folders(const wchar_t *left,const wchar_t *right,int hashes,int root,fpe_compare_cb cb,void *user,fpe_comparison *out)
{
    if(!out)return ERROR_INVALID_PARAMETER;
    if(!left||!right){memset(out,0,sizeof *out);return out->error=ERROR_INVALID_PARAMETER;}
    return fpe_compare_level(left,right,hashes,root,cb,user,out);
}
