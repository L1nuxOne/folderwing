/* Destination ancestors are fenced before mutation, then kept pinned through
 * the transfer. Elevation is confined to a disposable flush-only process. */
#include "internal.h"
#include "durablewire.h"
#include <shellapi.h>
#include <sddl.h>

typedef struct ancestry_item {HANDLE pin,barrier;BY_HANDLE_FILE_INFORMATION id;} ancestry_item;
struct fpe_win_ancestry {wchar_t *path;ancestry_item *items;unsigned count;};
typedef struct durable_broker {HANDLE pipe,process,job;uint32_t sequence;} durable_broker;
static int same_id(const BY_HANDLE_FILE_INFORMATION *a,const BY_HANDLE_FILE_INFORMATION *b)
{return a->dwVolumeSerialNumber==b->dwVolumeSerialNumber&&a->nFileIndexHigh==b->nFileIndexHigh&&a->nFileIndexLow==b->nFileIndexLow;}
static DWORD info(HANDLE h,BY_HANDLE_FILE_INFORMATION *i)
{
    if(!GetFileInformationByHandle(h,i))return GetLastError();
    return (i->dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DEVICE))==FILE_ATTRIBUTE_DIRECTORY?0:ERROR_CANT_ACCESS_FILE;
}
static DWORD io_wait(durable_broker *b,OVERLAPPED *over,DWORD size,fpe_compare_cb cb,void *user)
{
    uint64_t start=GetTickCount64();DWORD error=0,got=0;HANDLE waits[]={over->hEvent,b->process};
    for(;;){DWORD w=WaitForMultipleObjects(2,waits,FALSE,20);
        if(w==WAIT_OBJECT_0){if(!GetOverlappedResult(b->pipe,over,&got,FALSE))error=GetLastError();else if(got!=size)error=ERROR_INVALID_DATA;break;}
        if(w==WAIT_OBJECT_0+1){error=ERROR_BROKEN_PIPE;break;}
        if(w==WAIT_FAILED){error=GetLastError();break;}
        if(cb&&!cb(user)){error=ERROR_CANCELLED;break;}
        if(GetTickCount64()-start>=FPD_TIMEOUT){error=ERROR_TIMEOUT;break;}
    }
    if(error){CancelIoEx(b->pipe,over);GetOverlappedResult(b->pipe,over,&got,TRUE);}return error;
}
static DWORD packet(durable_broker *b,fpd_message *m,int write)
{
    OVERLAPPED over={.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL)};if(!over.hEvent)return GetLastError();DWORD got=0,error=0;
    BOOL ok=write?WriteFile(b->pipe,m,sizeof *m,&got,&over):ReadFile(b->pipe,m,sizeof *m,&got,&over);
    if(!ok){error=GetLastError();if(error==ERROR_IO_PENDING)error=io_wait(b,&over,sizeof *m,NULL,NULL);}
    else if(got!=sizeof *m)error=ERROR_INVALID_DATA;
    CloseHandle(over.hEvent);return error;
}
static void broker_close(durable_broker *b)
{
    if(b->pipe&&b->pipe!=INVALID_HANDLE_VALUE){CancelIoEx(b->pipe,NULL);CloseHandle(b->pipe);}
    if(b->job)CloseHandle(b->job);
    if(b->process){WaitForSingleObject(b->process,5000);CloseHandle(b->process);}memset(b,0,sizeof *b);
}
static DWORD broker_open(durable_broker *b,fpe_compare_cb cb,void *user)
{
    memset(b,0,sizeof *b);if(cb&&!cb(user))return ERROR_CANCELLED;
    BYTE random[16];typedef BOOLEAN (WINAPI *random_fn)(PVOID,ULONG);random_fn gen=(random_fn)GetProcAddress(GetModuleHandleW(L"advapi32.dll"),"SystemFunction036");
    if(!gen||!gen(random,sizeof random))return ERROR_GEN_FAILURE;
    wchar_t nonce[33],pipe_name[128],params[256];for(unsigned i=0;i<16;i++)swprintf(nonce+i*2,3,L"%02x",random[i]);
    swprintf(pipe_name,128,L"%ls%ls",FPD_PIPE_PREFIX,nonce);
    HANDLE token=NULL;BYTE user_info[512];DWORD size;LPWSTR sid=NULL;PSECURITY_DESCRIPTOR security=NULL;DWORD error=0;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return GetLastError();
    if(!GetTokenInformation(token,TokenUser,user_info,sizeof user_info,&size)||!ConvertSidToStringSidW(((TOKEN_USER*)user_info)->User.Sid,&sid))error=GetLastError();CloseHandle(token);
    if(error)return error;
    wchar_t acl[512];swprintf(acl,512,L"D:P(A;;GA;;;SY)(A;;GA;;;%ls)",sid);LocalFree(sid);
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl,SDDL_REVISION_1,&security,NULL))return GetLastError();
    SECURITY_ATTRIBUTES sa={sizeof sa,security,FALSE};
    b->pipe=CreateNamedPipeW(pipe_name,PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,sizeof(fpd_message),sizeof(fpd_message),0,&sa);
    LocalFree(security);if(b->pipe==INVALID_HANDLE_VALUE){error=GetLastError();goto done;}
    b->job=CreateJobObjectW(NULL,NULL);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!b->job||!SetInformationJobObject(b->job,JobObjectExtendedLimitInformation,&limits,sizeof limits)){error=GetLastError();goto done;}
    HMODULE module=NULL;wchar_t *exe=fpe_alloc(FPE_MAX_PATH*sizeof *exe);if(!exe){error=ERROR_NOT_ENOUGH_MEMORY;goto done;}
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)(uintptr_t)&fpe_win_ancestry_open,&module)){error=GetLastError();fpe_free(exe);goto done;}
    DWORD n=GetModuleFileNameW(module,exe,FPE_MAX_PATH);wchar_t *slash=n&&n<FPE_MAX_PATH?wcsrchr(exe,L'\\'):NULL;
    if(!slash||(size_t)(slash-exe)+15>=FPE_MAX_PATH){error=ERROR_FILENAME_EXCED_RANGE;fpe_free(exe);goto done;}
    wcscpy(slash+1,L"folderwing-durable.exe");
    if(GetFileAttributesW(exe)==INVALID_FILE_ATTRIBUTES){error=GetLastError();fpe_free(exe);goto done;}
    FILETIME born,exit,kernel,utime;if(!GetProcessTimes(GetCurrentProcess(),&born,&exit,&kernel,&utime)){error=GetLastError();fpe_free(exe);goto done;}
    swprintf(params,256,L"%lu %llx %llu %ls",(unsigned long)GetCurrentProcessId(),(unsigned long long)(((uint64_t)born.dwHighDateTime<<32)|born.dwLowDateTime),(unsigned long long)(uintptr_t)b->job,nonce);
    SHELLEXECUTEINFOW launch={.cbSize=sizeof launch,.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_NOASYNC|SEE_MASK_FLAG_NO_UI,.lpVerb=L"runas",.lpFile=exe,.lpParameters=params,.nShow=SW_HIDE};
    BOOL launched=ShellExecuteExW(&launch);error=launched?0:GetLastError();fpe_free(exe);if(error)goto done;
    b->process=launch.hProcess;if(!b->process){error=ERROR_INVALID_HANDLE;goto done;}
    OVERLAPPED over={.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL)};if(!over.hEvent){error=GetLastError();goto done;}
    BOOL connected=ConnectNamedPipe(b->pipe,&over);DWORD connect_error=connected?0:GetLastError();
    if(connect_error==ERROR_IO_PENDING)error=io_wait(b,&over,0,cb,user);
    else if(connect_error!=ERROR_PIPE_CONNECTED)error=connect_error;
    CloseHandle(over.hEvent);if(error)goto done;
    ULONG client=0;if(!GetNamedPipeClientProcessId(b->pipe,&client)||client!=GetProcessId(b->process)){error=ERROR_ACCESS_DENIED;goto done;}
    fpd_message hello={0};error=packet(b,&hello,0);
    if(!error&&(hello.magic!=FPD_MAGIC||hello.version!=FPD_VERSION||hello.sequence||hello.error||hello.handle||hello.volume||hello.high||hello.low||hello.reserved))error=ERROR_INVALID_DATA;
    if(!error&&cb&&!cb(user))error=ERROR_CANCELLED;
