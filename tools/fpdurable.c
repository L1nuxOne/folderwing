/* Elevated one-preflight broker. Only duplicates captured ordinary directory
 * handles and flushes the same object; never opens caller-supplied paths. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <winternl.h>
#include <stdint.h>
#include <wchar.h>
#include "../src/durablewire.h"

static int same_user(HANDLE process)
{
    HANDLE tokens[2]={0};BYTE users[2][512];DWORD sizes[2],sessions[2],n;int ok=1;
    HANDLE processes[]={GetCurrentProcess(),process};
    for(int i=0;i<2;i++){
        if(!OpenProcessToken(processes[i],TOKEN_QUERY,&tokens[i])||
           !GetTokenInformation(tokens[i],TokenUser,users[i],sizeof users[i],&sizes[i])||
           !GetTokenInformation(tokens[i],TokenSessionId,&sessions[i],sizeof sessions[i],&n)){ok=0;break;}
    }
    if(ok)ok=sessions[0]==sessions[1]&&EqualSid(((TOKEN_USER*)users[0])->User.Sid,((TOKEN_USER*)users[1])->User.Sid);
    for(int i=0;i<2;i++)if(tokens[i])CloseHandle(tokens[i]);return ok;
}
static DWORD identity(HANDLE h,fpd_message *m)
{
    BY_HANDLE_FILE_INFORMATION i;
    if(GetFileType(h)!=FILE_TYPE_DISK)return ERROR_NOT_SUPPORTED;
    if(!GetFileInformationByHandle(h,&i))return GetLastError();
    if((i.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DEVICE))!=FILE_ATTRIBUTE_DIRECTORY)return ERROR_CANT_ACCESS_FILE;
    m->volume=i.dwVolumeSerialNumber;m->high=i.nFileIndexHigh;m->low=i.nFileIndexLow;return 0;
}
static int same(const fpd_message *a,const fpd_message *b)
{return a->volume==b->volume&&a->high==b->high&&a->low==b->low;}
static HANDLE reopen(HANDLE pin,DWORD *error)
{
    typedef NTSTATUS (NTAPI *create_fn)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,PLARGE_INTEGER,ULONG,ULONG,ULONG,ULONG,PVOID,ULONG);
    typedef ULONG (WINAPI *error_fn)(NTSTATUS);
    HMODULE dll=GetModuleHandleW(L"ntdll.dll");create_fn create=(create_fn)GetProcAddress(dll,"NtCreateFile");error_fn convert=(error_fn)GetProcAddress(dll,"RtlNtStatusToDosError");
    if(!create||!convert){*error=ERROR_NOT_SUPPORTED;return INVALID_HANDLE_VALUE;}
    UNICODE_STRING name={.Buffer=L""};OBJECT_ATTRIBUTES object={.Length=sizeof object,.RootDirectory=pin,.ObjectName=&name,.Attributes=0x40};IO_STATUS_BLOCK io;HANDLE h=NULL;
    NTSTATUS status=create(&h,GENERIC_WRITE|FILE_READ_ATTRIBUTES|SYNCHRONIZE,&object,&io,NULL,0,FILE_SHARE_READ|FILE_SHARE_WRITE,1,0x204021,NULL,0);
    if(status<0){*error=convert(status);return INVALID_HANDLE_VALUE;}return h;
}
static void enable_restore(void)
{
    HANDLE token;TOKEN_PRIVILEGES p={.PrivilegeCount=1};
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&token))return;
    if(LookupPrivilegeValueW(NULL,SE_RESTORE_NAME,&p.Privileges[0].Luid)){
        p.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;AdjustTokenPrivileges(token,FALSE,&p,0,NULL,NULL);
    }CloseHandle(token);
}
static DWORD flush(HANDLE parent,const fpd_message *q)
{
    HANDLE pin=NULL,write=INVALID_HANDLE_VALUE;fpd_message before={0},after={0};DWORD error=0;
    if(!q->handle||q->handle>UINTPTR_MAX)return ERROR_INVALID_HANDLE;
    if(!DuplicateHandle(parent,(HANDLE)(uintptr_t)q->handle,GetCurrentProcess(),&pin,0,FALSE,DUPLICATE_SAME_ACCESS))return GetLastError();
    error=identity(pin,&before);
    if(!error&&!same(q,&before))error=ERROR_FILE_INVALID;
    /* Reject network redirectors and non-volume objects before a privileged
     * open. The pathname result is inspected only, never reopened. */
    wchar_t *volume=NULL;
    if(!error){DWORD n=GetFinalPathNameByHandleW(pin,NULL,0,VOLUME_NAME_GUID|FILE_NAME_OPENED);
        if(!n)error=GetLastError();else if(n>32768)error=ERROR_FILENAME_EXCED_RANGE;
        else if(!(volume=HeapAlloc(GetProcessHeap(),0,((size_t)n+1)*sizeof *volume)))error=ERROR_NOT_ENOUGH_MEMORY;
        else{DWORD got=GetFinalPathNameByHandleW(pin,volume,n+1,VOLUME_NAME_GUID|FILE_NAME_OPENED);
            if(!got||got>n)error=got?ERROR_FILENAME_EXCED_RANGE:GetLastError();else if(wcsncmp(volume,L"\\\\?\\Volume{",11))error=ERROR_NOT_SUPPORTED;}}
    if(volume)HeapFree(GetProcessHeap(),0,volume);
    if(!error){write=reopen(pin,&error);if(error==ERROR_ACCESS_DENIED){enable_restore();error=0;write=reopen(pin,&error);}}
    if(!error)error=identity(write,&after);
    if(!error&&!same(&before,&after))error=ERROR_FILE_INVALID;
    if(!error&&!FlushFileBuffers(write))error=GetLastError();
    if(write!=INVALID_HANDLE_VALUE)CloseHandle(write);
    CloseHandle(pin);return error;
}
static int number(const wchar_t *s,uint64_t *out,int base)
{
    if(!s||!*s||*s==L'-'||*s==L'+')return 0;wchar_t *end;*out=wcstoull(s,&end,base);return !*end;
}
int wmain(int argc,wchar_t **argv)
{
    uint64_t pid,creation,job_value;wchar_t pipe_name[128];
    if(argc!=5||!number(argv[1],&pid,10)||!pid||pid>MAXDWORD||!number(argv[2],&creation,16)||
       !number(argv[3],&job_value,10)||!job_value||job_value>UINTPTR_MAX||wcslen(argv[4])!=32)return 2;
    for(unsigned i=0;i<32;i++)if(!wcschr(L"0123456789abcdef",argv[4][i]))return 2;
    _snwprintf(pipe_name,128,L"%ls%ls",FPD_PIPE_PREFIX,argv[4]);
    HANDLE token=NULL;TOKEN_ELEVATION elevation;DWORD n;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return 3;
    int elevated=GetTokenInformation(token,TokenElevation,&elevation,sizeof elevation,&n)&&elevation.TokenIsElevated;CloseHandle(token);if(!elevated)return 3;
    HANDLE pipe=INVALID_HANDLE_VALUE,parent=NULL,job=NULL;int result=4;
    uint64_t started=GetTickCount64();
    do{pipe=CreateFileW(pipe_name,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
        if(pipe!=INVALID_HANDLE_VALUE)break;DWORD e=GetLastError();if(e!=ERROR_PIPE_BUSY&&e!=ERROR_FILE_NOT_FOUND)goto done;Sleep(10);
    }while(GetTickCount64()-started<5000);
    if(pipe==INVALID_HANDLE_VALUE)goto done;
    ULONG server=0;if(!GetNamedPipeServerProcessId(pipe,&server)||server!=pid)goto done;
    parent=OpenProcess(PROCESS_DUP_HANDLE|PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,(DWORD)pid);if(!parent)goto done;
    FILETIME born,exit,kernel,user;
    if(!GetProcessTimes(parent,&born,&exit,&kernel,&user)||(((uint64_t)born.dwHighDateTime<<32)|born.dwLowDateTime)!=creation||!same_user(parent))goto done;
    if(!DuplicateHandle(parent,(HANDLE)(uintptr_t)job_value,GetCurrentProcess(),&job,0,FALSE,DUPLICATE_SAME_ACCESS)||!AssignProcessToJobObject(job,GetCurrentProcess()))goto done;
    /* The client must not retain a kill-on-close job reference. */
    CloseHandle(job);job=NULL;
    DWORD mode=PIPE_READMODE_MESSAGE;if(!SetNamedPipeHandleState(pipe,&mode,NULL,NULL))goto done;
    fpd_message hello={.magic=FPD_MAGIC,.version=FPD_VERSION};
    if(!WriteFile(pipe,&hello,sizeof hello,&n,NULL)||n!=sizeof hello)goto done;
    for(uint32_t sequence=1;sequence<=FPD_LIMIT;sequence++){
        fpd_message q={0};if(!ReadFile(pipe,&q,sizeof q,&n,NULL)||n!=sizeof q)break;
        if(q.magic!=FPD_MAGIC||q.version!=FPD_VERSION||q.sequence!=sequence||q.error||q.reserved)break;
        DWORD error=flush(parent,&q);q.error=error;q.handle=0;
        /* Every writable/duplicated directory handle is closed before ACK. */
        if(!WriteFile(pipe,&q,sizeof q,&n,NULL)||n!=sizeof q||error)break;
    }
    result=0;
done:
    if(job)CloseHandle(job);if(parent)CloseHandle(parent);if(pipe!=INVALID_HANDLE_VALUE)CloseHandle(pipe);return result;
}
