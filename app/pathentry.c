#include "../src/namecodec.h"
/* Literal Linux path entry. Only fixed executable arguments reach wsl.exe;
 * filenames travel through a bounded binary pipe, never a shell command. */
#include "pathentry.h"
#include "fpe.h"
#include "termutil.h"
#include "../src/wslproto.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
int pathentry_kind(const wchar_t *s)
{
    if(!s || !*s)return 0;
    if(*s==L'/')return s[1]==L'/'?0:1;
    if(*s==L'~')return !s[1]||s[1]==L'/'?1:-1;
    return 0;
}
int pathentry_display(const wchar_t *s,wchar_t *d,size_t dc,wchar_t *p,size_t pc)
{
    if(!s)return 0;
    size_t start=!_wcsnicmp(s,L"\\\\wsl.localhost\\",16)?16:!_wcsnicmp(s,L"\\\\wsl$\\",7)?7:0;
    if(!start||pc<2)return 0;
    const wchar_t *tail=wcschr(s+start,L'\\');size_t n=tail?(size_t)(tail-s-start):wcslen(s+start);
    if(!n||n>=dc)return 0;
    for(size_t i=0;i<n;++i) {wchar_t c=s[start+i];if(!((c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||(c>=L'0'&&c<=L'9')||wcschr(L"._-",c)))return 0;}
    memcpy(d,s+start,n*sizeof(wchar_t));d[n]=0;
    p[0]=L'/';tail=tail?tail+1:s+start+n;
    return fpe_lx_literal(tail,p+1,pc-1,1);
}
int pathentry_bytes(const wchar_t *s,char *out,size_t cap,uint32_t *length)
{
    size_t k=0;
    for(size_t i=0;s[i];++i) {
        uint32_t c=(uint16_t)s[i];unsigned char b[4];size_t n;
        if(c>=0xd800&&c<=0xdbff) {uint32_t low=(uint16_t)s[++i];if(low<0xdc00||low>0xdfff)return 0;c=0x10000+((c-0xd800)<<10)+low-0xdc00;}
        else if(c>=0xdc80&&c<=0xdcff) {b[0]=(unsigned char)(c-0xdc00);n=1;goto put;}
        else if(c>=0xdc00&&c<=0xdfff)return 0;
        if(c<128){b[0]=(unsigned char)c;n=1;}
        else if(c<2048){b[0]=0xc0|(c>>6);b[1]=0x80|(c&63);n=2;}
        else if(c<65536){b[0]=0xe0|(c>>12);b[1]=0x80|((c>>6)&63);b[2]=0x80|(c&63);n=3;}
        else {b[0]=0xf0|(c>>18);b[1]=0x80|((c>>12)&63);b[2]=0x80|((c>>6)&63);b[3]=0x80|(c&63);n=4;}
put:    if(k+n>cap)return 0;memcpy(out+k,b,n);k+=n;
    }
    *length=(uint32_t)k;return 1;
}
wchar_t *pathentry_unc(const char *d,uint32_t dn,const char *p,uint32_t pn)
{
    if(!dn||dn>=64||!pn||pn>PATHENTRY_BYTES||p[0]!='/')return NULL;
    for(uint32_t i=0;i<dn;++i) if(!((d[i]>='a'&&d[i]<='z')||(d[i]>='A'&&d[i]<='Z')||(d[i]>='0'&&d[i]<='9')||d[i]=='.'||d[i]=='_'||d[i]=='-'))return NULL;
    wchar_t *out=malloc((16+dn+pn+1)*sizeof(wchar_t));if(!out)return NULL;
    wcscpy(out,L"\\\\wsl.localhost\\");size_t k=16;
    for(uint32_t i=0;i<dn;++i)out[k++]=(unsigned char)d[i];
    size_t units=0;
    if(!fpe_lx_from_bytes((const uint8_t*)p,pn,out+k,pn+1,1,1,&units)){free(out);return NULL;}
    k+=units;
    out[k]=0;return out;
}
static DWORD linux_error(int err)
{
    switch(err) {case 0:return 0;case 1:case 13:return ERROR_ACCESS_DENIED;case 2:return ERROR_PATH_NOT_FOUND;case 20:return ERROR_DIRECTORY;
    case 12:return ERROR_NOT_ENOUGH_MEMORY;case 22:return ERROR_INVALID_NAME;case 36:return ERROR_FILENAME_EXCED_RANGE;case 40:return ERROR_CANT_RESOLVE_FILENAME;
    default:return ERROR_GEN_FAILURE;}
}
static DWORD pipe_read(HANDLE pipe,HANDLE cancel,uint64_t until,void *data,DWORD size)
{
    while(size) {
        if(cancel && WaitForSingleObject(cancel,0)==WAIT_OBJECT_0)return ERROR_CANCELLED;
        if(GetTickCount64()>=until)return ERROR_TIMEOUT;
        DWORD available=0,got=0;
        if(!PeekNamedPipe(pipe,NULL,0,NULL,&available,NULL))return ERROR_BAD_ENVIRONMENT;
        if(!available){if(cancel)WaitForSingleObject(cancel,10);else Sleep(10);continue;}
        DWORD want=available<size?available:size;
        if(!ReadFile(pipe,data,want,&got,NULL)||!got)return ERROR_BAD_ENVIRONMENT;
        data=(char*)data+got;size-=got;
    }
    return 0;
}
DWORD pathentry_resolve(const wchar_t *base,const wchar_t *typed,int root,HANDLE cancel,wchar_t **out)
{
    *out=NULL;
    if(pathentry_kind(typed)!=1)return ERROR_INVALID_NAME;
    if(wcslen(typed)>=PATHENTRY_MAX)return ERROR_FILENAME_EXCED_RANGE;
    if(cancel && WaitForSingleObject(cancel,0)==WAIT_OBJECT_0)return ERROR_CANCELLED;
    char raw[PATHENTRY_BYTES];uint32_t bytes=0;
    if(!pathentry_bytes(typed,raw,sizeof raw,&bytes))return ERROR_INVALID_NAME;
    wchar_t distro[64]=L"",linux[PATHENTRY_BYTES+1];
    if(base) {
        int native=term_wsl_split(base,distro,64,linux,PATHENTRY_BYTES+1);
        if(!native&&fpe_wsl_requires_native(base))return ERROR_INVALID_NAME;
        if(!native && (!_wcsnicmp(base,L"\\\\wsl.localhost\\",16)||!_wcsnicmp(base,L"\\\\wsl$\\",7)))return ERROR_NOT_SUPPORTED;
    }
    wchar_t dir[MAX_PATH],wsl[MAX_PATH],cmd[3*MAX_PATH+128];
    DWORD n=GetModuleFileNameW(NULL,dir,MAX_PATH);wchar_t *slash=n&&n<MAX_PATH?wcsrchr(dir,L'\\'):NULL;
    if(!slash)return ERROR_NOT_SUPPORTED;*slash=0;
    n=GetSystemDirectoryW(wsl,MAX_PATH-10);if(!n||n>=MAX_PATH-10)return ERROR_NOT_SUPPORTED;wcscat(wsl,L"\\wsl.exe");
    _snwprintf(cmd,sizeof cmd/sizeof *cmd,L"\"%ls\"%ls%ls%ls --cd \"%ls\" --exec ./folderwing-wsl --resolve-path",wsl,
        distro[0]?L" --distribution ":L"",distro,root?L" --user root":L"",dir);cmd[sizeof cmd/sizeof *cmd-1]=0;
    SECURITY_ATTRIBUTES sa={sizeof sa,NULL,TRUE};
    HANDLE in_r=NULL,in_w=NULL,out_r=NULL,out_w=NULL,nul=NULL,job=NULL;
    PROCESS_INFORMATION pi={0};LPPROC_THREAD_ATTRIBUTE_LIST attrs=NULL;int initialized=0;DWORD error=ERROR_NOT_SUPPORTED;
    if(!CreatePipe(&in_r,&in_w,&sa,65536)||!CreatePipe(&out_r,&out_w,&sa,65536))goto cleanup;
    SetHandleInformation(in_w,HANDLE_FLAG_INHERIT,0);SetHandleInformation(out_r,HANDLE_FLAG_INHERIT,0);
    nul=CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,NULL);
    if(nul==INVALID_HANDLE_VALUE){nul=NULL;goto cleanup;}
    SIZE_T need=0;InitializeProcThreadAttributeList(NULL,1,0,&need);attrs=malloc(need);
    if(!attrs||!InitializeProcThreadAttributeList(attrs,1,0,&need))goto cleanup;initialized=1;
    HANDLE handles[3]={in_r,out_w,nul};
    if(!UpdateProcThreadAttribute(attrs,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof handles,NULL,NULL))goto cleanup;
    STARTUPINFOEXW si={{sizeof si}};si.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput=in_r;si.StartupInfo.hStdOutput=out_w;si.StartupInfo.hStdError=nul;si.lpAttributeList=attrs;
    job=CreateJobObjectW(NULL,NULL);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit={0};limit.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!job||!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limit,sizeof limit))goto cleanup;
    if(!CreateProcessW(wsl,cmd,NULL,NULL,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,NULL,NULL,&si.StartupInfo,&pi))goto cleanup;
    if(!AssignProcessToJobObject(job,pi.hProcess)){TerminateProcess(pi.hProcess,1);goto cleanup;}
    ResumeThread(pi.hThread);
    CloseHandle(in_r);in_r=NULL;CloseHandle(out_w);out_w=NULL;
    wslp_head head={WSLP_RESOLVE,bytes,0};DWORD got;
    if(!WriteFile(in_w,&head,sizeof head,&got,NULL)||got!=sizeof head||!WriteFile(in_w,raw,bytes,&got,NULL)||got!=bytes){error=ERROR_BROKEN_PIPE;goto cleanup;}
    CloseHandle(in_w);in_w=NULL;
    uint64_t until=GetTickCount64()+30000;
    error=pipe_read(out_r,cancel,until,&head,sizeof head);if(error)goto cleanup;
    if(head.type!=WSLP_RESOLVED || head.size<sizeof(wslp_resolved) || head.size>sizeof(wslp_resolved)+64+PATHENTRY_BYTES){error=ERROR_BAD_ENVIRONMENT;goto cleanup;}
    wslp_resolved reply;
    error=pipe_read(out_r,cancel,until,&reply,sizeof reply);if(error)goto cleanup;
    if(reply.distro_len>=64 || reply.path_len>PATHENTRY_BYTES || head.size!=sizeof reply+reply.distro_len+reply.path_len){error=ERROR_INVALID_DATA;goto cleanup;}
    error=linux_error(reply.err);if(error)goto cleanup;
    char result[64+PATHENTRY_BYTES];error=pipe_read(out_r,cancel,until,result,reply.distro_len+reply.path_len);if(error)goto cleanup;
    *out=pathentry_unc(result,reply.distro_len,result+reply.distro_len,reply.path_len);
    error=*out?0:ERROR_INVALID_DATA;
