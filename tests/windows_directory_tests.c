/* Checked Windows directory barriers, with real NTFS and WSL private trees.
 * API faults and observation exist only in this executable's included writer. */
#include "../src/internal.h"
#include "../src/wslproto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int passed,failed;
#define CHECK(x) do{if(x)passed++;else{failed++;printf("FAIL %s:%d %s (winerr=%lu)\n",__FILE__,__LINE__,#x,(unsigned long)GetLastError());}}while(0)
typedef struct observed {char kind;DWORD error;int source;wchar_t path[4096];} observed;
static struct {
    observed log[256];unsigned count,matched,fail_nth,cancel_nth;
    wchar_t match[4096],source[4096];DWORD error;int metadata_failure;
    fpe_engine *engine;uint64_t job;
} T;
static void record(char kind,const wchar_t *path,DWORD error)
{
    CHECK(T.count<256);if(T.count>=256)return;
    observed *o=T.log+T.count++;o->kind=kind;o->error=error;o->source=T.source[0]&&GetFileAttributesW(T.source)!=INVALID_FILE_ATTRIBUTES;
    if(path){wcsncpy(o->path,path,4095);o->path[4095]=0;}
}
static BOOL WINAPI directory_flush(HANDLE file)
{
    BY_HANDLE_FILE_INFORMATION info;CHECK(GetFileInformationByHandle(file,&info));
    int dir=(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;wchar_t path[4096]={0};
    DWORD n=GetFinalPathNameByHandleW(file,path,4096,0);CHECK(n&&n<4096);
    const wchar_t *shown=!wcsncmp(path,L"\\\\?\\",4)?path+4:path;
    unsigned hit=0;if(dir&&T.match[0]&&!_wcsicmp(shown,T.match))hit=++T.matched;
    if(hit&&hit==T.fail_nth){record('D',shown,T.error);SetLastError(T.error);return FALSE;}
    BOOL ok=FlushFileBuffers(file);DWORD error=ok?0:GetLastError();record(dir?'D':'F',shown,error);
    if(hit&&hit==T.cancel_nth&&T.engine)fpe_job_cancel(T.engine,T.job);
    SetLastError(error);return ok;
}
static BOOL WINAPI directory_write(HANDLE file,LPCVOID bytes,DWORD size,LPDWORD wrote,LPOVERLAPPED overlapped)
{
    if(size==sizeof(wslp_head)+sizeof(int32_t)){
        wslp_head h;memcpy(&h,bytes,sizeof h);
        if((h.type==WSLP_FILE_ACK||h.type==WSLP_DIRECTORY_ACK)&&h.id==1&&h.size==sizeof(int32_t)){
            int32_t error;memcpy(&error,(const char *)bytes+sizeof h,sizeof error);
            record(h.type==WSLP_FILE_ACK?'A':'K',NULL,(DWORD)error);
        }
    }
    /* Protocol v7 staged credits are not durable ACKs. Observe only the
     * correlated result after the unchanged file and directory barriers. */
    if(size==sizeof(wslp_head)+sizeof(wslp_file_result)){
        wslp_head h;memcpy(&h,bytes,sizeof h);
        if(h.type==WSLP_FILE_COMMITTED&&h.id==1&&h.size==sizeof(wslp_file_result)){
            wslp_file_result result;memcpy(&result,(const char*)bytes+sizeof h,sizeof result);
            record('A',NULL,(DWORD)result.error);
        }
    }
    return WriteFile(file,bytes,size,wrote,overlapped);
}
static BOOL WINAPI directory_time(HANDLE file,const FILETIME *create,const FILETIME *access,const FILETIME *modify)
{
    BY_HANDLE_FILE_INFORMATION info;
    if(T.metadata_failure&&GetFileInformationByHandle(file,&info)&&(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)){
        SetLastError(ERROR_ACCESS_DENIED);return FALSE;
    }
    return SetFileTime(file,create,access,modify);
}
#define FlushFileBuffers directory_flush
#define WriteFile directory_write
#define SetFileTime directory_time
#include "../src/wsl.c"
#undef FlushFileBuffers
#undef WriteFile
#undef SetFileTime
static const wchar_t *winroot,*lxroot;
static fpe_engine *engine;
static unsigned serial;
static struct {HANDLE done;fpe_job_progress last;unsigned finals,events;} J;
static const char bytes[]="directory barrier payload";
static void join_test(wchar_t *out,const wchar_t *a,const wchar_t *b)
{_snwprintf(out,4096,L"%ls\\%ls",a,b);out[4095]=0;}
static int present(const wchar_t *p){return GetFileAttributesW(p)!=INVALID_FILE_ATTRIBUTES;}
static int content(const wchar_t *p)
{
    HANDLE h=CreateFileW(p,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);if(h==INVALID_HANDLE_VALUE)return 0;
    char data[100];DWORD got=0;int ok=ReadFile(h,data,sizeof data,&got,NULL)&&got==sizeof bytes-1&&!memcmp(data,bytes,got);CloseHandle(h);return ok;
}
static void write_test(const wchar_t *p)
{
    HANDLE h=CreateFileW(p,GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);CHECK(h!=INVALID_HANDLE_VALUE);if(h==INVALID_HANDLE_VALUE)return;
    DWORD wrote=0;CHECK(WriteFile(h,bytes,sizeof bytes-1,&wrote,NULL)&&wrote==sizeof bytes-1);CHECK(CloseHandle(h));
}
static void fixture(wchar_t *src,wchar_t *dst,int child)
{
    memset(&T,0,sizeof T);wchar_t name[100];_snwprintf(name,100,L"tree-%u",++serial);join_test(src,lxroot,name);join_test(dst,winroot,name);CHECK(CreateDirectoryW(src,NULL));
    wchar_t file[4096];if(child){join_test(file,src,L"child");write_test(file);wcscpy(T.source,file);}else wcscpy(T.source,src);
}
static void progress_test(void *user,const fpe_job_progress *p)
{T.job=p->job;if(p->state>=FPE_JOB_DONE){J.last=*p;J.last.current=NULL;J.finals++;SetEvent(J.done);}}
static void event_test(void *user,const fpe_job_event *e){J.events++;}
static void run_test(const wchar_t *src,int move)
{
    memset(&J,0,sizeof J);J.done=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(J.done!=NULL);const wchar_t *one[]={src};
    uint64_t id=fpe_job_submit_ex(engine,move?FPE_OP_MOVE:FPE_OP_COPY,one,1,winroot,FPE_CONFLICT_OVERWRITE,progress_test,event_test,NULL);
    CHECK(id!=0);DWORD wait=WaitForSingleObject(J.done,30000);CHECK(wait==WAIT_OBJECT_0);
    if(wait!=WAIT_OBJECT_0){fpe_job_cancel(engine,id);CHECK(WaitForSingleObject(J.done,30000)==WAIT_OBJECT_0);}
    CHECK(J.finals==1);CloseHandle(J.done);
}
static unsigned count(char kind,DWORD error)
{unsigned n=0;for(unsigned i=0;i<T.count;i++)if(T.log[i].kind==kind&&T.log[i].error==error)n++;return n;}
static void cleanup_leaf_tree(const wchar_t *src,const wchar_t *dst)
{
    wchar_t p[4096];join_test(p,src,L"child");if(present(p))CHECK(DeleteFileW(p));if(present(src))CHECK(RemoveDirectoryW(src));
    join_test(p,dst,L"child");if(present(p))CHECK(DeleteFileW(p));if(present(dst))CHECK(RemoveDirectoryW(dst));
}
static void test_real_directory_barriers(void)
{
    memset(&T,0,sizeof T);wchar_t folder[4096];join_test(folder,winroot,L"direct");CHECK(CreateDirectoryW(folder,NULL));
    const char suffix[]="/direct";wslp_directory d={0,sizeof suffix-1};CHECK(!windows_directory_sync(&d,(const uint8_t *)suffix,winroot,NULL));
    CHECK(T.count==2&&T.log[0].kind=='D'&&T.log[1].kind=='D');CHECK(!_wcsicmp(T.log[0].path,folder)&&!_wcsicmp(T.log[1].path,winroot));
    CHECK(RemoveDirectoryW(folder));memset(&T,0,sizeof T);d.path_bytes=0;CHECK(!windows_directory_sync(&d,(const uint8_t *)"",winroot,NULL));CHECK(T.count==1&&!_wcsicmp(T.log[0].path,winroot));
}
static void test_initial_directory_failure_retains_children(void)
{
    DWORD errors[]={ERROR_WRITE_FAULT,ERROR_NOT_SUPPORTED,ERROR_ACCESS_DENIED};
    for(unsigned e=0;e<3;e++)for(int parent=0;parent<2;parent++){
        wchar_t src[4096],dst[4096],file[4096];fixture(src,dst,1);wcscpy(T.match,parent?winroot:dst);T.fail_nth=1;T.error=errors[e];run_test(src,1);
        CHECK(J.last.state==FPE_JOB_DONE&&J.last.errors==1&&J.events==1);join_test(file,src,L"child");CHECK(content(file));join_test(file,dst,L"child");CHECK(!present(file));
        CHECK(!count('A',0)&&!count('F',0));CHECK(count('K',(DWORD)errno_windows(errors[e]))==1);cleanup_leaf_tree(src,dst);
    }
}
static void test_final_directory_failure_retains_empty_source(void)
{
    for(int parent=0;parent<2;parent++){
        wchar_t src[4096],dst[4096];fixture(src,dst,0);wcscpy(T.match,parent?winroot:dst);T.fail_nth=2;T.error=ERROR_WRITE_FAULT;run_test(src,1);
        CHECK(J.last.state==FPE_JOB_DONE&&J.last.errors==1);CHECK(present(src)&&present(dst));CHECK(count('K',0)==1&&count('K',5)==1);cleanup_leaf_tree(src,dst);
    }
}
static void test_file_parent_failure_retains_source(void)
{
    for(int move=0;move<2;move++){
        wchar_t src[4096],dst[4096],file[4096];fixture(src,dst,1);wcscpy(T.match,dst);T.fail_nth=2;T.error=ERROR_WRITE_FAULT;run_test(src,move);
        CHECK(J.last.state==FPE_JOB_DONE&&J.last.errors==1&&J.events==1);join_test(file,src,L"child");CHECK(content(file));join_test(file,dst,L"child");CHECK(content(file));
        CHECK(count('A',5)==1&&count('A',0)==0);CHECK(count('F',0)==2);cleanup_leaf_tree(src,dst);
    }
}
static void test_final_directory_failure_retains_partial_move(void)
{
    wchar_t src[4096],dst[4096],file[4096];fixture(src,dst,1);wcscpy(T.match,dst);T.fail_nth=3;T.error=ERROR_WRITE_FAULT;run_test(src,1);
    CHECK(J.last.errors==1&&present(src));join_test(file,src,L"child");CHECK(!present(file));join_test(file,dst,L"child");CHECK(content(file));
    CHECK(count('A',0)==1&&count('K',5)==1);
    for(unsigned i=0;i<T.count;i++)if(T.log[i].kind=='A')CHECK(T.log[i].source); // removal follows ACK
    cleanup_leaf_tree(src,dst);
}
static void test_directory_metadata_error_retains_remaining_source(void)
{
    wchar_t src[4096],dst[4096],file[4096];fixture(src,dst,1);T.metadata_failure=1;run_test(src,1);
    CHECK(J.last.state==FPE_JOB_DONE&&J.last.errors==1&&J.events==1&&present(src));join_test(file,src,L"child");CHECK(!present(file));join_test(file,dst,L"child");CHECK(content(file));
    CHECK(count('K',13)==1&&count('A',0)==1);cleanup_leaf_tree(src,dst);
}
static void test_cancellation_retains_source(void)
{
    for(unsigned boundary=1;boundary<=2;boundary++){
        wchar_t src[4096],dst[4096];fixture(src,dst,0);wcscpy(T.match,dst);T.cancel_nth=boundary;T.engine=engine;run_test(src,1);
        CHECK(J.last.state==FPE_JOB_CANCELLED&&present(src));CHECK(count('K',125)==1);cleanup_leaf_tree(src,dst);
    }
}
static void test_nested_moves_and_merges(void)
{
    for(int merge=0;merge<2;merge++){
        wchar_t src[4096],dst[4096],inner[4096],outinner[4096],file[4096];fixture(src,dst,0);join_test(inner,src,L"inner");CHECK(CreateDirectoryW(inner,NULL));join_test(file,inner,L"child");write_test(file);wcscpy(T.source,file);
        join_test(outinner,dst,L"inner");if(merge){CHECK(CreateDirectoryW(dst,NULL));CHECK(CreateDirectoryW(outinner,NULL));}
        // Directory metadata is finalized before its completion barrier.
        HANDLE h=CreateFileW(inner,FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
        FILETIME time={0x12345600,0x01da0000};CHECK(h!=INVALID_HANDLE_VALUE&&SetFileTime(h,NULL,NULL,&time));if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);
        run_test(src,1);CHECK(J.last.state==FPE_JOB_DONE&&!J.last.errors&&!present(src));join_test(file,outinner,L"child");CHECK(content(file));CHECK(count('K',0)==4&&count('A',0)==1);
        unsigned ack=256;for(unsigned i=0;i<T.count;i++)if(T.log[i].kind=='A')ack=i;
        CHECK(ack>=3&&ack<T.count);if(ack>=3&&ack<T.count){CHECK(T.log[ack-1].kind=='D'&&!_wcsicmp(T.log[ack-1].path,outinner));CHECK(T.log[ack-2].kind=='F'&&T.log[ack-3].kind=='F');CHECK(T.log[ack].source);}
        h=CreateFileW(outinner,FILE_READ_ATTRIBUTES,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);FILETIME copied={0};CHECK(h!=INVALID_HANDLE_VALUE&&GetFileTime(h,NULL,NULL,&copied));
        if(!merge){if(copied.dwLowDateTime!=time.dwLowDateTime||copied.dwHighDateTime!=time.dwHighDateTime)printf("directory timestamp expected=%08lx:%08lx actual=%08lx:%08lx\n",(unsigned long)time.dwHighDateTime,(unsigned long)time.dwLowDateTime,(unsigned long)copied.dwHighDateTime,(unsigned long)copied.dwLowDateTime);CHECK(copied.dwLowDateTime==time.dwLowDateTime&&copied.dwHighDateTime==time.dwHighDateTime);}if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);
        CHECK(DeleteFileW(file));CHECK(RemoveDirectoryW(outinner));CHECK(RemoveDirectoryW(dst));
    }
}
static void test_invalid_directory_requests(void)
{
    const char *bad[]={"relative","/../escape","/.","/", "/bad:name","/bad\\..\\escape", "/\xff"};
    for(unsigned i=0;i<sizeof bad/sizeof *bad;i++){memset(&T,0,sizeof T);wslp_directory d={0,(uint32_t)strlen(bad[i])};CHECK(windows_directory_sync(&d,(const uint8_t *)bad[i],winroot,NULL)!=0);CHECK(T.count==0);}
    memset(&T,0,sizeof T);wslp_directory d={1,0};CHECK(windows_directory_sync(&d,(const uint8_t *)"",winroot,NULL)==ERROR_INVALID_DATA);d.root=0;d.path_bytes=3;CHECK(windows_directory_sync(&d,(const uint8_t *)"/\0x",winroot,NULL)==ERROR_INVALID_DATA);d.path_bytes=0;d.metadata=2;CHECK(windows_directory_sync(&d,(const uint8_t *)"",winroot,NULL)==ERROR_INVALID_DATA);d.metadata=0;d.pad=1;CHECK(windows_directory_sync(&d,(const uint8_t *)"",winroot,NULL)==ERROR_INVALID_DATA);d.pad=0;d.mtime=1;CHECK(windows_directory_sync(&d,(const uint8_t *)"",winroot,NULL)==ERROR_INVALID_DATA);CHECK(T.count==0);
}
static int junction_test(const wchar_t *alias,const wchar_t *target)
{wchar_t cmd[9000];_snwprintf(cmd,9000,L"cmd.exe /d /c mklink /J \"%ls\" \"%ls\" >nul",alias,target);return _wsystem(cmd)==0;}
static void test_root_alias_and_child_reparse(void)
{
    wchar_t real[4096],alias[4096];join_test(real,winroot,L"alias-target");join_test(alias,winroot,L"alias");CHECK(CreateDirectoryW(real,NULL));CHECK(junction_test(alias,real));
    memset(&T,0,sizeof T);wslp_directory d={0,0};CHECK(!windows_directory_sync(&d,(const uint8_t *)"",alias,NULL));CHECK(count('D',0)==1&&!_wcsicmp(T.log[0].path,real));
    memset(&T,0,sizeof T);d.path_bytes=6;CHECK(windows_directory_sync(&d,(const uint8_t *)"/alias",winroot,NULL)!=0);CHECK(T.count==0);
    CHECK(RemoveDirectoryW(alias));CHECK(RemoveDirectoryW(real));
}
static void test_protected_directory_preserves_guard(void)
{
    HANDLE guard=CreateFileW(winroot,FILE_TRAVERSE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);CHECK(guard!=INVALID_HANDLE_VALUE);
    memset(&T,0,sizeof T);wslp_directory d={0,0};CHECK(windows_directory_sync(&d,(const uint8_t *)"",winroot,NULL)==ERROR_SHARING_VIOLATION);CHECK(T.count==0);
    if(guard!=INVALID_HANDLE_VALUE)CloseHandle(guard);
}
typedef void (*test_fn)(void);
int wmain(int argc,wchar_t **argv)
{
    if(argc!=3)return 2;winroot=argv[1];lxroot=argv[2];if(!present(winroot)||!present(lxroot)||!fpe_wsl_path_kind(lxroot))return 2;
    fpe_wsl_configure(&(fpe_wsl_config){1,0,60000});engine=fpe_engine_create(NULL);CHECK(engine!=NULL);if(!engine)return 1;
    test_fn tests[]={test_real_directory_barriers,test_initial_directory_failure_retains_children,test_final_directory_failure_retains_empty_source,test_file_parent_failure_retains_source,test_final_directory_failure_retains_partial_move,test_directory_metadata_error_retains_remaining_source,test_cancellation_retains_source,test_nested_moves_and_merges,test_invalid_directory_requests,test_root_alias_and_child_reparse,test_protected_directory_preserves_guard};
    const char *names[]={"test_real_directory_barriers","test_initial_directory_failure_retains_children","test_final_directory_failure_retains_empty_source","test_file_parent_failure_retains_source","test_final_directory_failure_retains_partial_move","test_directory_metadata_error_retains_remaining_source","test_cancellation_retains_source","test_nested_moves_and_merges","test_invalid_directory_requests","test_root_alias_and_child_reparse","test_protected_directory_preserves_guard"};
    for(unsigned i=0;i<sizeof tests/sizeof *tests;i++){int before=failed;uint64_t t=GetTickCount64();tests[i]();printf("CASE %s %s %llu\n",names[i],failed==before?"passed":"failed",(unsigned long long)(GetTickCount64()-t));fflush(stdout);}
    memset(&T,0,sizeof T);fpe_engine_destroy(engine);fpe_wsl_shutdown();printf("%d passed, %d failed; 11 selectors, no skips\n",passed,failed);return failed?1:0;
}
