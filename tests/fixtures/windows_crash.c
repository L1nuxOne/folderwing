/* Uncatchable process-crash fixture, never part of a shipped image.
 * Compile twice: native jobs, and -DFP_CRASH_STREAM for the streamed writer.
 * Test controller supplies private files. No product environment hooks. */
#include "../../src/internal.h"
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
static const wchar_t *phase,*source,*destination;
static unsigned file_flushes;
static void crash(void){TerminateProcess(GetCurrentProcess(),86);ExitProcess(99);}
static int is_phase(const wchar_t *name){return !wcscmp(phase,name);}
static BOOL WINAPI crash_flush(HANDLE file)
{
    BY_HANDLE_FILE_INFORMATION info;
    if(!GetFileInformationByHandle(file,&info))ExitProcess(91);
    if(!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)){
        file_flushes++;
        if((is_phase(L"prepublish")&&file_flushes==1)||
           (is_phase(L"published")&&file_flushes==2))crash();
    }else if(is_phase(L"removed")&&GetFileAttributesW(source)==INVALID_FILE_ATTRIBUTES)crash();
    return FlushFileBuffers(file);
}

#ifndef FP_CRASH_STREAM
static unsigned renames;
static BOOL WINAPI crash_info(HANDLE file,FILE_INFO_BY_HANDLE_CLASS kind,LPVOID info,DWORD bytes)
{
    if(kind==FileDispositionInfo&&is_phase(L"before-remove")){
        wchar_t path[4096];DWORD size=GetFinalPathNameByHandleW(file,path,4096,0);
        if(size&&size<4096&&!_wcsicmp(!wcsncmp(path,L"\\\\?\\",4)?path+4:path,source))crash();
    }
    return SetFileInformationByHandle(file,kind,info,bytes);
}
typedef NTSTATUS (NTAPI *set_fn)(HANDLE,PIO_STATUS_BLOCK,PVOID,ULONG,FILE_INFORMATION_CLASS);
static set_fn real_set;
static NTSTATUS NTAPI crash_set(HANDLE file,PIO_STATUS_BLOCK io,PVOID data,ULONG bytes,FILE_INFORMATION_CLASS kind)
{
    NTSTATUS status=real_set(file,io,data,bytes,kind);
    if(status>=0&&(kind==10||kind==65)&&++renames==16&&is_phase(L"batch-prefix"))crash();
    return status;
}
static FARPROC WINAPI crash_lookup(HMODULE module,LPCSTR name)
{
    FARPROC fn=GetProcAddress(module,name);
    if((uintptr_t)name>65535&&!strcmp(name,"NtSetInformationFile")){
        union {FARPROC generic;set_fn typed;} cast;cast.generic=fn;real_set=cast.typed;cast.typed=crash_set;return cast.generic;
    }
    return fn;
}
#define GetProcAddress crash_lookup
#define FlushFileBuffers crash_flush
#define SetFileInformationByHandle crash_info
#include "../../src/jobs.c"
#undef SetFileInformationByHandle
#undef FlushFileBuffers
#undef GetProcAddress
#include "../jobwait.h"
static HANDLE done;
static void on_progress(void *user,const fpe_job_progress *p)
{
    (void)user;
    if(p->state>=FPE_JOB_DONE){
        fwprintf(stderr,L"MISSED boundary=%ls state=%d error=%u errors=%u current=%ls flushes=%u renames=%u\n",
            phase,p->state,(unsigned)p->last_error,p->errors,p->current?p->current:L"",file_flushes,renames);
        fflush(stderr);SetEvent(done);
    }
}
int wmain(int argc,wchar_t **argv)
{
    if(argc!=5)return 2;phase=argv[1];source=argv[2];destination=argv[3];
    fpe_engine *engine=fpe_engine_create(NULL);if(!engine)return 92;
    done=test_callback_event(TRUE);
    uint64_t id=fpe_job_submit_ex(engine,!wcscmp(argv[4],L"move")?FPE_OP_MOVE:FPE_OP_COPY,&source,1,destination,FPE_CONFLICT_OVERWRITE,on_progress,NULL,NULL);
    if(test_wait_job(engine,id,done,30000)!=WAIT_OBJECT_0)return 94;
    CloseHandle(done);fpe_engine_destroy(engine);
    return 95; /* A normal completion means the requested boundary was missed. */
}
#else
#define FlushFileBuffers crash_flush
#include "../../src/wsl.c"
#undef FlushFileBuffers
int wmain(int argc,wchar_t **argv)
{
    if(argc!=4)return 2;phase=argv[1];source=argv[2];destination=argv[3];
    windows_file w={0};static const uint8_t suffix[]="/selected";
    wslp_file file={.path_bytes=sizeof suffix-1,.replace=1};
    windows_begin(&w,&file,suffix,destination,1);
    if(w.err){fprintf(stderr,"windows_begin error=%u\n",(unsigned)w.err);return 92;}
    DWORD wrote;const char payload[]="complete selected bytes";
    DWORD size=is_phase(L"partial")?(sizeof payload-1)/2:sizeof payload-1;
    if(!WriteFile(w.file,payload,size,&wrote,NULL)||wrote!=size)return 93;
    if(is_phase(L"partial"))crash();
    fpe_err error=windows_end(&w,0,NULL);
    if(is_phase(L"lost-ack")&&!error)crash();
    fprintf(stderr,"MISSED streamed boundary error=%u flushes=%u\n",(unsigned)error,file_flushes);
    return 95;
}
#endif