cleanup:
    if(initialized)DeleteProcThreadAttributeList(attrs);free(attrs);
    if(pi.hProcess && error!=ERROR_CANCELLED && error!=ERROR_TIMEOUT) {
        HANDLE wait[2]={pi.hProcess,cancel};WaitForMultipleObjects(cancel?2:1,wait,FALSE,250);
    }
    if(job)CloseHandle(job);
    HANDLE handles_out[]={in_r,in_w,out_r,out_w,nul,pi.hThread,pi.hProcess};
    for(size_t i=0;i<sizeof handles_out/sizeof *handles_out;++i)if(handles_out[i])CloseHandle(handles_out[i]);
    return error;
}
struct pathentry_worker {
    HWND target;UINT message;HANDLE thread,wake,cancel;
    CRITICAL_SECTION lock;int stopped,root;
    uint64_t sequence;wchar_t *base,*typed;
};
void pathentry_result_free(pathentry_result *r){if(r){free(r->path);free(r->typed);free(r);}}
static void worker_free(pathentry_worker *w)
{
    if(w->thread)CloseHandle(w->thread);if(w->wake)CloseHandle(w->wake);if(w->cancel)CloseHandle(w->cancel);
    free(w->base);free(w->typed);DeleteCriticalSection(&w->lock);free(w);
}
static DWORD WINAPI worker_main(void *arg)
{
    pathentry_worker *w=arg;
    for(;;) {
        WaitForSingleObject(w->wake,INFINITE);
        EnterCriticalSection(&w->lock);
        if(w->stopped){LeaveCriticalSection(&w->lock);break;}
        wchar_t *base=w->base,*typed=w->typed;int root=w->root;uint64_t sequence=w->sequence;
        w->base=w->typed=NULL;ResetEvent(w->cancel);LeaveCriticalSection(&w->lock);
        if(!typed){free(base);continue;}
        pathentry_result *r=calloc(1,sizeof *r);
        if(r){r->sequence=sequence;r->typed=typed;r->error=pathentry_resolve(base,typed,root,w->cancel,&r->path);
            EnterCriticalSection(&w->lock);
            if(w->stopped || !PostMessageW(w->target,w->message,0,(LPARAM)r))pathentry_result_free(r);
            LeaveCriticalSection(&w->lock);
        }else {free(typed);EnterCriticalSection(&w->lock);if(!w->stopped)PostMessageW(w->target,w->message,(WPARAM)sequence,0);LeaveCriticalSection(&w->lock);}
        free(base);
    }
    worker_free(w);return 0;
}
pathentry_worker *pathentry_start(HWND target,UINT message)
{
    pathentry_worker *w=calloc(1,sizeof *w);if(!w)return NULL;
    InitializeCriticalSection(&w->lock);w->target=target;w->message=message;
    w->wake=CreateEventW(NULL,FALSE,FALSE,NULL);w->cancel=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(!w->wake||!w->cancel||!(w->thread=CreateThread(NULL,0,worker_main,w,0,NULL))){pathentry_stop(w);return NULL;}
    return w;
}
int pathentry_submit(pathentry_worker *w,const wchar_t *base,const wchar_t *typed,int root,uint64_t sequence)
{
    wchar_t *b=base?_wcsdup(base):NULL,*t=_wcsdup(typed);
    if((base&&!b)||!t){free(b);free(t);return 0;}
    EnterCriticalSection(&w->lock);free(w->base);free(w->typed);w->base=b;w->typed=t;w->root=root;w->sequence=sequence;
    SetEvent(w->cancel);SetEvent(w->wake);LeaveCriticalSection(&w->lock);return 1;
}
void pathentry_cancel(pathentry_worker *w)
{
    if(!w)return;EnterCriticalSection(&w->lock);free(w->base);free(w->typed);w->base=w->typed=NULL;SetEvent(w->cancel);LeaveCriticalSection(&w->lock);
}
void pathentry_stop(pathentry_worker *w)
{
    if(!w)return;EnterCriticalSection(&w->lock);int has_thread=w->thread!=NULL;w->stopped=1;SetEvent(w->cancel);SetEvent(w->wake);LeaveCriticalSection(&w->lock);
    /* The worker releases its own state. Posting is serialized with stop, so
     * destroying/reusing the target window cannot receive a late result. */
    if(!has_thread)worker_free(w);
}
