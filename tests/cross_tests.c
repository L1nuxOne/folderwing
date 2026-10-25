/* Real engine jobs against two private distro paths supplied by acceptance. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "fpe.h"
#include "jobwait.h"
static int passed,failed;
#define CHECK(x) do{if(x)passed++;else{failed++;printf("  FAIL %s:%d %s\n",__FILE__,__LINE__,#x);}}while(0)
static const wchar_t *source,*destination,*missing;
static fpe_engine *engine;
static struct {HANDLE done;fpe_job_progress final;unsigned terminals,events;int cancel;wchar_t event_path[4096];} R;
static void progress(void *user,const fpe_job_progress *p){
    if(R.cancel&&p->state==FPE_JOB_RUNNING)fpe_job_cancel(engine,p->job);
    if(p->state>=FPE_JOB_DONE){R.final=*p;R.final.current=NULL;R.terminals++;SetEvent(R.done);}
}
static void event(void *user,const fpe_job_event *p){R.events++;if(p->path){wcsncpy(R.event_path,p->path,4095);R.event_path[4095]=0;}}
static void run(const wchar_t *dest,int cancel){
    memset(&R,0,sizeof R);R.done=test_callback_event(TRUE);R.cancel=cancel;
    const wchar_t *paths[]={source};uint64_t id=fpe_job_submit_ex(engine,FPE_OP_MOVE,paths,1,dest,FPE_CONFLICT_OVERWRITE,progress,event,NULL);
    CHECK(id!=0);CHECK(test_wait_job(engine,id,R.done,10000)==WAIT_OBJECT_0);CloseHandle(R.done);R.done=NULL;
}
static int contents(const wchar_t *path,const char *expected){
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    if(file==INVALID_HANDLE_VALUE)return 0;char bytes[256];DWORD got=0;int ok=ReadFile(file,bytes,sizeof bytes,&got,NULL)&&got==strlen(expected)&&!memcmp(bytes,expected,got);CloseHandle(file);return ok;
}
static void path(wchar_t *out,const wchar_t *base){_snwprintf(out,4096,L"%ls\\before.bin",base);out[4095]=0;}
static void test_cross_cancel_before_mutation(void){
    puts("cross engine: callback cancellation before the first mutation");run(destination,1);wchar_t dest[4096];path(dest,destination);
    CHECK(R.terminals==1);CHECK(R.final.state==FPE_JOB_CANCELLED);CHECK(R.final.errors==0);CHECK(R.events==0);
    CHECK(contents(source,"new"));CHECK(contents(dest,"old"));
}
static void test_cross_missing_endpoint(void){
    puts("cross engine: unavailable destination does not delete the source");run(missing,0);
    CHECK(R.terminals==1);CHECK(R.final.errors==1);CHECK(R.events==1);CHECK(!wcscmp(R.event_path,source));CHECK(contents(source,"new"));
}
static void test_cross_successful_move(void){
    puts("cross engine: verified move produces exactly one completion");run(destination,0);wchar_t dest[4096];path(dest,destination);
    CHECK(R.terminals==1);CHECK(R.final.state==FPE_JOB_DONE);CHECK(R.final.errors==0);CHECK(R.events==0);
    CHECK(R.final.items_done==1&&R.final.items_total==1);CHECK(contents(dest,"new"));CHECK(GetFileAttributesW(source)==INVALID_FILE_ATTRIBUTES);
}
static void test_index_distro_scopes(void)
{
    puts("cross index: static provider and independent selections in two Linux distros");
    wchar_t left[4096],file[4096];wcsncpy(left,source,4095);left[4095]=0;wchar_t *slash=wcsrchr(left,L'\\');if(!slash){CHECK(0);return;}*slash=0;
    const wchar_t *roots[]={left,destination};const wchar_t *names[]={L"index-left-needle",L"index-right-needle"};
    fpe_index_status info;fpe_search_stats stats;
    for(unsigned i=0;i<2;i++){
        _snwprintf(file,4096,L"%ls\\%ls",roots[i],names[i]);file[4095]=0;
        CHECK(!fpe_wsl_create(file,0,NULL));
        CHECK(!fpe_search_index_control(roots[i],FPE_INDEX_ENABLE,NULL,NULL,&info)&&info.enabled);
        uint64_t until=GetTickCount64()+5000;
        while(info.state!=FPE_INDEX_CURRENT&&GetTickCount64()<until){fpe_search_index_control(roots[i],FPE_INDEX_STATUS,NULL,NULL,&info);Sleep(10);}
        CHECK(info.state==FPE_INDEX_CURRENT&&fpe_path_equal(info.root,roots[i]));
        CHECK(!fpe_search_folder_indexed(roots[i],L"index-",NULL,NULL,&stats,&info)&&stats.matched==1);
        CHECK(!fpe_search_folder_indexed(roots[i],names[1-i],NULL,NULL,&stats,&info)&&!stats.matched&&!stats.errors);
    }
    CHECK(!fpe_search_index_control(roots[0],FPE_INDEX_REMOVE,NULL,NULL,&info)&&!info.enabled);
    CHECK(!fpe_search_index_control(roots[1],FPE_INDEX_REMOVE,NULL,NULL,&info)&&!info.enabled);
    fpe_search_index_shutdown();CHECK(!fpe_search_index_control(roots[1],FPE_INDEX_STATUS,NULL,NULL,&info)&&info.state==FPE_INDEX_NONE);
    fpe_search_index_shutdown();
}
int wmain(int argc,wchar_t **argv){
    if(argc!=4)return 2;source=argv[1];destination=argv[2];missing=argv[3];
    fpe_wsl_configure(&(fpe_wsl_config){1,0,60000});engine=fpe_engine_create(NULL);CHECK(engine!=NULL);if(!engine)return 1;
    test_cross_cancel_before_mutation();test_cross_missing_endpoint();test_cross_successful_move();test_index_distro_scopes();fpe_engine_destroy(engine);
    printf("%d passed, %d failed\n",passed,failed);return failed?1:0;
}
