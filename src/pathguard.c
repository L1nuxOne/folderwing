#include "internal.h"
#include <stdlib.h>
#include <winternl.h>
/* Empty-name relative opens address the held directory itself. ReOpenFile
 * rejects these directory handles on NTFS; a pathname reopen could traverse a
 * reparse point during the flush handoff. Never substitute a pathname here. */
HANDLE fpe_win_reopen_directory(HANDLE directory,DWORD access,DWORD share)
{
    typedef NTSTATUS (NTAPI *create_fn)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,PLARGE_INTEGER,ULONG,ULONG,ULONG,ULONG,PVOID,ULONG);
    typedef ULONG (WINAPI *error_fn)(NTSTATUS);
    HMODULE dll=GetModuleHandleW(L"ntdll.dll");create_fn open=(create_fn)GetProcAddress(dll,"NtCreateFile");error_fn error=(error_fn)GetProcAddress(dll,"RtlNtStatusToDosError");
    if(!open||!error){SetLastError(ERROR_NOT_SUPPORTED);return INVALID_HANDLE_VALUE;}
    UNICODE_STRING name={.Buffer=L""};OBJECT_ATTRIBUTES object={.Length=sizeof object,.RootDirectory=directory,.ObjectName=&name,.Attributes=0x40};IO_STATUS_BLOCK io;HANDLE h=NULL;
    NTSTATUS status=open(&h,access|SYNCHRONIZE,&object,&io,NULL,0,share,1,0x204021,NULL,0);
    if(status<0){SetLastError(error(status));return INVALID_HANDLE_VALUE;}return h;
}
static fpe_err guard_info(HANDLE h,BY_HANDLE_FILE_INFORMATION *info)
{
    if(!GetFileInformationByHandle(h,info))return GetLastError();
    return !(info->dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||info->dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT?ERROR_CANT_ACCESS_FILE:0;
}
static int guard_same(const BY_HANDLE_FILE_INFORMATION *a,const BY_HANDLE_FILE_INFORMATION *b)
{return a->dwVolumeSerialNumber==b->dwVolumeSerialNumber&&a->nFileIndexHigh==b->nFileIndexHigh&&a->nFileIndexLow==b->nFileIndexLow;}
fpe_err fpe_guard_sync(fpe_path_guard *g,fpe_path_guard *const *held,uint32_t count,fpe_compare_cb cb,void *user)
{
    if(cb&&!cb(user))return ERROR_CANCELLED;
    if(g->linux)return fpe_wsl_guard_sync(g,cb,user);
    BY_HANDLE_FILE_INFORMATION before,now;fpe_err error=guard_info(g->directory,&before);if(error)return error;
    if(!held||!count||count>258)return ERROR_INVALID_PARAMETER;
    unsigned char peers[258]={0};int found=0;
    for(uint32_t i=0;i<count;i++)if(held[i]&&!held[i]->linux&&held[i]->directory){
        error=guard_info(held[i]->directory,&now);if(error)return error;
        if(guard_same(&before,&now)){if(!held[i]->barrier)return ERROR_INVALID_HANDLE;peers[i]=1;if(held[i]==g)found=1;}
    }
    if(!found)return ERROR_INVALID_PARAMETER;
    error=fpe_win_ancestry_exchange(g->ancestry,g->directory,0);if(error)return error;
    /* All matching name pins continue denying deletion/rename. Only this
     * synchronous, handle-relative operation runs while their write barriers
     * are exchanged. No pathname walk or user callback occurs in this window.
     * Duplicate source/destination guards for one object are exchanged together. */
    for(uint32_t i=0;i<count;i++)if(peers[i]){CloseHandle(held[i]->barrier);held[i]->barrier=NULL;}
    HANDLE write=fpe_win_reopen_directory(g->directory,GENERIC_WRITE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE);
    error=write==INVALID_HANDLE_VALUE?GetLastError():guard_info(write,&now);
    if(!error&&!guard_same(&before,&now))error=ERROR_FILE_INVALID;
    if(!error&&!FlushFileBuffers(write))error=GetLastError();
    if(write!=INVALID_HANDLE_VALUE)CloseHandle(write);
    for(uint32_t i=0;i<count;i++)if(peers[i]){
        HANDLE barrier=fpe_win_reopen_directory(held[i]->directory,FILE_TRAVERSE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ);
        fpe_err e=barrier==INVALID_HANDLE_VALUE?GetLastError():guard_info(barrier,&now);
        if(barrier!=INVALID_HANDLE_VALUE){held[i]->barrier=barrier;if(!e&&!guard_same(&before,&now))e=ERROR_FILE_INVALID;}
        if(e&&!error)error=e;
    }
    {fpe_err e=fpe_win_ancestry_exchange(g->ancestry,g->directory,1);if(e&&!error)error=e;}
    if(!error&&cb&&!cb(user))error=ERROR_CANCELLED;
    return error;
}
/* Only relative components may be appended to captured roots. Windows output
 * rejects names which the platform would reinterpret as ADS/devices/aliases. */
int fpe_relative_valid(const wchar_t *p,int windows)
{
    if(!p||!*p||wcslen(p)>32700)return 0;const wchar_t *part=p;
    for(const wchar_t *c=p;;c++)if(!*c||*c==L'\\'){
        size_t n=(size_t)(c-part);if(!n||n>255||(n==1&&part[0]==L'.')||(n==2&&part[0]==L'.'&&part[1]==L'.'))return 0;
        if(windows){
            if(part[n-1]==L'.'||part[n-1]==L' ')return 0;
            wchar_t stem[260];size_t k=0;int extension=0;
            for(size_t i=0;i<n;i++){wchar_t x=part[i];if(x<32||wcschr(L"/:*?\"<>|",x)||(x>=0xf000&&x<=0xf0ff))return 0;if(x==L'.')extension=1;if(!extension)stem[k++]=x;}
            stem[k]=0;if(!_wcsicmp(stem,L"CON")||!_wcsicmp(stem,L"PRN")||!_wcsicmp(stem,L"AUX")||!_wcsicmp(stem,L"NUL"))return 0;
            if(k==4&&(!_wcsnicmp(stem,L"COM",3)||!_wcsnicmp(stem,L"LPT",3))&&((stem[3]>=L'1'&&stem[3]<=L'9')||stem[3]==0xb9||stem[3]==0xb2||stem[3]==0xb3))return 0;
        }else for(size_t i=0;i<n;i++)if(part[i]==L'/')return 0;
        if(!*c)return 1;part=c+1;
    }
}
void fpe_guard_close(fpe_path_guard *g)
{
    if(g->barrier&&g->barrier!=INVALID_HANDLE_VALUE)CloseHandle(g->barrier);
    if(g->directory&&g->directory!=INVALID_HANDLE_VALUE)CloseHandle(g->directory);
    if(g->to)CloseHandle(g->to);if(g->from)CloseHandle(g->from);
    if(g->process){if(WaitForSingleObject(g->process,250)==WAIT_TIMEOUT)TerminateProcess(g->process,1);CloseHandle(g->process);}
    fpe_free(g->path);memset(g,0,sizeof *g);
}
fpe_err fpe_guard_open(const wchar_t *path,int anchored,uint64_t device,int create,int root,fpe_compare_cb cb,void *user,fpe_path_guard *out)
{
    memset(out,0,sizeof *out);if(cb&&!cb(user))return ERROR_CANCELLED;
    if(fpe_wsl_path_kind(path))return fpe_wsl_guard(path,anchored,device,create,root,cb,user,out);
    wchar_t *nt=NULL;fpe_err err=fpe_path_nt(path,&nt);if(err)return err;
    if(create&&!CreateDirectoryW(nt,NULL)&&GetLastError()!=ERROR_ALREADY_EXISTS){err=GetLastError();fpe_free(nt);return err;}
    HANDLE h=CreateFileW(nt,FILE_TRAVERSE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);fpe_free(nt);
    if(h==INVALID_HANDLE_VALUE)return GetLastError();BY_HANDLE_FILE_INFORMATION info;
    if(!GetFileInformationByHandle(h,&info))err=GetLastError();
    else if(!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)err=ERROR_CANT_ACCESS_FILE;
    else if(device&&device!=(uint64_t)info.dwVolumeSerialNumber+1)err=ERROR_NOT_SAME_DEVICE;
    HANDLE barrier=INVALID_HANDLE_VALUE;
    if(!err){barrier=fpe_win_reopen_directory(h,FILE_TRAVERSE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ);if(barrier==INVALID_HANDLE_VALUE)err=GetLastError();}
    if(!err)err=guard_info(barrier,&info);
    if(!err){out->path=fpe_wcsdup(path);if(!out->path)err=ERROR_NOT_ENOUGH_MEMORY;else{out->directory=h;out->barrier=barrier;out->device=(uint64_t)info.dwVolumeSerialNumber+1;}}
    if(err){if(barrier!=INVALID_HANDLE_VALUE)CloseHandle(barrier);CloseHandle(h);}return err;
}
fpe_err fpe_guard_child(const fpe_path_guard *parent,const wchar_t *relative,int create,int root,fpe_compare_cb cb,void *user,fpe_path_guard *out)
{
    memset(out,0,sizeof *out);if(!fpe_relative_valid(relative,!parent->linux))return ERROR_INVALID_NAME;
    size_t a=wcslen(parent->path),b=wcslen(relative);if(a+b+2>32768)return ERROR_FILENAME_EXCED_RANGE;
    wchar_t *path=fpe_alloc((a+b+2)*sizeof *path);if(!path)return ERROR_NOT_ENOUGH_MEMORY;
    memcpy(path,parent->path,a*sizeof *path);path[a]=L'\\';memcpy(path+a+1,relative,(b+1)*sizeof *path);
    fpe_err err=fpe_guard_open(path,parent->linux,parent->device,create,root,cb,user,out);if(!err)out->ancestry=parent->ancestry;fpe_free(path);return err;
}