done:
    if(error)broker_close(b);return error;
}
static DWORD broker_flush(durable_broker *b,HANDLE directory,const BY_HANDLE_FILE_INFORMATION *id)
{
    fpd_message q={.magic=FPD_MAGIC,.version=FPD_VERSION,.sequence=++b->sequence,.handle=(uintptr_t)directory,
        .volume=id->dwVolumeSerialNumber,.high=id->nFileIndexHigh,.low=id->nFileIndexLow};
    if(q.sequence>FPD_LIMIT)return ERROR_BUFFER_OVERFLOW;
    DWORD error=packet(b,&q,1);if(error)return error;fpd_message r={0};error=packet(b,&r,0);if(error)return error;
    if(r.magic!=q.magic||r.version!=q.version||r.sequence!=q.sequence||r.handle||r.volume!=q.volume||r.high!=q.high||r.low!=q.low||r.reserved)return ERROR_INVALID_DATA;
    return r.error;
}
DWORD fpe_win_ancestry_exchange(fpe_win_ancestry *a,HANDLE directory,int restore)
{
    if(!a)return 0;BY_HANDLE_FILE_INFORMATION id;DWORD error=info(directory,&id);if(error)return error;
    for(unsigned i=0;i<a->count;i++)if(same_id(&a->items[i].id,&id)){
        ancestry_item *item=a->items+i;
        if(!restore){if(!item->barrier)return ERROR_INVALID_HANDLE;CloseHandle(item->barrier);item->barrier=NULL;}
        else{BY_HANDLE_FILE_INFORMATION after;HANDLE h=fpe_win_reopen_directory(item->pin,FILE_TRAVERSE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ);
            DWORD e=h==INVALID_HANDLE_VALUE?GetLastError():info(h,&after);if(h!=INVALID_HANDLE_VALUE){item->barrier=h;if(!e&&!same_id(&item->id,&after))e=ERROR_FILE_INVALID;}if(e&&!error)error=e;}
    }
    return error;
}
void fpe_win_ancestry_close(fpe_win_ancestry *a)
{
    if(!a)return;while(a->count){ancestry_item *i=a->items+--a->count;if(i->barrier)CloseHandle(i->barrier);if(i->pin&&i->pin!=INVALID_HANDLE_VALUE)CloseHandle(i->pin);}
    fpe_free(a->items);fpe_free(a->path);fpe_free(a);
}
const wchar_t *fpe_win_ancestry_path(fpe_win_ancestry *a){return a?a->path:NULL;}
HANDLE fpe_win_ancestry_root(fpe_win_ancestry *a){return a&&a->count?a->items[a->count-1].pin:INVALID_HANDLE_VALUE;}
DWORD fpe_win_ancestry_validate(fpe_win_ancestry *a,HANDLE directory)
{
    if(!a)return 0;BY_HANDLE_FILE_INFORMATION id,root;DWORD error=info(directory,&id);if(!error)error=info(fpe_win_ancestry_root(a),&root);
    if(!error&&(!same_id(&a->items[a->count-1].id,&id)||!same_id(&id,&root)))error=ERROR_FILE_INVALID;return error;
}
DWORD fpe_win_ancestry_open(const wchar_t *path,fpe_compare_cb cb,void *user,fpe_win_ancestry **out)
{
    *out=NULL;if(cb&&!cb(user))return ERROR_CANCELLED;
    wchar_t *nt=NULL,*canonical=NULL;DWORD error=fpe_path_nt(path,&nt);HANDLE endpoint=INVALID_HANDLE_VALUE;BY_HANDLE_FILE_INFORMATION original;durable_broker broker={0};
    int elevation_attempted=0;
    fpe_win_ancestry *a=fpe_zalloc(sizeof *a);if(!a){fpe_free(nt);return ERROR_NOT_ENOUGH_MEMORY;}
    if(!error){endpoint=CreateFileW(nt,FILE_TRAVERSE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);if(endpoint==INVALID_HANDLE_VALUE)error=GetLastError();}
    fpe_free(nt);if(!error)error=info(endpoint,&original);
    DWORD size=!error?GetFinalPathNameByHandleW(endpoint,NULL,0,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS):0;
    if(!error&&!size)error=GetLastError();
    if(!error&&!(canonical=fpe_alloc(((size_t)size+1)*sizeof *canonical)))error=ERROR_NOT_ENOUGH_MEMORY;
    if(!error){DWORD got=GetFinalPathNameByHandleW(endpoint,canonical,size+1,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);if(!got||got>size)error=got?ERROR_FILENAME_EXCED_RANGE:GetLastError();}
    if(!error){size_t n=wcslen(canonical),root=0;
        if(n>=7&&!wcsncmp(canonical,L"\\\\?\\",4)&&canonical[5]==L':'&&canonical[6]==L'\\')root=7;
        else if(n>=8&&!wcsncmp(canonical,L"\\\\?\\UNC\\",8)){const wchar_t *server=wcschr(canonical+8,L'\\'),*share=server?wcschr(server+1,L'\\'):NULL;if(server&&server[1])root=share?(size_t)(share-canonical)+1:n;}
        else if(n>11&&!wcsncmp(canonical,L"\\\\?\\Volume{",11)){const wchar_t *end=wcschr(canonical+4,L'\\');if(end)root=(size_t)(end-canonical)+1;}
        if(!root)error=ERROR_INVALID_NAME;
        a->items=fpe_zalloc((FPD_LIMIT+1)*sizeof *a->items);if(!a->items)error=ERROR_NOT_ENOUGH_MEMORY;
        for(size_t at=root;!error;){
            if(a->count>=FPD_LIMIT+1){error=ERROR_BUFFER_OVERFLOW;break;}
            wchar_t saved=canonical[at];canonical[at]=0;ancestry_item *item=a->items+a->count++;
            item->pin=CreateFileW(canonical,FILE_TRAVERSE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);canonical[at]=saved;
            error=item->pin==INVALID_HANDLE_VALUE?GetLastError():info(item->pin,&item->id);
            if(!error){item->barrier=fpe_win_reopen_directory(item->pin,FILE_TRAVERSE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ);if(item->barrier==INVALID_HANDLE_VALUE){error=GetLastError();item->barrier=NULL;}}
            if(error||at==n)break;size_t next=at;while(next<n&&canonical[next]==L'\\')next++;while(next<n&&canonical[next]!=L'\\')next++;if(next==at){error=ERROR_INVALID_NAME;break;}at=next;
        }
        if(!error&&!same_id(&original,&a->items[a->count-1].id))error=ERROR_FILE_INVALID;
    }
    if(endpoint!=INVALID_HANDLE_VALUE)CloseHandle(endpoint);
    for(unsigned n=a->count;!error&&n>1;n--){
        ancestry_item *item=a->items+n-2;if(cb&&!cb(user)){error=ERROR_CANCELLED;break;}
retry:
        error=fpe_win_ancestry_exchange(a,item->pin,0);if(error)break;
        BY_HANDLE_FILE_INFORMATION after;HANDLE write=fpe_win_reopen_directory(item->pin,GENERIC_WRITE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE);
        error=write==INVALID_HANDLE_VALUE?GetLastError():info(write,&after);
        if(!error&&!same_id(&item->id,&after))error=ERROR_FILE_INVALID;
        if(!error&&!FlushFileBuffers(write))error=GetLastError();if(write!=INVALID_HANDLE_VALUE)CloseHandle(write);
        if(error==ERROR_ACCESS_DENIED&&broker.process)error=broker_flush(&broker,item->pin,&item->id);
        /* On broken IPC stop the helper before restoring a read barrier: an
         * unacknowledged request may still own a writable directory handle. */
        if(error&&broker.process)broker_close(&broker);
        DWORD restore=fpe_win_ancestry_exchange(a,item->pin,1);if(restore)error=restore;
        if(error==ERROR_ACCESS_DENIED&&!restore&&!elevation_attempted){
            elevation_attempted=1;
            error=broker_open(&broker,cb,user);if(!error)goto retry;
        }
        if(!error&&cb&&!cb(user))error=ERROR_CANCELLED;
    }
    broker_close(&broker);
    if(!error){if(!wcsncmp(canonical,L"\\\\?\\Volume{",11)){a->path=fpe_wcsdup(canonical);if(!a->path)error=ERROR_NOT_ENOUGH_MEMORY;}
        else error=fpe_path_display(canonical,&a->path);}fpe_free(canonical);
    if(error)fpe_win_ancestry_close(a);else *out=a;return error;
}
