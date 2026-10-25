/* Selected-copy coordinator ancestry gate: actual roots and child jobs,
 * private failure/cancel controls only in this test translation unit. */
#include "../src/internal.h"
#include <stdio.h>
static unsigned passed,failed,ancestry_calls;static DWORD injected;
static fpe_engine *engine;static uint64_t current_job;static int cancel_ancestry;
#define CHECK(x) do{if(x)passed++;else{failed++;printf("FAIL %d %s err=%lu\n",__LINE__,#x,(unsigned long)GetLastError());}}while(0)
static fpe_err observed_ancestry(fpe_path_guard *g,fpe_compare_cb cb,void *user)
{
    ancestry_calls++;CHECK(g->linux&&g->process&&g->to&&g->from);
    if(cancel_ancestry){fpe_job_cancel(engine,current_job);return ERROR_CANCELLED;}
    return injected?injected:fpe_wsl_guard_ancestry(g,cb,user);
}
#define fpe_wsl_guard_ancestry observed_ancestry
#include "../src/jobs.c"
#undef fpe_wsl_guard_ancestry
#include "jobwait.h"
static const wchar_t *winroot,*lxroot;static unsigned serial;
static struct {HANDLE done;fpe_job_progress final;unsigned errors;} J;
static void progress_case(void *user,const fpe_job_progress *p)
{current_job=p->job;if(p->state>=FPE_JOB_DONE){J.final=*p;J.final.current=NULL;SetEvent(J.done);}}
static void event_case(void *user,const fpe_job_event *event)
{if(event->kind==FPE_JOB_ERROR)J.errors++;}
static void put(const wchar_t *path,const char *data)
{HANDLE h=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);CHECK(h!=INVALID_HANDLE_VALUE);if(h!=INVALID_HANDLE_VALUE){DWORD n;CHECK(WriteFile(h,data,(DWORD)strlen(data),&n,NULL)&&n==strlen(data));CloseHandle(h);}}
static int contents(const wchar_t *path,const char *data)
{HANDLE h=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);if(h==INVALID_HANDLE_VALUE)return 0;char bytes[100];DWORD n;int ok=ReadFile(h,bytes,sizeof bytes,&n,NULL)&&n==strlen(data)&&!memcmp(bytes,data,n);CloseHandle(h);return ok;}
static void exercise(DWORD error,int cancel)
{
    wchar_t source[4096],dest[4096],a[4096],b[4096],da[4096],db[4096];
    _snwprintf(source,4096,L"%ls\\source-%u",winroot,++serial);_snwprintf(dest,4096,L"%ls\\destination-%u",lxroot,serial);
    CHECK(CreateDirectoryW(source,NULL));CHECK(CreateDirectoryW(dest,NULL));
    _snwprintf(a,4096,L"%ls\\a",source);_snwprintf(b,4096,L"%ls\\b",source);put(a,"selected-a");put(b,"selected-b");
    _snwprintf(da,4096,L"%ls\\a",dest);_snwprintf(db,4096,L"%ls\\b",dest);put(da,"old target");
    ancestry_calls=0;injected=error;cancel_ancestry=cancel;memset(&J,0,sizeof J);J.done=test_callback_event(TRUE);
    const wchar_t *paths[]={L"a",L"b"};uint64_t id=fpe_job_submit_tree(engine,source,paths,2,dest,FPE_CONFLICT_OVERWRITE,progress_case,event_case,NULL);
    CHECK(test_wait_job(engine,id,J.done,30000)==WAIT_OBJECT_0);CloseHandle(J.done);
    CHECK(ancestry_calls==1);CHECK(contents(a,"selected-a")&&contents(b,"selected-b"));
    if(error||cancel){CHECK(contents(da,"old target"));CHECK(GetFileAttributesW(db)==INVALID_FILE_ATTRIBUTES);CHECK(J.final.items_done==0);CHECK(J.final.state==(cancel?FPE_JOB_CANCELLED:FPE_JOB_FAILED));CHECK(J.errors==(cancel?0u:1u));}
    else{CHECK(contents(da,"selected-a")&&contents(db,"selected-b"));CHECK(J.final.state==FPE_JOB_DONE&&!J.final.errors&&J.final.items_done==2);CHECK(DeleteFileW(db));}
    CHECK(DeleteFileW(a)&&DeleteFileW(b)&&DeleteFileW(da));CHECK(RemoveDirectoryW(source)&&RemoveDirectoryW(dest));
}
static void test_selected_copy_rejects_ancestry_failure_before_mutation(void)
{DWORD errors[]={ERROR_ACCESS_DENIED,ERROR_NOT_SUPPORTED,ERROR_IO_DEVICE};for(unsigned i=0;i<3;i++)exercise(errors[i],0);}
static void test_selected_copy_cancelled_ancestry_preserves_sources(void)
{exercise(0,1);}
static void test_selected_copy_fences_root_once_for_multiple_files(void)
{exercise(0,0);}
int wmain(int argc,wchar_t **argv)
{
    if(argc!=3)return 2;winroot=argv[1];lxroot=argv[2];fpe_wsl_configure(&(fpe_wsl_config){1,0,60000});engine=fpe_engine_create(NULL);if(!engine)return 2;
    typedef void (*fn)(void);fn tests[]={test_selected_copy_rejects_ancestry_failure_before_mutation,test_selected_copy_cancelled_ancestry_preserves_sources,test_selected_copy_fences_root_once_for_multiple_files};
    const char *names[]={"test_selected_copy_rejects_ancestry_failure_before_mutation","test_selected_copy_cancelled_ancestry_preserves_sources","test_selected_copy_fences_root_once_for_multiple_files"};
    for(unsigned i=0;i<3;i++){unsigned before=failed;uint64_t start=GetTickCount64();tests[i]();printf("CASE %s %s %llu\n",names[i],failed==before?"passed":"failed",(unsigned long long)(GetTickCount64()-start));fflush(stdout);}
    fpe_engine_destroy(engine);fpe_wsl_shutdown();printf("%u passed, %u failed; 3 ancestry selectors\n",passed,failed);return failed?1:0;
}
