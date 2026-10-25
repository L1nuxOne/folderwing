/* Primary: engine integration. Owner: Windows destination ancestry durability.
 * Fault hooks exist only in this translation unit, never in release images. */
#include "../src/internal.h"
#include <shellapi.h>
#include <stdio.h>
static unsigned passed,failed,launches,ancestry_calls;
static int deny_launch,wrong_peer,missing_helper,restore_failure,block_restore,cancel_flush,wrong_birth;
static int force_broker;
static DWORD flush_error;static fpe_engine *engine;static uint64_t current_job;
#define CHECK(x) do{if(x)passed++;else{failed++;printf("FAIL %d %s err=%lu\n",__LINE__,#x,(unsigned long)GetLastError());}}while(0)
static BOOL WINAPI observed_launch(SHELLEXECUTEINFOW *s)
{launches++;if(deny_launch){SetLastError(ERROR_CANCELLED);return FALSE;}
 if(wrong_birth){unsigned long pid;unsigned long long birth,job;wchar_t nonce[33],params[256];
    if(swscanf(s->lpParameters,L"%lu %llx %llu %32ls",&pid,&birth,&job,nonce)!=4)return FALSE;
    swprintf(params,256,L"%lu %llx %llu %ls",pid,birth+1,job,nonce);SHELLEXECUTEINFOW changed=*s;changed.lpParameters=params;BOOL ok=ShellExecuteExW(&changed);s->hProcess=changed.hProcess;return ok;}
 return ShellExecuteExW(s);}
