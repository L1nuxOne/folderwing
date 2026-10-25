/* Permanent-delete identity and lifetime contracts. Faults alter observations
 * only; all real deletion controls are confined to the caller's owned root. */
#include "../src/internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <winioctl.h>
static unsigned passed,failed,serial;
#define CHECK(x) do{if(x)passed++;else{failed++;printf("FAIL %d %s err=%lu\n",__LINE__,#x,(unsigned long)GetLastError());}}while(0)
static fpe_engine *engine;
static struct {
    wchar_t match[4096];int running,identity,kind,created,parent,enumeration,unsupported;
    int cancel_start,cancel_after,cancel_scan,deny_remove,check_pins,probe_done,add_late;
    unsigned dispositions,events,finals,observations,path_removals;
    HANDLE done;uint64_t job;fpe_job_progress last;
} T;
static int matches(HANDLE h)
{
    wchar_t p[4096];DWORD n=GetFinalPathNameByHandleW(h,p,4096,0);
    return n&&n<4096&&!_wcsicmp(!wcsncmp(p,L"\\\\?\\",4)?p+4:p,T.match);
}
static BOOL WINAPI observed_info(HANDLE h,FILE_INFO_BY_HANDLE_CLASS kind,LPVOID data,DWORD size)
{
    if(T.unsupported&&kind==FileIdInfo){SetLastError(ERROR_NOT_SUPPORTED);return FALSE;}
    BOOL ok=GetFileInformationByHandleEx(h,kind,data,size);if(!ok)return ok;
    if(T.running&&matches(h)){
        if(kind==FileIdInfo&&T.identity){((FILE_ID_INFO *)data)->FileId.Identifier[T.identity==2?15:0]^=1;T.observations++;}
        if(kind==FileAttributeTagInfo&&T.kind){((FILE_ATTRIBUTE_TAG_INFO *)data)->ReparseTag=IO_REPARSE_TAG_MOUNT_POINT;T.observations++;}
    }
    if(T.enumeration&&(kind==FileIdBothDirectoryRestartInfo||kind==FileIdBothDirectoryInfo)){
        FILE_ID_BOTH_DIR_INFO *at=data;for(;;){unsigned n=at->FileNameLength/2;
            if(n&&at->FileName[0]!=L'.'){at->FileId.QuadPart^=1;T.observations++;break;}
            if(!at->NextEntryOffset)break;at=(void *)((char *)at+at->NextEntryOffset);
        }
    }
    if(T.cancel_scan&&(kind==FileIdBothDirectoryRestartInfo||kind==FileIdBothDirectoryInfo))fpe_job_cancel(engine,T.job);
    return ok;
}
static BOOL WINAPI observed_basic(HANDLE h,LPBY_HANDLE_FILE_INFORMATION data)
{
    BOOL ok=GetFileInformationByHandle(h,data);
    if(ok&&T.running&&matches(h)){
        if(T.kind){data->dwFileAttributes|=FILE_ATTRIBUTE_REPARSE_POINT;T.observations++;}
        if(T.created){data->ftCreationTime.dwLowDateTime^=1;T.observations++;}
        if(T.parent){data->nFileIndexLow^=1;T.observations++;}
    }
    return ok;
}
static BOOL WINAPI observed_set(HANDLE h,FILE_INFO_BY_HANDLE_CLASS kind,LPVOID data,DWORD size)
{
    if(kind==FileDispositionInfo){
        T.dispositions++;
        if(T.check_pins&&!T.probe_done){
            T.probe_done=1;wchar_t path[4096];DWORD n=GetFinalPathNameByHandleW(h,path,4096,0);CHECK(n&&n<4096);
            HANDLE other=CreateFileW(path,DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
            CHECK(other==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION);if(other!=INVALID_HANDLE_VALUE)CloseHandle(other);
            wchar_t *last=wcsrchr(path,L'\\');CHECK(last!=NULL);if(last){*last=0;other=CreateFileW(path,DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);CHECK(other==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION);if(other!=INVALID_HANDLE_VALUE)CloseHandle(other);}
        }
        if(T.deny_remove){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
        BOOL ok=SetFileInformationByHandle(h,kind,data,size);
        if(ok&&T.cancel_after)fpe_job_cancel(engine,T.job);return ok;
    }
    return SetFileInformationByHandle(h,kind,data,size);
}
static BOOL WINAPI observed_delete(LPCWSTR path){T.path_removals++;return DeleteFileW(path);}
static BOOL WINAPI observed_rmdir(LPCWSTR path){T.path_removals++;return RemoveDirectoryW(path);}
#define GetFileInformationByHandleEx observed_info
#define GetFileInformationByHandle observed_basic
#define SetFileInformationByHandle observed_set
#define DeleteFileW observed_delete
#define RemoveDirectoryW observed_rmdir
#ifndef JOBS_SOURCE
#define JOBS_SOURCE "../src/jobs.c"
#endif
#include JOBS_SOURCE
#undef GetFileInformationByHandleEx
#undef GetFileInformationByHandle
#undef SetFileInformationByHandle
#undef DeleteFileW
#undef RemoveDirectoryW
#include "jobwait.h"
static const wchar_t *root;
static void join(wchar_t *p,const wchar_t *a,const wchar_t *b){_snwprintf(p,4096,L"%ls\\%ls",a,b);p[4095]=0;}
static void put(const wchar_t *p)
{HANDLE h=CreateFileW(p,GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);CHECK(h!=INVALID_HANDLE_VALUE);if(h!=INVALID_HANDLE_VALUE){DWORD n;CHECK(WriteFile(h,"selected",8,&n,NULL)&&n==8);CloseHandle(h);}}
static int contents(const wchar_t *p)
{HANDLE h=CreateFileW(p,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);if(h==INVALID_HANDLE_VALUE)return 0;char b[9];DWORD n;int ok=ReadFile(h,b,9,&n,NULL)&&n==8&&!memcmp(b,"selected",8);CloseHandle(h);return ok;}
static int present(const wchar_t *p){return GetFileAttributesW(p)!=INVALID_FILE_ATTRIBUTES;}
static void reset(void){memset(&T,0,sizeof T);}
static void fixture(wchar_t *p)
{wchar_t name[64];_snwprintf(name,64,L"case-%u",++serial);join(p,root,name);CHECK(CreateDirectoryW(p,NULL));}
static void progress(void *user,const fpe_job_progress *v)
{
    T.job=v->job;
    if(v->state==FPE_JOB_RUNNING&&!T.running){T.running=1;
        if(T.add_late){wchar_t p[4096];join(p,T.match,L"late");put(p);}
        if(T.cancel_start)fpe_job_cancel(engine,v->job);
    }
    if(v->state>=FPE_JOB_DONE){T.last=*v;T.last.current=NULL;T.finals++;SetEvent(T.done);}
}
static void event(void *user,const fpe_job_event *e){if(e->kind==FPE_JOB_ERROR)T.events++;}
static void run_case(const wchar_t *p)
{
    T.done=test_callback_event(TRUE);const wchar_t *src[]={p};
    uint64_t id=fpe_job_submit_ex(engine,FPE_OP_DELETE,src,1,NULL,FPE_CONFLICT_FAIL,progress,event,NULL);
    CHECK(id!=0);CHECK(test_wait_job(engine,id,T.done,30000)==WAIT_OBJECT_0);CloseHandle(T.done);
    CHECK(T.finals==1);CHECK(T.path_removals==0);
}
static void test_delete_normal_tree_readonly_and_pins(void)
{
    reset();wchar_t p[4096],sub[4096],file[4096];fixture(p);join(sub,p,L"child");CHECK(CreateDirectoryW(sub,NULL));join(file,sub,L"readonly");put(file);CHECK(SetFileAttributesW(file,FILE_ATTRIBUTE_READONLY));
    T.check_pins=1;run_case(p);CHECK(!T.last.errors&&T.last.state==FPE_JOB_DONE);CHECK(T.dispositions==3&&T.probe_done);CHECK(!present(p));
}
static void test_delete_identity_kind_and_version_refusal(void)
{
    for(int kind=0;kind<4;kind++)for(int child=0;child<2;child++){
        reset();wchar_t p[4096],file[4096];fixture(p);join(file,p,L"selected");put(file);wcscpy(T.match,child?file:p);
        T.identity=kind==0?1:kind==3?2:0;T.kind=kind==1;T.created=kind==2;run_case(p);
        CHECK(T.last.errors&&T.events&&T.observations);CHECK(contents(file));CHECK(T.dispositions==(child?1u:0u));CHECK(present(p));CHECK(DeleteFileW(file)&&RemoveDirectoryW(p));
    }
}
static void test_delete_changed_child_directory_is_preserved(void)
{
    reset();wchar_t p[4096],sub[4096],file[4096];fixture(p);join(sub,p,L"child");CHECK(CreateDirectoryW(sub,NULL));join(file,sub,L"selected");put(file);wcscpy(T.match,sub);T.identity=1;run_case(p);
    CHECK(T.last.errors&&T.observations&&T.dispositions==1);CHECK(contents(file));CHECK(DeleteFileW(file)&&RemoveDirectoryW(sub)&&RemoveDirectoryW(p));
}
static void test_delete_explicit_operand_keeps_windows_case_lookup(void)
{
    reset();wchar_t p[4096],file[4096],operand[4096];fixture(p);join(file,p,L"MiXeD");put(file);join(operand,p,L"mixed");run_case(operand);
    CHECK(!T.last.errors&&T.dispositions==1&&!present(file));CHECK(RemoveDirectoryW(p));
}
static void test_delete_parent_identity_refusal(void)
{
    reset();wchar_t p[4096],file[4096];fixture(p);join(file,p,L"selected");put(file);wcscpy(T.match,p);T.parent=1;run_case(file);
    CHECK(T.last.errors&&T.observations&&!T.dispositions);CHECK(contents(file));CHECK(DeleteFileW(file)&&RemoveDirectoryW(p));
}
static void test_delete_enumerated_identity_refusal(void)
{
    reset();wchar_t p[4096],file[4096];fixture(p);join(file,p,L"selected");put(file);T.enumeration=1;run_case(p);
    CHECK(T.observations&&T.last.errors&&!T.dispositions);CHECK(contents(file));CHECK(DeleteFileW(file)&&RemoveDirectoryW(p));
}
static void test_delete_unsupported_identity_preserves_source(void)
{
    reset();wchar_t p[4096],file[4096];fixture(p);join(file,p,L"selected");put(file);T.unsupported=1;run_case(p);
    CHECK(T.last.errors&&T.last.last_error==ERROR_NOT_SUPPORTED&&!T.dispositions);CHECK(contents(file));CHECK(DeleteFileW(file)&&RemoveDirectoryW(p));
}
static void test_delete_new_children_are_preserved(void)
{
    reset();wchar_t p[4096],file[4096];fixture(p);wcscpy(T.match,p);T.add_late=1;run_case(p);join(file,p,L"late");
    CHECK(T.last.errors&&T.last.last_error==ERROR_DIR_NOT_EMPTY);CHECK(contents(file));CHECK(DeleteFileW(file)&&RemoveDirectoryW(p));
}
static void test_delete_cancellation_and_handle_cleanup(void)
{
    for(int after=0;after<2;after++){
        reset();wchar_t p[4096],file[4096];fixture(p);join(file,p,L"selected");put(file);T.cancel_start=!after;T.cancel_after=after;run_case(p);
        CHECK(T.last.state==FPE_JOB_CANCELLED&&!T.last.errors);CHECK(T.dispositions==(after?1u:0u));CHECK(present(p));CHECK(after?!present(file):contents(file));
        if(!after)CHECK(DeleteFileW(file));CHECK(RemoveDirectoryW(p));
    }
}
static void test_delete_scan_cancel_preserves_tree(void)
{
    reset();wchar_t p[4096],file[4096];fixture(p);join(file,p,L"selected");put(file);T.cancel_scan=1;run_case(p);
    CHECK(T.last.state==FPE_JOB_CANCELLED&&!T.last.errors&&!T.dispositions);CHECK(contents(file));CHECK(DeleteFileW(file)&&RemoveDirectoryW(p));
}
static void test_delete_failed_disposition_restores_readonly(void)
{
    reset();wchar_t p[4096],file[4096];fixture(p);join(file,p,L"selected");put(file);CHECK(SetFileAttributesW(file,FILE_ATTRIBUTE_READONLY));T.deny_remove=1;run_case(file);
    CHECK(T.last.errors&&T.last.last_error==ERROR_ACCESS_DENIED&&T.dispositions==1);CHECK(contents(file));CHECK(GetFileAttributesW(file)&FILE_ATTRIBUTE_READONLY);
    CHECK(SetFileAttributesW(file,FILE_ATTRIBUTE_NORMAL)&&DeleteFileW(file)&&RemoveDirectoryW(p));
}
static int junction(const wchar_t *alias,const wchar_t *target)
{wchar_t cmd[9000];_snwprintf(cmd,9000,L"cmd.exe /d /c mklink /J \"%ls\" \"%ls\" >nul",alias,target);return _wsystem(cmd)==0;}
static void test_delete_static_junction_is_leaf(void)
{
    for(int nested=0;nested<2;nested++){
        reset();wchar_t p[4096],target[4096],file[4096],alias[4096];fixture(p);fixture(target);join(file,target,L"sentinel");put(file);join(alias,p,L"link");CHECK(junction(alias,target));
        run_case(nested?p:alias);CHECK(!T.last.errors);CHECK(!present(alias)&&contents(file));CHECK(T.dispositions==(nested?2u:1u));
        if(!nested)CHECK(RemoveDirectoryW(p));CHECK(DeleteFileW(file)&&RemoveDirectoryW(target));
    }
}
static void test_delete_explicit_parent_alias_binds_physical_root(void)
{
    reset();wchar_t p[4096],alias[4096],file[4096],through[4096];fixture(p);join(alias,root,L"parent-alias");CHECK(junction(alias,p));join(file,p,L"selected");put(file);join(through,alias,L"selected");run_case(through);
    CHECK(!T.last.errors&&!present(file)&&T.dispositions==1);CHECK(RemoveDirectoryW(alias)&&RemoveDirectoryW(p));
}
int wmain(int argc,wchar_t **argv)
{
    wchar_t owned[4096];
    if(argc==1){DWORD n=GetTempPathW(4096,owned);if(!n||n>3900)return 2;_snwprintf(owned+n,4096-n,L"openpilot-delete-%lu-%llu",(unsigned long)GetCurrentProcessId(),(unsigned long long)GetTickCount64());if(!CreateDirectoryW(owned,NULL))return 2;root=owned;printf("Owned fixture retained: %ls\n",root);}
    else if(argc==2)root=argv[1];else return 2;engine=fpe_engine_create(NULL);if(!engine)return 2;
    typedef void (*fn)(void);fn cases[]={test_delete_scan_cancel_preserves_tree,test_delete_changed_child_directory_is_preserved,test_delete_explicit_operand_keeps_windows_case_lookup,test_delete_normal_tree_readonly_and_pins,test_delete_identity_kind_and_version_refusal,test_delete_parent_identity_refusal,test_delete_enumerated_identity_refusal,test_delete_unsupported_identity_preserves_source,test_delete_new_children_are_preserved,test_delete_cancellation_and_handle_cleanup,test_delete_failed_disposition_restores_readonly,test_delete_static_junction_is_leaf,test_delete_explicit_parent_alias_binds_physical_root};
    const char *names[]={"test_delete_scan_cancel_preserves_tree","test_delete_changed_child_directory_is_preserved","test_delete_explicit_operand_keeps_windows_case_lookup","test_delete_normal_tree_readonly_and_pins","test_delete_identity_kind_and_version_refusal","test_delete_parent_identity_refusal","test_delete_enumerated_identity_refusal","test_delete_unsupported_identity_preserves_source","test_delete_new_children_are_preserved","test_delete_cancellation_and_handle_cleanup","test_delete_failed_disposition_restores_readonly","test_delete_static_junction_is_leaf","test_delete_explicit_parent_alias_binds_physical_root"};
    for(unsigned i=0;i<sizeof cases/sizeof *cases;i++){unsigned before=failed;uint64_t start=GetTickCount64();cases[i]();printf("CASE %s %s %llu\n",names[i],failed==before?"passed":"failed",(unsigned long long)(GetTickCount64()-start));fflush(stdout);}
    fpe_engine_destroy(engine);fpe_wsl_shutdown();printf("%u passed, %u failed; 13 delete selectors\n",passed,failed);return failed?1:0;
}
