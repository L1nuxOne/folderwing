/* Selected-copy directory persistence and guard handoff. Faults are test-only.
 * Primary: Windows native. Owner: selected-copy durability/namespace guards.
 * Impact: jobs.c, pathguard.c, internal.h, wsl.c, wslproto.h, fpguard.c.
 * Required lanes: native acceptance and existing tree/search/transfer contracts. */
#include "../src/internal.h"
#include "../src/wslproto.h"
#include <winternl.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
static int passed,failed;
#define CHECK(x) do{if(x)passed++;else{failed++;printf("FAIL %d %s err=%lu\n",__LINE__,#x,(unsigned long)GetLastError());}}while(0)
typedef NTSTATUS (NTAPI *tree_create_fn)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,PLARGE_INTEGER,ULONG,ULONG,ULONG,ULONG,PVOID,ULONG);
static struct {
    wchar_t match[4096],target[4096],file[4096],paths[256][4096];
    unsigned calls,hits,fail_nth,cancel_nth;DWORD error;int restore_error,reparse,retargeted,visible[256];
    fpe_engine *engine;uint64_t job;
} T;
static tree_create_fn real_create;
static NTSTATUS NTAPI observed_create(PHANDLE h,ACCESS_MASK access,POBJECT_ATTRIBUTES object,PIO_STATUS_BLOCK io,PLARGE_INTEGER size,ULONG attr,ULONG share,ULONG disposition,ULONG options,PVOID ea,ULONG bytes)
{
    CHECK(object->RootDirectory&&object->ObjectName&&!object->ObjectName->Length);
    CHECK((options&0x200001)==0x200001&&!(share&FILE_SHARE_DELETE));
    if(T.restore_error&&T.calls&&!(access&GENERIC_WRITE))return (NTSTATUS)0xc0000022;
    return real_create(h,access,object,io,size,attr,share,disposition,options,ea,bytes);
}
static FARPROC WINAPI observed_lookup(HMODULE module,LPCSTR name)
{
    FARPROC value=GetProcAddress(module,name);
    if(!strcmp(name,"NtCreateFile")){union {FARPROC generic;tree_create_fn create;} cast={.generic=value};real_create=cast.create;cast.create=observed_create;return cast.generic;}return value;
}
static int retarget(HANDLE h,const wchar_t *target)
{
    struct {DWORD tag;WORD length,reserved,suboff,sublen,printoff,printlen;wchar_t path[8192];} data={0};
    data.tag=IO_REPARSE_TAG_MOUNT_POINT;_snwprintf(data.path,8192,L"\\??\\%ls",target);
    data.sublen=(WORD)(wcslen(data.path)*sizeof(wchar_t));data.printoff=data.sublen+sizeof(wchar_t);
    wcscpy((wchar_t*)((unsigned char*)data.path+data.printoff),target);data.printlen=(WORD)(wcslen(target)*sizeof(wchar_t));data.length=8+data.printoff+data.printlen+sizeof(wchar_t);
    DWORD done=0;return DeviceIoControl(h,FSCTL_SET_REPARSE_POINT,&data,data.length+8,NULL,0,&done,NULL);
}
static BOOL WINAPI observed_flush(HANDLE h)
{
    wchar_t path[4096];DWORD n=GetFinalPathNameByHandleW(h,path,4096,0);CHECK(n&&n<4096);
    const wchar_t *shown=!wcsncmp(path,L"\\\\?\\",4)?path+4:path;
    unsigned index=T.calls++;CHECK(index<256);if(index<256){wcscpy(T.paths[index],shown);T.visible[index]=T.file[0]&&GetFileAttributesW(T.file)!=INVALID_FILE_ATTRIBUTES;}
    int hit=!T.match[0]||!_wcsicmp(shown,T.match);unsigned count=hit?++T.hits:0;
    if(hit&&count==T.fail_nth){SetLastError(T.error);return FALSE;}
    BOOL ok=FlushFileBuffers(h);DWORD error=ok?0:GetLastError();
    if(ok&&T.reparse&&hit&&!T.retargeted){T.retargeted=retarget(h,T.target);CHECK(T.retargeted);}
    if(hit&&count==T.cancel_nth&&T.engine)fpe_job_cancel(T.engine,T.job);
    SetLastError(error);return ok;
}
#define GetProcAddress observed_lookup
#define FlushFileBuffers observed_flush
#include "../src/pathguard.c"
#undef GetProcAddress
#undef FlushFileBuffers
static const wchar_t *winroot,*lxroot;static unsigned serial;static fpe_engine *engine;
static struct {HANDLE done;fpe_job_progress last;unsigned finals,errors;} J;
static void join(wchar_t *out,const wchar_t *a,const wchar_t *b){_snwprintf(out,4096,L"%ls\\%ls",a,b);}
static int present(const wchar_t *p){return GetFileAttributesW(p)!=INVALID_FILE_ATTRIBUTES;}
static void folder(wchar_t *out,const wchar_t *root){wchar_t name[60];_snwprintf(name,60,L"selected-%u",++serial);join(out,root,name);CHECK(CreateDirectoryW(out,NULL));}
static void write_bytes(const wchar_t *path)
{HANDLE h=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);CHECK(h!=INVALID_HANDLE_VALUE);if(h!=INVALID_HANDLE_VALUE){DWORD n=0;CHECK(WriteFile(h,"selected",8,&n,NULL)&&n==8);CloseHandle(h);}}
static int bytes(const wchar_t *path)
{HANDLE h=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);if(h==INVALID_HANDLE_VALUE)return 0;char b[9];DWORD n=0;int ok=ReadFile(h,b,9,&n,NULL)&&n==8&&!memcmp(b,"selected",8);CloseHandle(h);return ok;}
static void denied(const wchar_t *path)
{HANDLE h=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);CHECK(h==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION);if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}
static void progress(void *user,const fpe_job_progress *p){T.job=p->job;if(p->state>=FPE_JOB_DONE){J.last=*p;J.last.current=NULL;J.finals++;SetEvent(J.done);}}
static void event(void *user,const fpe_job_event *e){if(e->kind==FPE_JOB_ERROR)J.errors++;}
static void run(const wchar_t *src,const wchar_t *dst,const wchar_t *const *paths,unsigned count,fpe_conflict policy)
{
    memset(&J,0,sizeof J);J.done=CreateEventW(NULL,TRUE,FALSE,NULL);T.engine=engine;
    uint64_t id=fpe_job_submit_tree(engine,src,paths,count,dst,policy,progress,event,NULL);CHECK(id!=0);
    DWORD wait=WaitForSingleObject(J.done,30000);CHECK(wait==WAIT_OBJECT_0);if(wait!=WAIT_OBJECT_0){fpe_job_cancel(engine,id);CHECK(WaitForSingleObject(J.done,30000)==WAIT_OBJECT_0);}
    CHECK(J.finals==1);CloseHandle(J.done);
}
static void test_held_directory_flush_restores_guards(void)
{
    memset(&T,0,sizeof T);wchar_t path[4096],moved[4096];folder(path,winroot);join(moved,winroot,L"renamed");
    fpe_path_guard g={0};CHECK(!fpe_guard_open(path,0,0,0,0,NULL,NULL,&g));fpe_path_guard *held[]={&g};denied(path);
    CHECK(!fpe_guard_sync(&g,held,1,NULL,NULL));CHECK(T.calls==1);denied(path);CHECK(!MoveFileW(path,moved));
    fpe_guard_close(&g);CHECK(RemoveDirectoryW(path));
}
static void test_duplicate_guards_flush_same_object(void)
{
    memset(&T,0,sizeof T);wchar_t path[4096];folder(path,winroot);fpe_path_guard g[2]={{0}};fpe_path_guard *held[]={g,g+1};
    CHECK(!fpe_guard_open(path,0,0,0,0,NULL,NULL,g));CHECK(!fpe_guard_open(path,0,0,0,0,NULL,NULL,g+1));
    CHECK(!fpe_guard_sync(g,held,2,NULL,NULL));denied(path);CHECK(g[0].barrier&&g[1].barrier);
    fpe_guard_close(g+1);fpe_guard_close(g);CHECK(RemoveDirectoryW(path));
}
static void test_directory_flush_errors_restore_guard(void)
{
    DWORD errors[]={ERROR_ACCESS_DENIED,ERROR_NOT_SUPPORTED,ERROR_DISK_FULL,ERROR_GEN_FAILURE};
    for(unsigned i=0;i<4;i++){
        memset(&T,0,sizeof T);wchar_t path[4096];folder(path,winroot);fpe_path_guard g={0};fpe_path_guard *held[]={&g};
        CHECK(!fpe_guard_open(path,0,0,0,0,NULL,NULL,&g));T.fail_nth=1;T.error=errors[i];CHECK(fpe_guard_sync(&g,held,1,NULL,NULL)==errors[i]);denied(path);
        fpe_guard_close(&g);CHECK(RemoveDirectoryW(path));
    }
}
static void test_restore_failure_is_not_success(void)
{
    memset(&T,0,sizeof T);wchar_t path[4096];folder(path,winroot);fpe_path_guard g={0};fpe_path_guard *held[]={&g};
    CHECK(!fpe_guard_open(path,0,0,0,0,NULL,NULL,&g));T.restore_error=1;CHECK(fpe_guard_sync(&g,held,1,NULL,NULL)==ERROR_ACCESS_DENIED);CHECK(!g.barrier);T.restore_error=0;
    fpe_guard_close(&g);CHECK(RemoveDirectoryW(path));
}
static void test_external_read_guard_remains_effective(void)
{
    memset(&T,0,sizeof T);wchar_t path[4096];folder(path,winroot);fpe_path_guard g={0};fpe_path_guard *held[]={&g};
    CHECK(!fpe_guard_open(path,0,0,0,0,NULL,NULL,&g));HANDLE other=CreateFileW(path,FILE_TRAVERSE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);CHECK(other!=INVALID_HANDLE_VALUE);
    CHECK(fpe_guard_sync(&g,held,1,NULL,NULL)==ERROR_SHARING_VIOLATION);CHECK(T.calls==0);denied(path);CloseHandle(other);fpe_guard_close(&g);CHECK(RemoveDirectoryW(path));
}
static int cancel_after_flush(void *user){return T.calls==0;}
static void test_cancellation_restores_guard(void)
{
    memset(&T,0,sizeof T);wchar_t path[4096];folder(path,winroot);fpe_path_guard g={0};fpe_path_guard *held[]={&g};CHECK(!fpe_guard_open(path,0,0,0,0,NULL,NULL,&g));
    CHECK(fpe_guard_sync(&g,held,1,cancel_after_flush,NULL)==ERROR_CANCELLED);CHECK(T.calls==1);denied(path);fpe_guard_close(&g);CHECK(RemoveDirectoryW(path));
}
static void test_in_place_reparse_conversion_is_rejected(void)
{
    memset(&T,0,sizeof T);wchar_t path[4096],target[4096],file[4096];folder(path,winroot);folder(target,winroot);join(file,target,L"sentinel");write_bytes(file);
    fpe_path_guard g={0};fpe_path_guard *held[]={&g};CHECK(!fpe_guard_open(path,0,0,0,0,NULL,NULL,&g));T.reparse=1;wcscpy(T.target,target);
    CHECK(fpe_guard_sync(&g,held,1,NULL,NULL)==ERROR_CANT_ACCESS_FILE);CHECK(T.retargeted);CHECK(bytes(file));CHECK(T.calls==1);
    fpe_guard_close(&g);CHECK(RemoveDirectoryW(path));CHECK(DeleteFileW(file));CHECK(RemoveDirectoryW(target));
}
static void nested(const wchar_t *root,wchar_t *src,wchar_t *dst,wchar_t *source,wchar_t *output)
{
    folder(src,root);folder(dst,winroot);wchar_t path[4096];join(path,src,L"branch");CHECK(CreateDirectoryW(path,NULL));join(path,src,L"branch\\deep");CHECK(CreateDirectoryW(path,NULL));
    join(source,src,L"branch\\deep\\picked");write_bytes(source);join(output,dst,L"branch\\deep\\picked");
}
static void test_selected_nested_copy_barriers(void)
{
    memset(&T,0,sizeof T);wchar_t src[4096],dst[4096],source[4096],output[4096],extra[4096];nested(winroot,src,dst,source,output);join(extra,src,L"branch\\unselected");write_bytes(extra);
    wcscpy(T.file,output);const wchar_t *paths[]={L"branch\\deep\\picked"};run(src,dst,paths,1,FPE_CONFLICT_FAIL);
    CHECK(J.last.state==FPE_JOB_DONE&&!J.last.errors&&J.last.items_done==1);CHECK(bytes(source)&&bytes(output));CHECK(T.calls==8);
    CHECK(!_wcsicmp(T.paths[0],dst)&&!T.visible[0]);CHECK(!T.visible[4]&&T.visible[5]&&T.visible[6]&&T.visible[7]);
    wchar_t branch[4096],deep[4096];join(branch,dst,L"branch");join(deep,dst,L"branch\\deep");CHECK(!_wcsicmp(T.paths[5],deep)&&!_wcsicmp(T.paths[6],branch)&&!_wcsicmp(T.paths[7],dst));join(extra,dst,L"branch\\unselected");CHECK(!present(extra));
}
static void test_initial_barrier_failure_stops_copy(void)
{
    memset(&T,0,sizeof T);wchar_t src[4096],dst[4096],source[4096],output[4096];nested(winroot,src,dst,source,output);join(T.match,dst,L"branch\\deep");T.fail_nth=1;T.error=ERROR_NOT_SUPPORTED;
    const wchar_t *paths[]={L"branch\\deep\\picked"};run(src,dst,paths,1,FPE_CONFLICT_FAIL);CHECK(J.last.state==FPE_JOB_DONE&&J.last.errors==1&&J.errors==1);CHECK(bytes(source)&&!present(output));
}
static void test_final_barrier_failure_preserves_published_copy(void)
{
    memset(&T,0,sizeof T);wchar_t src[4096],dst[4096],source[4096],output[4096],second[4096];nested(winroot,src,dst,source,output);join(second,src,L"later");write_bytes(second);join(T.match,dst,L"branch\\deep");T.fail_nth=2;T.error=ERROR_DISK_FULL;
    const wchar_t *paths[]={L"branch\\deep\\picked",L"later"};run(src,dst,paths,2,FPE_CONFLICT_FAIL);CHECK(J.last.state==FPE_JOB_DONE&&J.last.errors==1&&J.last.items_done==1);CHECK(bytes(source)&&bytes(output)&&bytes(second));join(second,dst,L"later");CHECK(!present(second));
}
static void test_cancel_after_publication_retains_both(void)
{
    memset(&T,0,sizeof T);wchar_t src[4096],dst[4096],source[4096],output[4096];nested(winroot,src,dst,source,output);join(T.match,dst,L"branch\\deep");T.cancel_nth=2;
    const wchar_t *paths[]={L"branch\\deep\\picked"};run(src,dst,paths,1,FPE_CONFLICT_FAIL);CHECK(J.last.state==FPE_JOB_CANCELLED&&!J.last.errors);CHECK(bytes(source)&&bytes(output));
}
static void test_same_root_selected_copy(void)
{
    memset(&T,0,sizeof T);wchar_t root[4096],source[4096],copy[4096];folder(root,winroot);join(source,root,L"picked");write_bytes(source);const wchar_t *paths[]={L"picked"};
    run(root,root,paths,1,FPE_CONFLICT_RENAME);join(copy,root,L"picked (2)");CHECK(J.last.state==FPE_JOB_DONE&&!J.last.errors);CHECK(bytes(source)&&bytes(copy));
}
static void test_selected_linux_to_windows(void)
{
    memset(&T,0,sizeof T);wchar_t src[4096],dst[4096],source[4096],output[4096];nested(lxroot,src,dst,source,output);const wchar_t *paths[]={L"branch\\deep\\picked"};run(src,dst,paths,1,FPE_CONFLICT_FAIL);
    CHECK(J.last.state==FPE_JOB_DONE&&!J.last.errors);CHECK(bytes(source)&&bytes(output)&&T.calls==8);
}
static void test_selected_windows_to_linux(void)
{
    memset(&T,0,sizeof T);wchar_t src[4096],dummy[4096],source[4096],unused[4096],dst[4096],output[4096];nested(winroot,src,dummy,source,unused);folder(dst,lxroot);join(output,dst,L"branch\\deep\\picked");
    const wchar_t *paths[]={L"branch\\deep\\picked"};run(src,dst,paths,1,FPE_CONFLICT_FAIL);CHECK(J.last.state==FPE_JOB_DONE&&!J.last.errors);CHECK(bytes(source)&&bytes(output)&&T.calls==0);
}
static int second_pulse_cancels(void *user){return (*(int *)user)++==0;}
static void test_guard_sync_ack_validation(void)
{
    struct {uint32_t type,size,id;int32_t error;DWORD expected;} cases[]={
        {57,4,1,0,0},{57,4,1,5,ERROR_IO_DEVICE},{57,4,1,95,ERROR_NOT_SUPPORTED},{57,4,1,125,ERROR_CANCELLED},
        {57,4,1,-1,ERROR_INVALID_DATA},{55,4,1,0,ERROR_INVALID_DATA},{57,8,1,0,ERROR_INVALID_DATA},{57,4,2,0,ERROR_INVALID_DATA}};
    for(int ancestry=0;ancestry<2;ancestry++)for(unsigned i=0;i<sizeof cases/sizeof *cases;i++){
        fpe_path_guard g={.linux=1,.process=GetCurrentProcess()};HANDLE reader,writer;CHECK(CreatePipe(&reader,&g.to,NULL,0));CHECK(CreatePipe(&g.from,&writer,NULL,0));
        wslp_head header={cases[i].type,cases[i].size,cases[i].id};DWORD n=0;CHECK(WriteFile(writer,&header,sizeof header,&n,NULL));CHECK(WriteFile(writer,&cases[i].error,4,&n,NULL));
        CHECK((ancestry?fpe_wsl_guard_ancestry(&g,NULL,NULL):fpe_wsl_guard_sync(&g,NULL,NULL))==cases[i].expected);
        CHECK(ReadFile(reader,&header,sizeof header,&n,NULL)&&n==sizeof header&&header.type==(ancestry?58u:56u)&&header.size==0&&header.id==1);
        CloseHandle(reader);CloseHandle(writer);CloseHandle(g.to);CloseHandle(g.from);
    }
    for(int ancestry=0;ancestry<2;ancestry++)for(int cancel=0;cancel<2;cancel++){
        fpe_path_guard g={.linux=1,.process=GetCurrentProcess()};HANDLE reader,writer;CHECK(CreatePipe(&reader,&g.to,NULL,0));CHECK(CreatePipe(&g.from,&writer,NULL,0));
        wslp_head header={57,4,1};DWORD n=0;int32_t error=0;int pulses=0;
        if(cancel){CHECK(WriteFile(writer,&header,sizeof header,&n,NULL));CHECK(WriteFile(writer,&error,4,&n,NULL));}else{CloseHandle(writer);writer=NULL;}
        CHECK((ancestry?fpe_wsl_guard_ancestry(&g,cancel?second_pulse_cancels:NULL,&pulses):fpe_wsl_guard_sync(&g,cancel?second_pulse_cancels:NULL,&pulses))==(cancel?ERROR_CANCELLED:ERROR_BROKEN_PIPE));
        CloseHandle(reader);if(writer)CloseHandle(writer);CloseHandle(g.to);CloseHandle(g.from);
    }
}
typedef void (*test_fn)(void);
int wmain(int argc,wchar_t **argv)
{
    if(argc!=3)return 2;winroot=argv[1];lxroot=argv[2];fpe_wsl_configure(&(fpe_wsl_config){1,0,60000});engine=fpe_engine_create(NULL);CHECK(engine!=NULL);if(!engine)return 1;
    test_fn cases[]={test_held_directory_flush_restores_guards,test_duplicate_guards_flush_same_object,test_directory_flush_errors_restore_guard,test_restore_failure_is_not_success,test_external_read_guard_remains_effective,test_cancellation_restores_guard,test_in_place_reparse_conversion_is_rejected,test_selected_nested_copy_barriers,test_initial_barrier_failure_stops_copy,test_final_barrier_failure_preserves_published_copy,test_cancel_after_publication_retains_both,test_same_root_selected_copy,test_selected_linux_to_windows,test_selected_windows_to_linux,test_guard_sync_ack_validation};
    const char *names[]={"test_held_directory_flush_restores_guards","test_duplicate_guards_flush_same_object","test_directory_flush_errors_restore_guard","test_restore_failure_is_not_success","test_external_read_guard_remains_effective","test_cancellation_restores_guard","test_in_place_reparse_conversion_is_rejected","test_selected_nested_copy_barriers","test_initial_barrier_failure_stops_copy","test_final_barrier_failure_preserves_published_copy","test_cancel_after_publication_retains_both","test_same_root_selected_copy","test_selected_linux_to_windows","test_selected_windows_to_linux","test_guard_sync_ack_validation"};
    for(unsigned i=0;i<sizeof cases/sizeof *cases;i++){int before=failed;uint64_t start=GetTickCount64();cases[i]();printf("CASE %s %s %llu\n",names[i],before==failed?"passed":"failed",(unsigned long long)(GetTickCount64()-start));}
    fpe_engine_destroy(engine);fpe_wsl_shutdown();printf("%d passed, %d failed; 15 selected-copy durability contracts\n",passed,failed);return failed?1:0;
}