static BOOL WINAPI observed_peer(HANDLE p,PULONG pid)
{BOOL ok=GetNamedPipeClientProcessId(p,pid);if(ok&&wrong_peer)(*pid)++;return ok;}
static DWORD WINAPI observed_attributes(LPCWSTR path)
{if(missing_helper&&wcsstr(path,L"folderwing-durable.exe")){SetLastError(ERROR_FILE_NOT_FOUND);return INVALID_FILE_ATTRIBUTES;}return GetFileAttributesW(path);}
static HANDLE observed_reopen(HANDLE directory,DWORD access,DWORD share)
{if(block_restore&&!(access&GENERIC_WRITE)){SetLastError(ERROR_ACCESS_DENIED);return INVALID_HANDLE_VALUE;}return fpe_win_reopen_directory(directory,access,share);}
static BOOL WINAPI observed_flush(HANDLE h)
{
    if(restore_failure)block_restore=1;
    if(cancel_flush&&engine)fpe_job_cancel(engine,current_job);
    if(flush_error){SetLastError(flush_error);return FALSE;}
    /* Broker fault cases must reach the broker even on hosts whose ordinary
     * token can flush every ancestor directly. No ambient ACL assumption. */
    if(force_broker){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    return FlushFileBuffers(h);
}
#define ShellExecuteExW observed_launch
#define GetNamedPipeClientProcessId observed_peer
#define GetFileAttributesW observed_attributes
#define fpe_win_reopen_directory observed_reopen
#define FlushFileBuffers observed_flush
#include "../src/durable.c"
#undef ShellExecuteExW
#undef GetNamedPipeClientProcessId
#undef GetFileAttributesW
#undef fpe_win_reopen_directory
#undef FlushFileBuffers
static DWORD observed_ancestry(const wchar_t *path,fpe_compare_cb cb,void *user,fpe_win_ancestry **out)
{ancestry_calls++;return fpe_win_ancestry_open(path,cb,user,out);}
#define fpe_win_ancestry_open observed_ancestry
#include "../src/jobs.c"
#undef fpe_win_ancestry_open
#include "jobwait.h"
static const wchar_t *winroot,*lxroot,*crossroot;static unsigned serial;
static struct {HANDLE done;fpe_job_progress final;} J;
static void progress_case(void *user,const fpe_job_progress *p)
{current_job=p->job;if(p->state>=FPE_JOB_DONE){J.final=*p;J.final.current=NULL;SetEvent(J.done);}}
static void put(const wchar_t *path,const char *data)
{HANDLE h=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);CHECK(h!=INVALID_HANDLE_VALUE);if(h!=INVALID_HANDLE_VALUE){DWORD n;CHECK(WriteFile(h,data,(DWORD)strlen(data),&n,NULL)&&n==strlen(data));CloseHandle(h);}}
static int contents(const wchar_t *path,const char *data)
{HANDLE h=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);if(h==INVALID_HANDLE_VALUE)return 0;char bytes[100];DWORD n;int ok=ReadFile(h,bytes,sizeof bytes,&n,NULL)&&n==strlen(data)&&!memcmp(bytes,data,n);CloseHandle(h);return ok;}
static void clear(void)
{deny_launch=wrong_peer=missing_helper=restore_failure=block_restore=cancel_flush=wrong_birth=force_broker=0;flush_error=0;launches=ancestry_calls=0;}
static void exercise(int kind,DWORD expected)
{
    wchar_t source[4096],dest[4096],a[4096],target[4096];unsigned n=++serial;
    const wchar_t *source_base=kind>=3?lxroot:winroot,*dest_base=kind==2?crossroot:winroot;
    _snwprintf(source,4096,L"%ls\\source-%u",source_base,n);_snwprintf(dest,4096,L"%ls\\dest-%u",dest_base,n);
    CHECK(CreateDirectoryW(source,NULL));CHECK(CreateDirectoryW(dest,NULL));
    _snwprintf(a,4096,L"%ls\\item",source);_snwprintf(target,4096,L"%ls\\item",dest);put(a,"new source");put(target,"old target");
    memset(&J,0,sizeof J);J.done=test_callback_event(TRUE);const wchar_t *one[]={a},*relative[]={L"item"};
    int selected=kind==4;fpe_op op=kind==0||selected?FPE_OP_COPY:FPE_OP_MOVE;
    uint64_t id=selected?fpe_job_submit_tree(engine,source,relative,1,dest,FPE_CONFLICT_OVERWRITE,progress_case,NULL,NULL):
        fpe_job_submit(engine,op,one,1,dest,FPE_CONFLICT_OVERWRITE,progress_case,NULL);
    CHECK(test_wait_job(engine,id,J.done,30000)==WAIT_OBJECT_0);CloseHandle(J.done);
    CHECK(ancestry_calls==1);
    if(expected){CHECK(contents(a,"new source")&&contents(target,"old target"));CHECK(!J.final.items_done);CHECK(J.final.state==(expected==ERROR_CANCELLED?FPE_JOB_CANCELLED:FPE_JOB_FAILED));if(expected!=ERROR_CANCELLED)CHECK(J.final.last_error==expected);}
    else{CHECK(J.final.state==FPE_JOB_DONE&&!J.final.errors&&contents(target,"new source"));CHECK(op==FPE_OP_COPY?contents(a,"new source"):GetFileAttributesW(a)==INVALID_FILE_ATTRIBUTES);}
    if(GetFileAttributesW(a)!=INVALID_FILE_ATTRIBUTES)CHECK(DeleteFileW(a));CHECK(DeleteFileW(target));CHECK(RemoveDirectoryW(source)&&RemoveDirectoryW(dest));
}
static int stopped(void *u){return 0;}
static void test_windows_ancestry_cancel_before_open(void)
{clear();fpe_win_ancestry *a=(void*)1;CHECK(fpe_win_ancestry_open(winroot,stopped,NULL,&a)==ERROR_CANCELLED);CHECK(!a&&!launches);}
static void test_windows_ancestry_native_and_cross_volume_moves(void)
{for(int kind=0;kind<=2;kind++){clear();exercise(kind,0);}}
static void test_windows_ancestry_streamed_move_and_selected_copy(void)
{for(int kind=3;kind<=4;kind++){clear();exercise(kind,0);}}
static void test_windows_ancestry_failure_preserves_all_transfer_sources(void)
{for(int kind=0;kind<=4;kind++){clear();flush_error=ERROR_IO_DEVICE;exercise(kind,ERROR_IO_DEVICE);CHECK(!launches);}}
static void test_windows_ancestry_denied_elevation_preserves_move_source(void)
{clear();force_broker=deny_launch=1;exercise(1,ERROR_CANCELLED);CHECK(launches==1);}
static void test_windows_ancestry_missing_helper_preserves_stream_source(void)
{clear();force_broker=missing_helper=1;exercise(3,ERROR_FILE_NOT_FOUND);CHECK(!launches);}
static void test_windows_ancestry_wrong_peer_is_rejected(void)
{clear();force_broker=wrong_peer=1;exercise(1,ERROR_ACCESS_DENIED);CHECK(launches==1);}
static void test_windows_ancestry_barrier_restore_failure_stops_before_mutation(void)
{clear();restore_failure=1;exercise(1,ERROR_ACCESS_DENIED);CHECK(!launches);}
static void test_windows_ancestry_cancellation_after_flush_preserves_source(void)
{clear();cancel_flush=1;exercise(1,ERROR_CANCELLED);}
static void test_windows_ancestry_source_parent_guard_overlap(void)
{
    clear();wchar_t source[4096],dest[4096],target[4096];unsigned n=++serial;
    _snwprintf(source,4096,L"%ls\\overlap-%u",winroot,n);_snwprintf(dest,4096,L"%ls\\nested-%u",winroot,n);_snwprintf(target,4096,L"%ls\\overlap-%u",dest,n);
    put(source,"shared parent");CHECK(CreateDirectoryW(dest,NULL));memset(&J,0,sizeof J);J.done=test_callback_event(TRUE);const wchar_t *paths[]={source};
    uint64_t id=fpe_job_submit(engine,FPE_OP_MOVE,paths,1,dest,FPE_CONFLICT_FAIL,progress_case,NULL);CHECK(test_wait_job(engine,id,J.done,30000)==WAIT_OBJECT_0);CloseHandle(J.done);
    CHECK(J.final.state==FPE_JOB_DONE&&!J.final.errors&&contents(target,"shared parent"));CHECK(GetFileAttributesW(source)==INVALID_FILE_ATTRIBUTES);CHECK(DeleteFileW(target)&&RemoveDirectoryW(dest));
}
static void test_windows_ancestry_helper_death_is_failure(void)
{
    clear();durable_broker b={0};CHECK(!broker_open(&b,NULL,NULL));if(b.process){CHECK(TerminateJobObject(b.job,91));CHECK(WaitForSingleObject(b.process,5000)==WAIT_OBJECT_0);
        HANDLE pin=CreateFileW(winroot,FILE_TRAVERSE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);BY_HANDLE_FILE_INFORMATION i;CHECK(pin!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(pin,&i));CHECK(broker_flush(&b,pin,&i)!=0);CloseHandle(pin);}broker_close(&b);
}
static void test_windows_ancestry_helper_authenticates_parent_identity(void)
{clear();wrong_birth=1;durable_broker b={0};CHECK(broker_open(&b,NULL,NULL)!=0);CHECK(!b.pipe&&!b.job&&!b.process);}
static void test_windows_ancestry_endpoint_barrier_survives_flush_handoff(void)
{
    clear();fpe_win_ancestry *a=NULL;CHECK(!fpe_win_ancestry_open(winroot,NULL,NULL,&a));if(!a)return;HANDLE root=fpe_win_ancestry_root(a);
    HANDLE write=fpe_win_reopen_directory(root,GENERIC_WRITE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE);CHECK(write==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION);if(write!=INVALID_HANDLE_VALUE)CloseHandle(write);
    CHECK(!fpe_win_ancestry_exchange(a,root,0));write=fpe_win_reopen_directory(root,GENERIC_WRITE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE);CHECK(write!=INVALID_HANDLE_VALUE);if(write!=INVALID_HANDLE_VALUE){CHECK(!fpe_win_ancestry_validate(a,write));CloseHandle(write);}CHECK(!fpe_win_ancestry_exchange(a,root,1));
    write=fpe_win_reopen_directory(root,GENERIC_WRITE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE);CHECK(write==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION);if(write!=INVALID_HANDLE_VALUE)CloseHandle(write);fpe_win_ancestry_close(a);
}
typedef struct fake_peer {HANDLE pipe;unsigned mode;} fake_peer;
static DWORD WINAPI reply_fake(void *user)
{
    fake_peer *p=user;fpd_message q;DWORD n;
    if(ReadFile(p->pipe,&q,sizeof q,&n,NULL)&&n==sizeof q){q.handle=0;
        switch(p->mode){case 0:q.version++;break;case 1:q.sequence++;break;case 2:q.low^=1;break;case 3:q.handle=1;break;case 4:q.error=ERROR_IO_DEVICE;break;case 5:q.reserved=1;break;}
        WriteFile(p->pipe,&q,p->mode==6?sizeof q-1:sizeof q,&n,NULL);}
    CloseHandle(p->pipe);return 0;
}
static void test_windows_ancestry_malformed_and_error_replies_fail_closed(void)
{
    clear();for(unsigned mode=0;mode<7;mode++){
        wchar_t name[128];swprintf(name,128,L"\\\\.\\pipe\\OpenPilot-ancestry-test-%lu-%u",(unsigned long)GetCurrentProcessId(),mode);
        durable_broker b={.pipe=CreateNamedPipeW(name,PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_REJECT_REMOTE_CLIENTS,1,80,80,0,NULL)};
        CHECK(b.pipe!=INVALID_HANDLE_VALUE);fake_peer peer={.pipe=CreateFileW(name,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL),.mode=mode};CHECK(peer.pipe!=INVALID_HANDLE_VALUE);
        DWORD message=PIPE_READMODE_MESSAGE;CHECK(SetNamedPipeHandleState(peer.pipe,&message,NULL,NULL));CHECK(DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&b.process,SYNCHRONIZE,FALSE,0));
        HANDLE thread=CreateThread(NULL,0,reply_fake,&peer,0,NULL);BY_HANDLE_FILE_INFORMATION i={.dwVolumeSerialNumber=1,.nFileIndexHigh=2,.nFileIndexLow=3};
        DWORD error=broker_flush(&b,(HANDLE)(uintptr_t)7,&i);CHECK(error==(mode==4?ERROR_IO_DEVICE:ERROR_INVALID_DATA));CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);CloseHandle(thread);CloseHandle(b.process);b.process=NULL;broker_close(&b);
    }
}
static void test_windows_legacy_wsl_mutations_refused_before_changes(void)
{
    for(int kind=0;kind<6;kind++){
        clear();wchar_t folder[4096],source[4096],dest[4096],sentinel[4096],target[4096];unsigned n=++serial;
        int linux_source=kind==2||kind==4,windows_dest=kind==2,tree=kind==1;
        _snwprintf(folder,4096,L"%ls\\legacy-source-%u",linux_source?lxroot:winroot,n);
        _snwprintf(dest,4096,L"%ls\\legacy-dest-%u",windows_dest?winroot:lxroot,n);
        CHECK(CreateDirectoryW(folder,NULL)&&CreateDirectoryW(dest,NULL));
        _snwprintf(source,4096,L"%ls\\item",folder);put(source,"preserved source");
        _snwprintf(sentinel,4096,L"%ls\\sentinel",dest);put(sentinel,"preserved destination");
        const wchar_t *one[]={tree?folder:source},*relative[]={L"item"};
        fpe_wsl_configure(&(fpe_wsl_config){0,0,60000});
        memset(&J,0,sizeof J);J.done=test_callback_event(TRUE);
        uint64_t id=kind==3?fpe_job_submit_tree(engine,folder,relative,1,dest,FPE_CONFLICT_OVERWRITE,progress_case,NULL,NULL):
            fpe_job_submit(engine,kind==2||kind==5?FPE_OP_MOVE:FPE_OP_COPY,one,1,dest,FPE_CONFLICT_OVERWRITE,progress_case,NULL);
        CHECK(id&&test_wait_job(engine,id,J.done,30000)==WAIT_OBJECT_0);CloseHandle(J.done);
        CHECK(J.final.state==FPE_JOB_FAILED&&J.final.errors==1&&J.final.last_error==ERROR_NOT_SUPPORTED&&!J.final.items_done&&!J.final.bytes_done);
        CHECK(!ancestry_calls&&!launches&&contents(source,"preserved source")&&contents(sentinel,"preserved destination"));
        _snwprintf(target,4096,L"%ls\\*",dest);WIN32_FIND_DATAW data;HANDLE scan=FindFirstFileW(target,&data);unsigned count=0;
        if(scan!=INVALID_HANDLE_VALUE){do{if(wcscmp(data.cFileName,L".")&&wcscmp(data.cFileName,L".."))count++;}while(FindNextFileW(scan,&data));FindClose(scan);}
        CHECK(count==1); /* no publication, child directory or staging object */
        fpe_wsl_configure(&(fpe_wsl_config){1,0,60000});
        CHECK(DeleteFileW(source)&&DeleteFileW(sentinel)&&RemoveDirectoryW(folder)&&RemoveDirectoryW(dest));
    }
}
int wmain(int argc,wchar_t **argv)
{
    if(argc!=4)return 2;winroot=argv[1];lxroot=argv[2];crossroot=argv[3];CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    fpe_wsl_configure(&(fpe_wsl_config){1,0,60000});engine=fpe_engine_create(NULL);if(!engine)return 2;
    typedef void (*fn)(void);fn tests[]={test_windows_legacy_wsl_mutations_refused_before_changes,test_windows_ancestry_cancel_before_open,test_windows_ancestry_native_and_cross_volume_moves,test_windows_ancestry_streamed_move_and_selected_copy,test_windows_ancestry_failure_preserves_all_transfer_sources,test_windows_ancestry_denied_elevation_preserves_move_source,test_windows_ancestry_missing_helper_preserves_stream_source,test_windows_ancestry_wrong_peer_is_rejected,test_windows_ancestry_barrier_restore_failure_stops_before_mutation,test_windows_ancestry_cancellation_after_flush_preserves_source,test_windows_ancestry_source_parent_guard_overlap,test_windows_ancestry_helper_death_is_failure,test_windows_ancestry_helper_authenticates_parent_identity,test_windows_ancestry_endpoint_barrier_survives_flush_handoff,test_windows_ancestry_malformed_and_error_replies_fail_closed};
    const char *names[]={"test_windows_legacy_wsl_mutations_refused_before_changes","test_windows_ancestry_cancel_before_open","test_windows_ancestry_native_and_cross_volume_moves","test_windows_ancestry_streamed_move_and_selected_copy","test_windows_ancestry_failure_preserves_all_transfer_sources","test_windows_ancestry_denied_elevation_preserves_move_source","test_windows_ancestry_missing_helper_preserves_stream_source","test_windows_ancestry_wrong_peer_is_rejected","test_windows_ancestry_barrier_restore_failure_stops_before_mutation","test_windows_ancestry_cancellation_after_flush_preserves_source","test_windows_ancestry_source_parent_guard_overlap","test_windows_ancestry_helper_death_is_failure","test_windows_ancestry_helper_authenticates_parent_identity","test_windows_ancestry_endpoint_barrier_survives_flush_handoff","test_windows_ancestry_malformed_and_error_replies_fail_closed"};
    for(unsigned i=0;i<sizeof tests/sizeof *tests;i++){unsigned before=failed;uint64_t start=GetTickCount64();tests[i]();printf("CASE %s %s %llu\n",names[i],failed==before?"passed":"failed",(unsigned long long)(GetTickCount64()-start));fflush(stdout);}
    clear();fpe_engine_destroy(engine);fpe_wsl_shutdown();CoUninitialize();printf("%u passed, %u failed; 15 Windows ancestry selectors\n",passed,failed);return failed?1:0;
}
