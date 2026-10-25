/* Worker-owned preview and non-replacing rename batches. */
#include "internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static int component(const wchar_t *s)
{
    size_t n=0;while(n<260&&s[n])n++;
    if(!n||n>255||!wcscmp(s,L".")||!wcscmp(s,L"..")||wcschr(s,L'\\')||wcschr(s,L'/'))return 0;
    return 1;
}
static int same_win(const wchar_t *a,const wchar_t *b){return CompareStringOrdinal(a,-1,b,-1,TRUE)==CSTR_EQUAL;}
static int win_name(const wchar_t *s)
{
    size_t n=wcslen(s);if(s[n-1]==L'.'||s[n-1]==L' ')return 0;
    for(const wchar_t *p=s;*p;p++)if(*p<32||wcschr(L"<>:\"/|?*",*p))return 0;
    wchar_t base[260];wcscpy(base,s);wchar_t *dot=wcschr(base,L'.');if(dot)*dot=0;
    size_t k=wcslen(base);while(k&&base[k-1]==L' ')base[--k]=0;
    if(!_wcsicmp(base,L"CON")||!_wcsicmp(base,L"PRN")||!_wcsicmp(base,L"AUX")||!_wcsicmp(base,L"NUL")||!_wcsicmp(base,L"CONIN$")||!_wcsicmp(base,L"CONOUT$"))return 0;
    if(k==4&&(!_wcsnicmp(base,L"COM",3)||!_wcsnicmp(base,L"LPT",3))&&
       ((base[3]>=L'1'&&base[3]<=L'9')||base[3]==0xb9||base[3]==0xb2||base[3]==0xb3))return 0;
    return WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s,-1,NULL,0,NULL,NULL)>0;
}
static wchar_t *full(const wchar_t *parent,const wchar_t *name)
{
    size_t n=wcslen(parent),k=wcslen(name),sep=n&&parent[n-1]!=L'\\';
    if(n+sep+k>=32767){SetLastError(ERROR_FILENAME_EXCED_RANGE);return NULL;}
    wchar_t *p=fpe_alloc((n+sep+k+1)*sizeof *p);if(!p){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return NULL;}
    memcpy(p,parent,n*sizeof *p);if(sep)p[n++]=L'\\';memcpy(p+n,name,(k+1)*sizeof *p);return p;
}
static DWORD identity(HANDLE h,fpe_rename_identity *out)
{
    BY_HANDLE_FILE_INFORMATION s;if(!GetFileInformationByHandle(h,&s))return GetLastError();
    *out=(fpe_rename_identity){s.dwVolumeSerialNumber,((uint64_t)s.nFileIndexHigh<<32)|s.nFileIndexLow,
        ((uint64_t)s.ftCreationTime.dwHighDateTime<<32)|s.ftCreationTime.dwLowDateTime,0,
        s.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)};return 0;
}
static HANDLE open_item(const wchar_t *parent,const wchar_t *name,int write,DWORD *error)
{
    wchar_t *p=full(parent,name);if(!p){*error=GetLastError();return INVALID_HANDLE_VALUE;}
    HANDLE h=CreateFileW(p,FILE_READ_ATTRIBUTES|(write?DELETE:0),FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_NO_RECALL,NULL);
    *error=h==INVALID_HANDLE_VALUE?GetLastError():0;fpe_free(p);return h;
}
static DWORD inspect(const wchar_t *parent,fpe_rename_item *item,int apply,HANDLE *held)
{
    DWORD err=0;HANDLE h=open_item(parent,item->from,apply,&err);*held=h;
    if(err)return err;fpe_rename_identity current;err=identity(h,&current);if(err)return err;
    if(apply&&memcmp(&current,&item->identity,sizeof current))return ERROR_FILE_INVALID;
    if(!apply)item->identity=current;
    if(!wcscmp(item->from,item->to))return 0;
    HANDLE target=open_item(parent,item->to,0,&err);
    if(target!=INVALID_HANDLE_VALUE){
        fpe_rename_identity other;err=identity(target,&other);CloseHandle(target);
        if(err)return err;
        if(same_win(item->from,item->to)&&!memcmp(&current,&other,sizeof current))return 0;
        return ERROR_FILE_EXISTS;
    }
    return err==ERROR_FILE_NOT_FOUND||err==ERROR_PATH_NOT_FOUND?0:err;
}
static void mark(fpe_rename_item *items,uint32_t count,uint32_t state,DWORD error)
{
    for(uint32_t i=0;i<count;i++)if(items[i].state==FPE_RENAME_PENDING||items[i].state==FPE_RENAME_READY){items[i].state=state;items[i].error=error;}
}
FPE_API fpe_err fpe_rename_batch(const wchar_t *folder,int as_root,fpe_rename_item *items,uint32_t count,int apply,fpe_rename_cb cb,void *user)
{
    if(!folder||!*folder||!items||!count||count>FPE_RENAME_MAX||(apply!=0&&apply!=1))return ERROR_INVALID_PARAMETER;
    int linux=fpe_wsl_path_kind(folder);DWORD first=0;int stopped=0;
    for(uint32_t i=0;i<count;i++){
        int valid=component(items[i].from)&&component(items[i].to)&&
            (linux||(win_name(items[i].from)&&win_name(items[i].to)));
        if(apply&&(items[i].error||(items[i].state!=FPE_RENAME_READY&&items[i].state!=FPE_RENAME_UNCHANGED)))valid=0;
        items[i].error=valid?0:ERROR_INVALID_NAME;items[i].state=valid?FPE_RENAME_PENDING:FPE_RENAME_ERROR;
        if(!valid)first=ERROR_INVALID_NAME;
    }
    if(first){mark(items,count,FPE_RENAME_BLOCKED,0);return first;}
    for(uint32_t i=0;i<count;i++)for(uint32_t j=0;j<i;j++){
        int duplicate=linux?!wcscmp(items[i].from,items[j].from)||!wcscmp(items[i].to,items[j].to):
            same_win(items[i].from,items[j].from)||same_win(items[i].to,items[j].to);
        if(duplicate){items[i].state=items[j].state=FPE_RENAME_ERROR;items[i].error=items[j].error=first=ERROR_FILE_EXISTS;}
    }
    if(first){mark(items,count,FPE_RENAME_BLOCKED,0);return first;}
    if(linux)return fpe_wsl_rename_batch(folder,as_root,items,count,apply,cb,user);
    wchar_t *parent=NULL;first=fpe_path_nt(folder,&parent);
    if(first){mark(items,count,FPE_RENAME_ERROR,first);return first;}
    for(uint32_t i=0;i<count;i++){
        if(cb&&!cb(user,i,0)){stopped=1;break;}
        HANDLE h;DWORD err=inspect(parent,&items[i],apply,&h);if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);
        items[i].error=err;items[i].state=err?FPE_RENAME_ERROR:!wcscmp(items[i].from,items[i].to)?FPE_RENAME_UNCHANGED:FPE_RENAME_READY;
        if(err&&!first)first=err;
    }
    if(stopped)mark(items,count,FPE_RENAME_STOPPED,0);
    else if(apply&&first)mark(items,count,FPE_RENAME_BLOCKED,0);
    else if(apply)for(uint32_t i=0;i<count;i++){
        if(cb&&!cb(user,i,1)){stopped=1;mark(items,count,FPE_RENAME_STOPPED,0);break;}
        if(items[i].state==FPE_RENAME_UNCHANGED)continue;
        HANDLE h;DWORD err=inspect(parent,&items[i],1,&h);
        if(!err){
            wchar_t *to=full(parent,items[i].to);size_t bytes=to?wcslen(to)*sizeof(wchar_t):0;
            FILE_RENAME_INFO *info=to?fpe_zalloc(offsetof(FILE_RENAME_INFO,FileName)+bytes+sizeof(wchar_t)):NULL;
            if(!info)err=to?ERROR_NOT_ENOUGH_MEMORY:GetLastError();
            else{
                info->ReplaceIfExists=FALSE;info->FileNameLength=(DWORD)bytes;memcpy(info->FileName,to,bytes);
                if(!SetFileInformationByHandle(h,FileRenameInfo,info,(DWORD)(offsetof(FILE_RENAME_INFO,FileName)+bytes)))err=GetLastError();
                fpe_free(info);
            }
            fpe_free(to);
        }
        if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);
        items[i].error=err;items[i].state=err?FPE_RENAME_ERROR:FPE_RENAME_RENAMED;if(err&&!first)first=err;
    }
    if(cb)cb(user,count,apply);fpe_free(parent);return stopped?ERROR_CANCELLED:first;
}
