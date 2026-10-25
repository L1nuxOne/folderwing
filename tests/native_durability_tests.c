/* Native Windows persistence/source-removal contracts. API faults are test-only.
 * Primary: Windows native. Owner: Windows copy/move durability.
 * Impact: jobs.c, pathguard.c, internal.h, build.ps1 and Windows durability contract.
 * Requires private folders on two distinct NTFS volumes; never skips a case. */
#include "../src/internal.h"
#include <winternl.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned passed,failed;
#define CHECK(x) do{if(x)passed++;else{failed++;printf("FAIL %d %s err=%lu\n",__LINE__,#x,(unsigned long)GetLastError());}}while(0)
typedef struct observation {char kind;DWORD error,stream;int source;wchar_t path[4096];} observation;
static struct {
    observation log[512];unsigned count,file_calls,dir_hits,fail_file,fail_dir,cancel_file;
    unsigned writeouts,fail_writeout,cancel_writeout,pending_mode,wait_calls;
    int no_batch,no_ntfs,remote_drive,skip_policy,collision,collided,wrong_volume;HANDLE pending_file;
    DWORD error;wchar_t source[4096],target[4096],dir[4096],outside[4096];
    int deny_remove,restore_failure,block_restore,reparse,retargeted,mutate_source,mutate_target,mutated,check_exclusive;
    unsigned rename_requests,short_rename_requests;
    unsigned stream_queries,stream_fail_at,stream_grow;DWORD stream_error;int stream_late,stream_remove,stream_added;
    DWORD close_error;int observe_streams;unsigned active_stream,seen_streams,fail_stream,cancel_stream,fail_duplicate,fail_close,pins_live,stop_stream;
    HANDLE active_destination;
    struct {HANDLE handle;unsigned stream;} pins[64];
    struct {unsigned stream;LONGLONG size;wchar_t path[4096];} streams[32];
    fpe_engine *engine;uint64_t job;
} T;
static void handle_path(HANDLE h,wchar_t *path)
{wchar_t tmp[4096];DWORD n=GetFinalPathNameByHandleW(h,tmp,4096,0);CHECK(n&&n<4096);if(n&&n<4096)wcscpy(path,!wcsncmp(tmp,L"\\\\?\\",4)?tmp+4:tmp);else path[0]=0;}
static void record(char kind,const wchar_t *path,DWORD error)
{CHECK(T.count<512);if(T.count<512){observation *e=T.log+T.count++;e->kind=kind;e->error=error;e->source=GetFileAttributesW(T.source)!=INVALID_FILE_ATTRIBUTES;if(path)wcscpy(e->path,path);else e->path[0]=0;}}
static int reparse(HANDLE h,const wchar_t *target)
{
    struct {DWORD tag;WORD length,reserved,suboff,sublen,printoff,printlen;wchar_t path[8192];} data={0};data.tag=IO_REPARSE_TAG_MOUNT_POINT;
    _snwprintf(data.path,8192,L"\\??\\%ls",target);data.sublen=(WORD)(wcslen(data.path)*sizeof(wchar_t));data.printoff=data.sublen+sizeof(wchar_t);
    wcscpy((wchar_t*)((char*)data.path+data.printoff),target);data.printlen=(WORD)(wcslen(target)*sizeof(wchar_t));data.length=8+data.printoff+data.printlen+sizeof(wchar_t);
    DWORD done;return DeviceIoControl(h,FSCTL_SET_REPARSE_POINT,&data,data.length+8,NULL,0,&done,NULL);
}
static void add_directory_stream(void)
{
    wchar_t path[4096];_snwprintf(path,4096,L"%ls:auditdir",T.source);
    HANDLE h=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,CREATE_NEW,FILE_FLAG_BACKUP_SEMANTICS,NULL);
    CHECK(h!=INVALID_HANDLE_VALUE);if(h!=INVALID_HANDLE_VALUE){DWORD n=0;T.stream_added=WriteFile(h,"streamed",8,&n,NULL)&&n==8;CloseHandle(h);}CHECK(T.stream_added);
}
static BOOL WINAPI observed_extended(HANDLE h,FILE_INFO_BY_HANDLE_CLASS kind,LPVOID data,DWORD size)
{
    if(kind==FileStreamInfo){wchar_t path[4096];handle_path(h,path);if(!_wcsicmp(path,T.source)){
        unsigned nth=++T.stream_queries;
        if(nth==T.stream_fail_at){SetLastError(T.stream_error);return FALSE;}
        if(T.stream_grow&&size<8192){SetLastError(ERROR_MORE_DATA);return FALSE;}
    }}
    return GetFileInformationByHandleEx(h,kind,data,size);
}
static HANDLE WINAPI observed_open(LPCWSTR path,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES security,DWORD creation,DWORD flags,HANDLE template)
{
    HANDLE h=CreateFileW(path,access,share,security,creation,flags,template);
    const wchar_t *plain=!wcsncmp(path,L"\\\\?\\",4)?path+4:path;
    if(h!=INVALID_HANDLE_VALUE&&T.stream_remove&&T.stream_queries>=3&&!T.stream_added&&(access&DELETE)&&!_wcsicmp(plain,T.source))add_directory_stream();
    return h;
}
static unsigned stream_for(HANDLE h)
{for(unsigned i=0;i<64;i++)if(T.pins[i].handle==h)return T.pins[i].stream;return 0;}
static BOOL WINAPI observed_duplicate(HANDLE from,HANDLE source,HANDLE to,LPHANDLE target,DWORD access,BOOL inherit,DWORD options)
{
    unsigned stream=source==T.active_destination?T.active_stream:0;
    if(stream&&stream==T.fail_duplicate){SetLastError(T.error);return FALSE;}
    BOOL ok=DuplicateHandle(from,source,to,target,access,inherit,options);
    if(ok&&stream){unsigned i=0;while(i<64&&T.pins[i].handle)i++;CHECK(i<64);if(i<64){T.pins[i].handle=*target;T.pins[i].stream=stream;T.pins_live++;}}
    return ok;
}
static BOOL WINAPI observed_close(HANDLE h)
{
    unsigned stream=stream_for(h);
    if(stream){for(unsigned i=0;i<64;i++)if(T.pins[i].handle==h){T.pins[i].handle=NULL;T.pins_live--;break;}}
    BOOL ok=CloseHandle(h);DWORD error=ok?0:GetLastError();
    if(stream&&T.observe_streams){if(stream==T.fail_close)error=T.close_error?T.close_error:T.error;record('Q',NULL,error);T.log[T.count-1].stream=stream;}
    if(error){SetLastError(error);return FALSE;}return ok;
}
typedef struct stream_observer {LPPROGRESS_ROUTINE callback;void *user;} stream_observer;
static DWORD CALLBACK stream_progress(LARGE_INTEGER total,LARGE_INTEGER done,LARGE_INTEGER size,LARGE_INTEGER copied,DWORD stream,DWORD reason,HANDLE source,HANDLE dest,void *user)
{
    stream_observer *o=user;
    T.active_stream=stream;T.active_destination=dest;
    if(T.observe_streams){
        if(reason==CALLBACK_STREAM_SWITCH){
            CHECK(T.seen_streams<32);
            if(T.seen_streams<32){unsigned n=T.seen_streams++;T.streams[n].stream=stream;T.streams[n].size=size.QuadPart;handle_path(source,T.streams[n].path);record('B',T.streams[n].path,0);T.log[T.count-1].stream=stream;}
        }
    }
    DWORD result=o->callback(total,done,size,copied,stream,reason,source,dest,o->user);
    if(T.observe_streams&&reason==CALLBACK_STREAM_SWITCH){record('G',NULL,0);T.log[T.count-1].stream=stream;}
    T.active_stream=0;T.active_destination=NULL;
    return T.stop_stream==stream&&reason==CALLBACK_CHUNK_FINISHED?PROGRESS_STOP:result;
}
static BOOL WINAPI observed_copy(LPCWSTR source,LPCWSTR dest,LPPROGRESS_ROUTINE progress,LPVOID data,LPBOOL cancel,DWORD flags)
{stream_observer o={progress,data};return CopyFileExW(source,dest,stream_progress,&o,cancel,flags);}
static BOOL WINAPI observed_flush(HANDLE h)
{
    unsigned stream=stream_for(h);int directory=0;
    /* CopyFileEx opens named streams without FILE_READ_ATTRIBUTES. Their
     * actual callback/duplicate identity supplies file-vs-directory here. */
    if(!stream){BY_HANDLE_FILE_INFORMATION info={0};CHECK(GetFileInformationByHandle(h,&info));directory=!!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY);}
    wchar_t path[4096];handle_path(h,path);unsigned nth=directory?(!_wcsicmp(path,T.dir)?++T.dir_hits:0):++T.file_calls;
    DWORD error=0;BOOL ok;
    if(stream&&stream==T.fail_stream){error=T.error;ok=FALSE;}
    else if((directory&&nth&&nth==T.fail_dir)||(!directory&&nth==T.fail_file)){error=T.error;ok=FALSE;}
    else{ok=FlushFileBuffers(h);if(!ok)error=GetLastError();}
    record(directory?'D':'F',path,error);T.log[T.count-1].stream=stream;
    if(!directory&&nth==1&&T.collision){
        wchar_t target[4096];_snwprintf(target,4096,L"%ls\\child-00",T.target);
        HANDLE file=CreateFileW(target,GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);CHECK(file!=INVALID_HANDLE_VALUE);
        if(file!=INVALID_HANDLE_VALUE){DWORD n=0;T.collided=WriteFile(file,"foreign!",8,&n,NULL)&&n==8;CloseHandle(file);}CHECK(T.collided);
    }
    if(!directory&&nth==1&&T.check_exclusive){HANDLE other=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);CHECK(other==INVALID_HANDLE_VALUE);if(other!=INVALID_HANDLE_VALUE)CloseHandle(other);}
    if(!directory&&nth==1&&T.mutate_source){HANDLE source=CreateFileW(T.source,FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);CHECK(source!=INVALID_HANDLE_VALUE);FILETIME time={1234567,30000000};if(source!=INVALID_HANDLE_VALUE){T.mutated=SetFileTime(source,NULL,NULL,&time);CloseHandle(source);}CHECK(T.mutated);}
    if(!directory&&nth==1&&T.mutate_target){HANDLE target=CreateFileW(T.target,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);CHECK(target!=INVALID_HANDLE_VALUE);DWORD n=0;if(target!=INVALID_HANDLE_VALUE){T.mutated=WriteFile(target,"foreign!",8,&n,NULL)&&n==8;CloseHandle(target);}CHECK(T.mutated);}
    if(T.stream_late&&!T.stream_added&&((!directory&&nth==1)||(directory&&nth==2)))add_directory_stream();
    if(directory&&nth&&T.reparse&&!T.retargeted){T.retargeted=reparse(h,T.outside);CHECK(T.retargeted);}
    if(directory&&nth&&T.restore_failure)T.block_restore=1;
    if(!directory&&nth==T.cancel_file&&T.engine)fpe_job_cancel(T.engine,T.job);
    if(stream&&stream==T.cancel_stream&&T.engine)fpe_job_cancel(T.engine,T.job);
    SetLastError(error);return ok;
}
static BOOL WINAPI observed_set(HANDLE h,FILE_INFO_BY_HANDLE_CLASS kind,LPVOID data,DWORD size)
{
    if(kind==FileDispositionInfo){wchar_t path[4096];handle_path(h,path);int source=!_wcsicmp(path,T.source);
        if(source&&T.deny_remove){record('S',path,ERROR_ACCESS_DENIED);SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
        BOOL ok=SetFileInformationByHandle(h,kind,data,size);DWORD error=ok?0:GetLastError();record(source?'S':'C',path,error);SetLastError(error);return ok;}
    return SetFileInformationByHandle(h,kind,data,size);
}
typedef NTSTATUS (NTAPI *native_set_fn)(HANDLE,PIO_STATUS_BLOCK,PVOID,ULONG,FILE_INFORMATION_CLASS);
typedef NTSTATUS (NTAPI *native_create_fn)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,PLARGE_INTEGER,ULONG,ULONG,ULONG,ULONG,PVOID,ULONG);
typedef NTSTATUS (NTAPI *native_flush_fn)(HANDLE,ULONG,PVOID,ULONG,PIO_STATUS_BLOCK);
static native_set_fn native_set;static native_create_fn native_create;static native_flush_fn native_flush;
static NTSTATUS NTAPI observed_native_flush(HANDLE h,ULONG flags,PVOID parameters,ULONG bytes,PIO_STATUS_BLOCK io)
{
    CHECK(flags==2&&!parameters&&!bytes);unsigned nth=++T.writeouts;wchar_t path[4096];handle_path(h,path);
    NTSTATUS result=nth==T.fail_writeout?(NTSTATUS)0xc0000185:native_flush(h,flags,parameters,bytes,io);
    record('W',path,(DWORD)result);
    if(nth==T.cancel_writeout&&T.engine)fpe_job_cancel(T.engine,T.job);
    if(nth==1&&T.pending_mode&&result==0){T.pending_file=h;io->Status=0;return (NTSTATUS)0x103;}
    return result;
}
static DWORD WINAPI observed_wait(HANDLE h,DWORD timeout)
{
    if(T.pending_file==h&&timeout==INFINITE){T.wait_calls++;T.pending_file=NULL;record('P',L"completed test writeout",0);if(T.pending_mode==2){SetLastError(ERROR_ACCESS_DENIED);return WAIT_FAILED;}return WAIT_OBJECT_0;}
    return WaitForSingleObject(h,timeout);
}
static BOOL WINAPI observed_volume(HANDLE h,LPWSTR name,DWORD cap,LPDWORD serial,LPDWORD length,LPDWORD flags,LPWSTR fs,DWORD fs_cap)
{BOOL ok=GetVolumeInformationByHandleW(h,name,cap,serial,length,flags,fs,fs_cap);if(ok&&T.no_ntfs&&fs&&fs_cap>=5)wcscpy(fs,L"ReFS");return ok;}
static BOOL WINAPI observed_info(HANDLE h,LPBY_HANDLE_FILE_INFORMATION info)
{BOOL ok=GetFileInformationByHandle(h,info);if(ok&&T.wrong_volume&&!(info->dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))info->dwVolumeSerialNumber^=1;return ok;}
static UINT WINAPI observed_drive(LPCWSTR path)
{return T.remote_drive?DRIVE_REMOTE:GetDriveTypeW(path);}

static NTSTATUS NTAPI observed_native_set(HANDLE h,PIO_STATUS_BLOCK io,PVOID data,ULONG size,FILE_INFORMATION_CLASS kind)
{
    if(kind==10||kind==65){FILE_RENAME_INFO *r=data;T.rename_requests++;
        CHECK(size>=sizeof(FILE_RENAME_INFO));CHECK(size>=offsetof(FILE_RENAME_INFO,FileName)+r->FileNameLength);
        if(r->FileNameLength==sizeof(wchar_t))T.short_rename_requests++;
    }
    wchar_t path[4096];handle_path(h,path);NTSTATUS result=native_set(h,io,data,size,kind);record('R',path,(DWORD)result);return result;
}
static NTSTATUS NTAPI observed_native_create(PHANDLE h,ACCESS_MASK access,POBJECT_ATTRIBUTES object,PIO_STATUS_BLOCK io,PLARGE_INTEGER size,ULONG attr,ULONG share,ULONG disposition,ULONG options,PVOID ea,ULONG bytes)
{CHECK(object->RootDirectory&&object->ObjectName&&!object->ObjectName->Length);if(T.block_restore&&!(access&GENERIC_WRITE))return (NTSTATUS)0xc0000022;return native_create(h,access,object,io,size,attr,share,disposition,options,ea,bytes);}
static FARPROC WINAPI observed_lookup(HMODULE module,LPCSTR name)
{
    FARPROC result=GetProcAddress(module,name);
    if(!strcmp(name,"NtFlushBuffersFileEx")){if(T.no_batch)return NULL;union {FARPROC generic;native_flush_fn typed;} cast={.generic=result};native_flush=cast.typed;cast.typed=observed_native_flush;return cast.generic;}
    if(!strcmp(name,"NtSetInformationFile")){union {FARPROC generic;native_set_fn typed;} cast={.generic=result};native_set=cast.typed;cast.typed=observed_native_set;return cast.generic;}
    if(!strcmp(name,"NtCreateFile")){union {FARPROC generic;native_create_fn typed;} cast={.generic=result};native_create=cast.typed;cast.typed=observed_native_create;return cast.generic;}return result;
}
#define GetFileInformationByHandleEx observed_extended
#define CreateFileW observed_open
#define WaitForSingleObject observed_wait
#define GetDriveTypeW observed_drive
#define GetVolumeInformationByHandleW observed_volume
#define GetFileInformationByHandle observed_info
#define FlushFileBuffers observed_flush
#define SetFileInformationByHandle observed_set
#define GetProcAddress observed_lookup
#include "../src/pathguard.c"
#define DuplicateHandle observed_duplicate
#define CloseHandle observed_close
#define CopyFileExW observed_copy
#ifndef JOBS_SOURCE
#define JOBS_SOURCE "../src/jobs.c"
#endif
#include JOBS_SOURCE
#undef CopyFileExW
#undef CloseHandle
#undef DuplicateHandle
#undef GetFileInformationByHandleEx
#undef CreateFileW
#undef WaitForSingleObject
#undef GetDriveTypeW
#undef GetVolumeInformationByHandleW
#undef GetFileInformationByHandle
#undef FlushFileBuffers
#undef SetFileInformationByHandle
#undef GetProcAddress
static const wchar_t *root,*other;static fpe_engine *engine;static unsigned serial;
static struct {HANDLE done;fpe_job_progress last;unsigned finals,errors;} J;
typedef struct fixture {wchar_t sr[4096],dr[4096],source[4096],target[4096],file[4096],out[4096];} fixture;
static void join(wchar_t *out,const wchar_t *a,const wchar_t *b){_snwprintf(out,4096,L"%ls\\%ls",a,b);}
static int present(const wchar_t *p){return GetFileAttributesW(p)!=INVALID_FILE_ATTRIBUTES;}
static void put(const wchar_t *p,const char *data,unsigned size)
{HANDLE h=CreateFileW(p,GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);CHECK(h!=INVALID_HANDLE_VALUE);if(h!=INVALID_HANDLE_VALUE){DWORD n=0;CHECK(WriteFile(h,data,size,&n,NULL)&&n==size);CloseHandle(h);}}
static int content(const wchar_t *p,const char *data,unsigned size)
{HANDLE h=CreateFileW(p,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);if(h==INVALID_HANDLE_VALUE)return 0;char bytes[64];DWORD n=0;int ok=ReadFile(h,bytes,sizeof bytes,&n,NULL)&&n==size&&!memcmp(bytes,data,size);CloseHandle(h);return ok;}
static fixture make_fixture(int cross,int directory,int child)
{
    memset(&T,0,sizeof T);fixture f={0};wchar_t name[80];_snwprintf(name,80,L"case-%u-source",++serial);join(f.sr,root,name);_snwprintf(name,80,L"case-%u-target",serial);join(f.dr,cross?other:root,name);CHECK(CreateDirectoryW(f.sr,NULL));CHECK(CreateDirectoryW(f.dr,NULL));
    join(f.source,f.sr,directory?L"bundle":L"payload");join(f.target,f.dr,directory?L"bundle":L"payload");wcscpy(f.file,f.source);wcscpy(f.out,f.target);
    if(directory){CHECK(CreateDirectoryW(f.source,NULL));join(f.file,f.source,L"child");join(f.out,f.target,L"child");}
    if(!directory||child)put(f.file,"selected",8);wcscpy(T.source,f.source);wcscpy(T.target,f.target);return f;
}
static void progress_test(void *user,const fpe_job_progress *p){T.job=p->job;if(p->state>=FPE_JOB_DONE){J.last=*p;J.last.current=NULL;J.finals++;SetEvent(J.done);}}
static void event_test(void *user,const fpe_job_event *e){if(e->kind==FPE_JOB_ERROR)J.errors++;}
static void dump_stream_trace(void)
{
    if(!T.observe_streams)return;
    printf("STREAM_CASE source=%ls target=%ls state=%u errors=%llu last_error=%lu pins=%u\n",T.source,T.target,(unsigned)J.last.state,(unsigned long long)J.last.errors,(unsigned long)J.last.last_error,T.pins_live);
    for(unsigned i=0;i<T.seen_streams;i++)printf("STREAM id=%u bytes=%lld source=%ls\n",T.streams[i].stream,(long long)T.streams[i].size,T.streams[i].path);
    for(unsigned i=0;i<T.count;i++){observation *o=T.log+i;printf("STREAM_EVENT index=%u kind=%c id=%lu error=%lu source_present=%d path=%ls\n",i,o->kind,(unsigned long)o->stream,(unsigned long)o->error,o->source,o->path);}
}
static void run_sources(fixture *f,int move,const wchar_t *const *sources,unsigned n)
{memset(&J,0,sizeof J);J.done=CreateEventW(NULL,TRUE,FALSE,NULL);T.engine=engine;uint64_t id=fpe_job_submit_ex(engine,move?FPE_OP_MOVE:FPE_OP_COPY,sources,n,f->dr,T.skip_policy?FPE_CONFLICT_SKIP:FPE_CONFLICT_OVERWRITE,progress_test,event_test,NULL);CHECK(id);DWORD wait=WaitForSingleObject(J.done,30000);CHECK(wait==WAIT_OBJECT_0);if(wait!=WAIT_OBJECT_0){fpe_job_cancel(engine,id);CHECK(WaitForSingleObject(J.done,30000)==WAIT_OBJECT_0);}CHECK(J.finals==1);CloseHandle(J.done);dump_stream_trace();}
static void run_fixture(fixture *f,int move){const wchar_t *sources[]={f->source};run_sources(f,move,sources,1);}
static int find(char kind,unsigned nth)
{for(unsigned i=0;i<T.count;i++)if(T.log[i].kind==kind&&!--nth)return (int)i;return -1;}
static unsigned count(char kind){unsigned n=0;for(unsigned i=0;i<T.count;i++)n+=T.log[i].kind==kind;return n;}
static void no_stages(const wchar_t *dir)
{wchar_t mask[4096];join(mask,dir,L".openpilot-copy-*.tmp");WIN32_FIND_DATAW entry;HANDLE h=FindFirstFileW(mask,&entry);CHECK(h==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_FILE_NOT_FOUND);if(h!=INVALID_HANDLE_VALUE)FindClose(h);}
static void test_file_copy_flush_order(void)
{fixture f=make_fixture(0,0,1);T.check_exclusive=1;run_fixture(&f,0);CHECK(!J.last.errors&&J.last.state==FPE_JOB_DONE);CHECK(count('F')==2&&count('D')==1);int a=find('F',1),b=find('R',1),c=find('F',2),d=find('D',1);CHECK(a>=0&&a<b&&b<c&&c<d);if(a>=0&&c>=0){CHECK(wcsstr(T.log[a].path,L".openpilot-copy-")!=NULL);CHECK(!_wcsicmp(T.log[c].path,f.target));}CHECK(content(f.source,"selected",8)&&content(f.target,"selected",8));no_stages(f.dr);}
static void test_empty_and_readonly_copies(void)
{for(int readonly=0;readonly<2;readonly++){fixture f=make_fixture(0,0,1);if(!readonly){CHECK(DeleteFileW(f.source));put(f.source,"",0);}else CHECK(SetFileAttributesW(f.source,FILE_ATTRIBUTE_READONLY));run_fixture(&f,0);CHECK(!J.last.errors&&count('F')==2);CHECK(content(f.target,readonly?"selected":"",readonly?8:0));if(readonly){CHECK(GetFileAttributesW(f.target)&FILE_ATTRIBUTE_READONLY);CHECK(SetFileAttributesW(f.source,FILE_ATTRIBUTE_NORMAL));CHECK(SetFileAttributesW(f.target,FILE_ATTRIBUTE_NORMAL));}}}
static void test_prepublication_flush_errors_preserve_target(void)
{DWORD errors[]={ERROR_ACCESS_DENIED,ERROR_NOT_SUPPORTED,ERROR_DISK_FULL,ERROR_GEN_FAILURE};for(unsigned i=0;i<4;i++){fixture f=make_fixture(0,0,1);put(f.target,"previous",8);T.fail_file=1;T.error=errors[i];run_fixture(&f,0);CHECK(J.last.state==FPE_JOB_DONE&&J.last.errors==1&&J.last.last_error==errors[i]);CHECK(content(f.source,"selected",8)&&content(f.target,"previous",8));CHECK(!count('R'));no_stages(f.dr);}}
static void test_postpublication_flush_error_keeps_copy(void)
{fixture f=make_fixture(0,0,1);put(f.target,"previous",8);T.fail_file=2;T.error=ERROR_IO_DEVICE;run_fixture(&f,0);CHECK(J.last.errors==1&&J.last.last_error==ERROR_IO_DEVICE);CHECK(content(f.source,"selected",8)&&content(f.target,"selected",8));CHECK(count('F')==2&&count('C')==0);no_stages(f.dr);}
static void test_copy_directory_flush_failure_is_reported(void)
{fixture f=make_fixture(0,0,1);wchar_t later[4096],output[4096];join(later,f.sr,L"later");join(output,f.dr,L"later");put(later,"selected",8);const wchar_t *sources[]={f.source,later};wcscpy(T.dir,f.dr);T.fail_dir=1;T.error=ERROR_NOT_SUPPORTED;run_sources(&f,0,sources,2);CHECK(J.last.state==FPE_JOB_DONE&&J.last.errors==1);CHECK(content(f.source,"selected",8)&&content(f.target,"selected",8));CHECK(content(later,"selected",8)&&!present(output));}
static void test_cancel_before_and_after_publication(void)
{for(unsigned phase=1;phase<=2;phase++){fixture f=make_fixture(0,0,1);put(f.target,"previous",8);T.cancel_file=phase;run_fixture(&f,0);CHECK(J.last.state==FPE_JOB_CANCELLED&&!J.last.errors);CHECK(content(f.source,"selected",8)&&content(f.target,phase==1?"previous":"selected",8));no_stages(f.dr);}}
static void test_flush_error_precedes_cancel(void)
{fixture f=make_fixture(0,0,1);T.fail_file=T.cancel_file=1;T.error=ERROR_DISK_FULL;run_fixture(&f,0);CHECK(J.last.errors==1&&J.last.last_error==ERROR_DISK_FULL&&J.last.state==FPE_JOB_DONE);CHECK(content(f.source,"selected",8)&&!present(f.target));}
static void test_cross_volume_move_order(void)
{fixture f=make_fixture(1,0,1);run_fixture(&f,1);CHECK(!J.last.errors&&J.last.state==FPE_JOB_DONE);CHECK(!present(f.source)&&content(f.target,"selected",8));int file=find('F',2),remove=find('S',1);CHECK(file>=0&&remove>file);if(remove>=0){CHECK(remove>0&&T.log[remove-1].source);CHECK(remove>0&&T.log[remove-1].kind=='D'&&!_wcsicmp(T.log[remove-1].path,f.dr));CHECK((unsigned)(remove+1)<T.count&&T.log[remove+1].kind=='D'&&!T.log[remove+1].source&&!_wcsicmp(T.log[remove+1].path,f.sr));}no_stages(f.dr);}
static void test_cross_volume_file_errors_retain_source(void)
{for(unsigned phase=1;phase<=2;phase++){fixture f=make_fixture(1,0,1);put(f.target,"previous",8);T.fail_file=phase;T.error=ERROR_GEN_FAILURE;run_fixture(&f,1);CHECK(J.last.errors==1&&content(f.source,"selected",8));CHECK(content(f.target,phase==1?"previous":"selected",8));CHECK(!count('S'));no_stages(f.dr);}}
static void test_cross_volume_destination_fence_retains_source(void)
{fixture f=make_fixture(1,0,1);wcscpy(T.dir,f.dr);T.fail_dir=2;T.error=ERROR_NOT_SUPPORTED;run_fixture(&f,1);CHECK(J.last.errors==1&&content(f.source,"selected",8)&&content(f.target,"selected",8));CHECK(!count('S'));}
static void test_source_parent_error_after_unlink_keeps_destination(void)
{fixture f=make_fixture(1,0,1);wcscpy(T.dir,f.sr);T.fail_dir=2;T.error=ERROR_IO_DEVICE;run_fixture(&f,1);CHECK(J.last.errors==1&&J.last.last_error==ERROR_IO_DEVICE);CHECK(!present(f.source)&&content(f.target,"selected",8));CHECK(count('S')==1);}
static void test_source_removal_error_keeps_both(void)
{fixture f=make_fixture(1,0,1);T.deny_remove=1;run_fixture(&f,1);CHECK(J.last.errors==1&&J.last.last_error==ERROR_ACCESS_DENIED);CHECK(content(f.source,"selected",8)&&content(f.target,"selected",8));}
static void test_same_volume_rename_namespace_order(void)
{fixture f=make_fixture(0,0,1);BY_HANDLE_FILE_INFORMATION before,after;HANDLE h=CreateFileW(f.source,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);CHECK(GetFileInformationByHandle(h,&before));CloseHandle(h);run_fixture(&f,1);CHECK(!J.last.errors&&!present(f.source)&&content(f.target,"selected",8));CHECK(count('D')==4&&!count('F')&&!count('S'));int rename=find('R',1);CHECK(rename==2&&T.log[rename+1].kind=='D'&&T.log[rename+2].kind=='D');h=CreateFileW(f.target,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);CHECK(GetFileInformationByHandle(h,&after));CloseHandle(h);CHECK(before.nFileIndexHigh==after.nFileIndexHigh&&before.nFileIndexLow==after.nFileIndexLow);}
static void test_rename_fence_errors_keep_correct_name(void)
{for(unsigned phase=1;phase<=2;phase++){fixture f=make_fixture(0,0,1);wcscpy(T.dir,f.dr);T.fail_dir=phase;T.error=ERROR_IO_DEVICE;run_fixture(&f,1);CHECK(J.last.errors==1&&J.last.state==FPE_JOB_DONE);CHECK(phase==1?content(f.source,"selected",8)&&!present(f.target):!present(f.source)&&content(f.target,"selected",8));if(phase==2)CHECK(count('D')==4);}}
static void test_folder_copy_final_fences(void)
{fixture f=make_fixture(0,1,1);run_fixture(&f,0);CHECK(!J.last.errors&&content(f.file,"selected",8)&&content(f.out,"selected",8));CHECK(count('F')==2&&count('D')==4);CHECK(T.log[T.count-2].kind=='D'&&!_wcsicmp(T.log[T.count-2].path,f.target));CHECK(T.log[T.count-1].kind=='D'&&!_wcsicmp(T.log[T.count-1].path,f.dr));}
static void test_initial_folder_error_prevents_descent(void)
{fixture f=make_fixture(0,1,1);wcscpy(T.dir,f.target);T.fail_dir=1;T.error=ERROR_NOT_SUPPORTED;run_fixture(&f,0);CHECK(J.last.errors==1&&content(f.file,"selected",8)&&!present(f.out)&&!count('F'));}
static void test_final_folder_error_retains_remaining_source(void)
{for(int child=0;child<2;child++){fixture f=make_fixture(1,1,child);wcscpy(T.dir,f.target);T.fail_dir=child?3:2;T.error=ERROR_GEN_FAILURE;run_fixture(&f,1);CHECK(J.last.errors==1&&present(f.source)&&present(f.target));if(child)CHECK(!present(f.file)&&content(f.out,"selected",8));}}
static void test_merge_move_removes_only_after_final_fences(void)
{fixture f=make_fixture(0,1,1);CHECK(CreateDirectoryW(f.target,NULL));run_fixture(&f,1);CHECK(!J.last.errors&&!present(f.source)&&content(f.out,"selected",8));int remove=find('S',1);CHECK(remove>=2);if(remove>=2){CHECK(T.log[remove-1].kind=='D'&&!_wcsicmp(T.log[remove-1].path,f.dr));CHECK(T.log[remove-2].kind=='D'&&!_wcsicmp(T.log[remove-2].path,f.target));}}
static void test_late_source_and_target_changes_are_rejected(void)
{fixture f=make_fixture(1,0,1);put(f.target,"previous",8);T.mutate_source=1;run_fixture(&f,1);CHECK(T.mutated&&J.last.errors==1&&J.last.last_error==ERROR_FILE_INVALID);CHECK(content(f.source,"selected",8)&&content(f.target,"previous",8));no_stages(f.dr);
 f=make_fixture(0,0,1);put(f.target,"previous",8);T.mutate_target=1;run_fixture(&f,0);CHECK(T.mutated&&J.last.errors==1&&J.last.last_error==ERROR_FILE_INVALID);CHECK(content(f.source,"selected",8)&&content(f.target,"foreign!",8));no_stages(f.dr);}
static void test_reparse_and_restore_failure_abort_walk(void)
{for(int restore=0;restore<2;restore++){fixture f=make_fixture(0,1,1);wcscpy(T.dir,f.target);T.restore_failure=restore;T.reparse=!restore;join(T.outside,f.dr,L"outside");CHECK(CreateDirectoryW(T.outside,NULL));wchar_t sentinel[4096];join(sentinel,T.outside,L"sentinel");put(sentinel,"previous",8);run_fixture(&f,0);CHECK(J.last.errors&&content(f.file,"selected",8)&&!count('F'));CHECK(content(sentinel,"previous",8));if(!restore){CHECK(T.retargeted);CHECK(RemoveDirectoryW(f.target));}}
 fixture f=make_fixture(0,0,1);wcscpy(T.dir,f.dr);T.restore_failure=1;run_fixture(&f,0);CHECK(J.last.state==FPE_JOB_DONE&&J.last.errors==1);CHECK(content(f.source,"selected",8)&&content(f.target,"selected",8));}
static fixture batch_fixture(unsigned files)
{
    fixture f=make_fixture(0,1,0);wchar_t name[64],path[4096];
    for(unsigned i=0;i<files;i++){_snwprintf(name,64,L"child-%02u",i);join(path,f.source,name);put(path,"selected",8);}
    return f;
}
static unsigned batch_content(fixture *f,unsigned files,int source)
{
    unsigned n=0;wchar_t name[64],path[4096];
    for(unsigned i=0;i<files;i++){_snwprintf(name,64,L"child-%02u",i);join(path,source?f->source:f->target,name);n+=content(path,"selected",8);}
    return n;
}
static void check_batch_order(void)
{
    unsigned staged=0,published=0,after=0,fences=0;int ready=0;
    for(unsigned i=0;i<T.count;i++){
        observation *e=T.log+i;
        if(e->kind=='W'&&wcsstr(e->path,L".openpilot-copy-")){CHECK(!ready);staged++;CHECK(staged<=WIN_COPY_BATCH_FILES);}
        if(e->kind=='F'){
            if(wcsstr(e->path,L".openpilot-copy-")){CHECK(!ready&&staged&&staged<=WIN_COPY_BATCH_FILES);ready=1;fences++;}
            else{CHECK(ready&&published==staged&&after==published);ready=0;staged=published=after=0;fences++;}
        }
        if(e->kind=='R'){CHECK(ready&&e->error==0);published++;CHECK(published<=staged);}
        if(e->kind=='W'&&!wcsstr(e->path,L".openpilot-copy-")){CHECK(ready&&after<published);after++;}
        if(e->kind=='D'&&fences)CHECK(!ready&&!staged);
    }
    CHECK(!ready&&!staged&&fences);
}
static void test_batch_order_bounds_and_tail(void)
{
    fixture f=batch_fixture(69);T.check_exclusive=1;run_fixture(&f,0);
    CHECK(J.last.state==FPE_JOB_DONE&&!J.last.errors&&J.last.items_done==70&&J.last.bytes_done==69*8);
    CHECK(count('W')==138&&count('F')==6&&count('R')==69&&count('D')==4);
    CHECK(batch_content(&f,69,1)==69&&batch_content(&f,69,0)==69);check_batch_order();no_stages(f.target);
}
static void test_batch_writeout_failure_prevents_publication(void)
{
    fixture f=batch_fixture(64);T.fail_writeout=3;run_fixture(&f,0);
    CHECK(J.last.state==FPE_JOB_DONE&&J.last.errors==1&&J.last.last_error==ERROR_IO_DEVICE);
    CHECK(count('W')==3&&!count('F')&&!count('R'));CHECK(batch_content(&f,64,1)==64&&!batch_content(&f,64,0));no_stages(f.target);
}
static void test_batch_prepublication_fence_failure_stops_selections(void)
{
    fixture f=batch_fixture(64);wchar_t later[4096],out[4096];join(later,f.sr,L"later");join(out,f.dr,L"later");put(later,"selected",8);
    const wchar_t *sources[]={f.source,later};T.fail_file=1;T.error=ERROR_NOT_SUPPORTED;run_sources(&f,0,sources,2);
    CHECK(J.last.state==FPE_JOB_DONE&&J.last.errors==1&&J.last.last_error==ERROR_NOT_SUPPORTED);
    CHECK(count('W')==32&&count('F')==1&&!count('R'));CHECK(batch_content(&f,64,1)==64&&!batch_content(&f,64,0));CHECK(!present(out)&&content(later,"selected",8));no_stages(f.target);
}
static void test_batch_postpublication_writeout_failure_keeps_prefix(void)
{
    fixture f=batch_fixture(64);T.fail_writeout=33;run_fixture(&f,0);
    CHECK(J.last.state==FPE_JOB_DONE&&J.last.errors==1&&J.last.last_error==ERROR_IO_DEVICE);
    CHECK(count('W')==33&&count('F')==2&&count('R')==1);CHECK(batch_content(&f,64,1)==64&&batch_content(&f,64,0)==1);no_stages(f.target);
}
static void test_batch_postpublication_fence_failure_keeps_batch(void)
{
    fixture f=batch_fixture(64);T.fail_file=2;T.error=ERROR_DISK_FULL;run_fixture(&f,0);
    CHECK(J.last.state==FPE_JOB_DONE&&J.last.errors==1&&J.last.last_error==ERROR_DISK_FULL);
    CHECK(count('W')==64&&count('F')==2&&count('R')==32);CHECK(batch_content(&f,64,1)==64&&batch_content(&f,64,0)==32);no_stages(f.target);
}
static void test_batch_cancellation_across_commit_phases(void)
{
    for(unsigned phase=0;phase<4;phase++){
        fixture f=batch_fixture(64);if(phase==0)T.cancel_writeout=3;if(phase==1)T.cancel_file=1;if(phase==2)T.cancel_writeout=33;if(phase==3)T.cancel_file=2;
        run_fixture(&f,0);unsigned expected=phase<2?0:phase==2?1:32;
        CHECK(J.last.state==FPE_JOB_CANCELLED&&!J.last.errors);CHECK(batch_content(&f,64,1)==64&&batch_content(&f,64,0)==expected);
        CHECK(count('F')==(phase==0?0:phase==1?1:2));no_stages(f.target);
    }
}
static void test_batch_error_precedes_cancellation(void)
{
    fixture f=batch_fixture(64);T.fail_file=T.cancel_file=2;T.error=ERROR_ACCESS_DENIED;run_fixture(&f,0);
    CHECK(J.last.state==FPE_JOB_DONE&&J.last.errors==1&&J.last.last_error==ERROR_ACCESS_DENIED);
    CHECK(batch_content(&f,64,1)==64&&batch_content(&f,64,0)==32);no_stages(f.target);
}
static void test_batch_late_target_is_never_replaced(void)
{
    fixture f=batch_fixture(32);T.collision=1;run_fixture(&f,0);wchar_t out[4096];join(out,f.target,L"child-00");
    CHECK(T.collided&&J.last.state==FPE_JOB_DONE&&J.last.errors==1);CHECK(content(out,"foreign!",8)&&batch_content(&f,32,1)==32);CHECK(batch_content(&f,32,0)<32);no_stages(f.target);
}
static void test_batch_missing_api_uses_full_file_barriers(void)
{
    fixture f=batch_fixture(32);T.no_batch=1;run_fixture(&f,0);
    CHECK(!J.last.errors&&count('F')==64&&!count('W'));CHECK(batch_content(&f,32,1)==32&&batch_content(&f,32,0)==32);no_stages(f.target);
}
static void test_batch_unsupported_filesystem_uses_full_file_barriers(void)
{
    fixture f=batch_fixture(32);T.no_ntfs=1;run_fixture(&f,0);
    CHECK(!J.last.errors&&count('F')==64&&!count('W'));CHECK(batch_content(&f,32,1)==32&&batch_content(&f,32,0)==32);no_stages(f.target);
}
static void test_batch_replacement_flushes_pending_stages_first(void)
{
    fixture f=batch_fixture(32);CHECK(CreateDirectoryW(f.target,NULL));wchar_t out[4096];join(out,f.target,L"child-17");put(out,"previous",8);run_fixture(&f,0);
    CHECK(!J.last.errors&&count('W')==62&&count('F')>=4);CHECK(batch_content(&f,32,1)==32&&batch_content(&f,32,0)==32);no_stages(f.target);
}
static void test_batch_nested_directories_finish_their_own_fences(void)
{
    fixture f=batch_fixture(32);wchar_t source[4096],dir[4096],output[4096];join(dir,f.source,L"nested");CHECK(CreateDirectoryW(dir,NULL));join(source,dir,L"leaf");put(source,"selected",8);join(dir,f.target,L"nested");join(output,dir,L"leaf");run_fixture(&f,0);
    CHECK(!J.last.errors&&count('W')==64&&count('F')==4&&count('D')==8);CHECK(content(source,"selected",8)&&content(output,"selected",8));CHECK(batch_content(&f,32,0)==32);no_stages(f.target);
}
static void test_batch_final_directory_failure_preserves_copies(void)
{
    fixture f=batch_fixture(64);wcscpy(T.dir,f.target);T.fail_dir=2;T.error=ERROR_IO_DEVICE;run_fixture(&f,0);
    CHECK(J.last.state==FPE_JOB_DONE&&J.last.errors==1&&J.last.last_error==ERROR_IO_DEVICE);CHECK(count('W')==128&&count('F')==4&&count('D')==4);CHECK(batch_content(&f,64,1)==64&&batch_content(&f,64,0)==64);no_stages(f.target);
}
static void test_batch_pending_writeout_requires_completed_wait(void)
{
    for(unsigned mode=1;mode<=2;mode++){
        fixture f=batch_fixture(32);T.pending_mode=mode;run_fixture(&f,0);CHECK(T.wait_calls==1&&count('P')==1);
        if(mode==1){CHECK(!J.last.errors&&batch_content(&f,32,0)==32);CHECK(find('P',1)<find('F',1)&&find('F',1)<find('R',1));}
        else CHECK(J.last.errors==1&&J.last.last_error==ERROR_ACCESS_DENIED&&!count('R')&&!batch_content(&f,32,0));
        CHECK(batch_content(&f,32,1)==32);no_stages(f.target);
    }
}
static void test_batch_wrong_volume_is_rejected_before_writeout(void)
{
    fixture f=batch_fixture(32);T.wrong_volume=1;run_fixture(&f,0);
    CHECK(J.last.state==FPE_JOB_DONE&&J.last.errors==1&&J.last.last_error==ERROR_NOT_SAME_DEVICE);CHECK(!count('W')&&!count('R'));CHECK(batch_content(&f,32,1)==32&&!batch_content(&f,32,0));no_stages(f.target);
}

static void test_batch_empty_and_readonly_files_keep_metadata(void)
{
    fixture f=batch_fixture(32);wchar_t empty[4096],ro[4096],out[4096];join(empty,f.source,L"child-00");CHECK(DeleteFileW(empty));put(empty,"",0);join(ro,f.source,L"child-01");CHECK(SetFileAttributesW(ro,FILE_ATTRIBUTE_READONLY));run_fixture(&f,0);
    CHECK(!J.last.errors&&count('W')==64&&count('F')==2);join(out,f.target,L"child-00");CHECK(content(empty,"",0)&&content(out,"",0));join(out,f.target,L"child-01");CHECK(GetFileAttributesW(out)&FILE_ATTRIBUTE_READONLY);CHECK(content(ro,"selected",8)&&content(out,"selected",8));CHECK(batch_content(&f,32,0)==31);no_stages(f.target);
}
static void test_batch_extra_stream_uses_ordinary_writer(void)
{
    fixture f=batch_fixture(32);wchar_t source[4096],out[4096];join(source,f.source,L"child-17:extra");put(source,"streamed",8);run_fixture(&f,0);join(out,f.target,L"child-17:extra");
    CHECK(!J.last.errors&&count('W')==62&&count('F')>=4);CHECK(content(source,"streamed",8)&&content(out,"streamed",8));CHECK(batch_content(&f,32,1)==32&&batch_content(&f,32,0)==32);CHECK(!T.pins_live);
    int named=-1,published=-1;for(unsigned i=0;i<T.count;i++)if(T.log[i].kind=='F'&&T.log[i].stream>1)named=(int)i;
    CHECK(named>=0);if(named>=0){wchar_t base[4096];wcscpy(base,T.log[named].path);wchar_t *colon=wcschr(base+2,L':');CHECK(colon!=NULL);if(colon)*colon=0;
        for(unsigned i=0;i<T.count;i++)if(T.log[i].kind=='R'&&!T.log[i].error&&!_wcsicmp(base,T.log[i].path))published=(int)i;CHECK(published>named&&!T.log[named].error);}
    no_stages(f.target);
}
static void test_batch_large_file_retains_ordinary_barriers(void)
{
    fixture f=batch_fixture(32);wchar_t source[4096],out[4096];join(source,f.source,L"large");unsigned size=WIN_COPY_BATCH_SMALL+1;char *bytes=malloc(size);CHECK(bytes);if(!bytes)return;memset(bytes,0x5a,size);put(source,bytes,size);free(bytes);run_fixture(&f,0);join(out,f.target,L"large");
    CHECK(!J.last.errors&&count('W')==64&&count('F')==4);CHECK(J.last.bytes_done==32*8+size&&J.last.items_done==34);CHECK(batch_content(&f,32,1)==32&&batch_content(&f,32,0)==32);
    HANDLE file=CreateFileW(out,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);CHECK(file!=INVALID_HANDLE_VALUE);if(file!=INVALID_HANDLE_VALUE){char data[4096];DWORD n;unsigned total=0;int valid=1;while(ReadFile(file,data,sizeof data,&n,NULL)&&n){for(DWORD i=0;i<n;i++)if(data[i]!=0x5a)valid=0;total+=n;}CHECK(valid&&total==size);CloseHandle(file);}no_stages(f.target);
}

static void test_batch_remote_drive_uses_full_file_barriers(void)
{
    fixture f=batch_fixture(32);T.remote_drive=1;run_fixture(&f,0);
    CHECK(!J.last.errors&&count('F')==64&&!count('W'));CHECK(batch_content(&f,32,1)==32&&batch_content(&f,32,0)==32);no_stages(f.target);
}

static void test_batch_skipped_bytes_survive_late_directory_error(void)
{
    fixture f=batch_fixture(32);CHECK(CreateDirectoryW(f.target,NULL));wchar_t out[4096];join(out,f.target,L"child-17");put(out,"previous",8);T.skip_policy=1;wcscpy(T.dir,f.target);T.fail_dir=2;T.error=ERROR_IO_DEVICE;run_fixture(&f,0);
    CHECK(J.last.errors==1&&J.last.skipped==1&&J.last.bytes_done==32*8&&J.last.items_done==33);CHECK(count('F')==4&&count('W')==62);CHECK(content(out,"previous",8)&&batch_content(&f,32,1)==32&&batch_content(&f,32,0)==31);no_stages(f.target);
}

static void test_directory_streams_refuse_before_descent(void)
{
    for(int route=0;route<5;route++)for(int child=0;child<2;child++)for(int empty=0;empty<2;empty++){
        int move=route>=2,merge=route==1||route==3||route==4;
        fixture f=make_fixture(route==2||route==3,1,child);wchar_t stream[4096],target_stream[4096];
        _snwprintf(stream,4096,L"%ls:auditdir",f.source);put(stream,"streamed",empty?0:8);
        if(merge){CHECK(CreateDirectoryW(f.target,NULL));_snwprintf(target_stream,4096,L"%ls:existing",f.target);put(target_stream,"previous",8);}
        run_fixture(&f,move);
        CHECK(J.last.errors==1&&J.last.last_error==ERROR_NOT_SUPPORTED);CHECK(present(f.source)&&content(stream,"streamed",empty?0:8));
        CHECK(child?content(f.file,"selected",8):!present(f.file));CHECK(!present(f.out)&&!count('F')&&!count('S'));
        CHECK(merge?content(target_stream,"previous",8):!present(f.target));
    }
}
static void test_directory_streams_added_during_children_retain_source(void)
{
    for(int route=0;route<4;route++){
        fixture f=make_fixture(route==1||route==3,1,1);int move=route!=0;
        if(route>=2)CHECK(CreateDirectoryW(f.target,NULL));wcscpy(T.dir,f.target);T.stream_late=1;run_fixture(&f,move);
        wchar_t stream[4096];_snwprintf(stream,4096,L"%ls:auditdir",f.source);
        CHECK(T.stream_added&&J.last.errors==1&&J.last.last_error==ERROR_NOT_SUPPORTED);CHECK(present(f.source)&&content(stream,"streamed",8));
        CHECK(content(f.out,"selected",8));CHECK(move?!present(f.file):content(f.file,"selected",8));
    }
}
static void test_directory_streams_rechecked_on_removal_handle(void)
{
    for(int merge=0;merge<2;merge++){
        fixture f=make_fixture(!merge,1,0);if(merge)CHECK(CreateDirectoryW(f.target,NULL));
        T.stream_remove=1;run_fixture(&f,1);wchar_t stream[4096];_snwprintf(stream,4096,L"%ls:auditdir",f.source);
        CHECK(T.stream_added&&J.last.errors==1&&J.last.last_error==ERROR_NOT_SUPPORTED);CHECK(present(f.source)&&content(stream,"streamed",8)&&present(f.target));CHECK(!count('S'));
    }
}
static void test_directory_stream_inventory_failures_keep_source(void)
{
    DWORD errors[]={ERROR_ACCESS_DENIED,ERROR_NOT_SUPPORTED,ERROR_IO_DEVICE,ERROR_DIR_NOT_EMPTY};
    for(int merge=0;merge<2;merge++)for(unsigned phase=1;phase<=4;phase++)for(unsigned e=0;e<4;e++){
        fixture f=make_fixture(!merge,1,0);if(merge)CHECK(CreateDirectoryW(f.target,NULL));
        T.stream_fail_at=phase;T.stream_error=errors[e];run_fixture(&f,1);
        CHECK(T.stream_queries==phase&&J.last.errors==1&&J.last.last_error==errors[e]);CHECK(present(f.source)&&!count('S'));
        if(phase==1&&!merge)CHECK(!present(f.target));
    }
}
static void test_directory_stream_inventory_growth_and_destination_streams(void)
{
    for(int move=0;move<2;move++){
        fixture f=make_fixture(move,1,1);CHECK(CreateDirectoryW(f.target,NULL));wchar_t stream[4096];_snwprintf(stream,4096,L"%ls:existing",f.target);put(stream,"previous",8);
        T.stream_grow=1;run_fixture(&f,move);CHECK(!J.last.errors&&T.stream_queries>=4);CHECK(content(stream,"previous",8)&&content(f.out,"selected",8));CHECK(move?!present(f.source):present(f.source));
    }
}
static void test_directory_rename_retains_own_streams(void)
{
    fixture f=make_fixture(0,1,1);wchar_t stream[4096],out[4096];_snwprintf(stream,4096,L"%ls:auditdir",f.source);put(stream,"streamed",8);
    _snwprintf(out,4096,L"%ls:auditdir",f.target);run_fixture(&f,1);
    CHECK(!J.last.errors&&!present(f.source)&&content(out,"streamed",8)&&content(f.out,"selected",8));CHECK(!T.stream_queries&&!count('F')&&!count('S'));
}

static fixture short_fixture(int cross,int directory,const wchar_t *leaf)
{
    fixture f=make_fixture(cross,directory,0);
    CHECK(directory?RemoveDirectoryW(f.source):DeleteFileW(f.source));
    join(f.source,f.sr,leaf);join(f.target,f.dr,leaf);wcscpy(f.file,f.source);wcscpy(f.out,f.target);
    if(directory){CHECK(CreateDirectoryW(f.source,NULL));join(f.file,f.source,L"b");join(f.out,f.target,L"b");}
    put(f.file,"selected",8);wcscpy(T.source,f.source);wcscpy(T.target,f.target);return f;
}
static void test_rename_request_minimum_size_and_utf16_lengths(void)
{
    wchar_t longest[256];for(unsigned i=0;i<255;i++)longest[i]=L'x';longest[255]=0;
    const wchar_t *names[]={L"a",L"\x03bb",L"\xd83d\xde80",L"abc",longest};
    for(unsigned i=0;i<5;i++)for(int relative=0;relative<2;relative++)for(int replace=0;replace<2;replace++){
        fixture f=make_fixture(0,0,1);join(f.target,relative?f.dr:f.sr,names[i]);
        wchar_t *target=NULL;CHECK(!fpe_path_nt(f.target,&target));if(!target)continue;
        if(replace)put(target,"previous",8);
        HANDLE parent=relative?CreateFileW(f.dr,FILE_TRAVERSE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL):NULL;
        CHECK(!relative||parent!=INVALID_HANDLE_VALUE);
        HANDLE h=CreateFileW(f.source,DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
        CHECK(h!=INVALID_HANDLE_VALUE);BY_HANDLE_FILE_INFORMATION before,after;CHECK(GetFileInformationByHandle(h,&before));
        DWORD error=fpe_win_rename_handle(h,parent,names[i],replace,0);
        CHECK(!error&&content(target,"selected",8)&&!present(f.source));CHECK(GetFileInformationByHandle(h,&after));
        CHECK(before.nFileIndexHigh==after.nFileIndexHigh&&before.nFileIndexLow==after.nFileIndexLow);
        CHECK(T.rename_requests==1&&T.short_rename_requests==(wcslen(names[i])==1?1u:0u));
        CHECK(CloseHandle(h));if(relative)CHECK(CloseHandle(parent));fpe_free(target);
    }
}
static void test_single_unit_file_copy_move_and_hardlink_sources(void)
{
    const wchar_t *names[]={L"a",L"\x03bb"};
    for(unsigned i=0;i<2;i++)for(int route=0;route<6;route++)for(int linked=0;linked<2;linked++){
        int move=route>=2,replace=route&1;fixture f=short_fixture(route==2||route==3,0,names[i]);
        wchar_t alias[4096],stream[4096],out[4096];join(alias,f.sr,L"z");
        if(linked)CHECK(CreateHardLinkW(alias,f.source,NULL));
        _snwprintf(stream,4096,L"%ls:extra",f.source);_snwprintf(out,4096,L"%ls:extra",f.target);put(stream,"streamed",8);
        if(replace){put(f.target,"previous",8);CHECK(SetFileAttributesW(f.target,FILE_ATTRIBUTE_READONLY));}
        run_fixture(&f,move);CHECK(!J.last.errors&&J.last.state==FPE_JOB_DONE);
        CHECK(content(f.target,"selected",8)&&content(out,"streamed",8));CHECK(move?!present(f.source):content(f.source,"selected",8));
        if(linked)CHECK(content(alias,"selected",8));CHECK(T.short_rename_requests==((route==2||route==3)?2u:1u));no_stages(f.dr);
        if(present(f.target))CHECK(SetFileAttributesW(f.target,FILE_ATTRIBUTE_NORMAL));
    }
}
static void test_single_unit_directory_copy_and_move(void)
{
    for(int route=0;route<3;route++){
        fixture f=short_fixture(route==1,1,L"a");run_fixture(&f,route!=0);
        CHECK(!J.last.errors&&content(f.out,"selected",8));CHECK(route?!present(f.source):content(f.file,"selected",8));
        CHECK(T.short_rename_requests==(route==1?2u:1u));no_stages(f.dr);
    }
}
static void test_single_unit_batch_publication(void)
{
    fixture f=make_fixture(0,1,0);wchar_t name[2]={0},path[4096];
    for(unsigned i=0;i<32;i++){name[0]=i<26?L'a'+i:L'0'+i-26;join(path,f.source,name);put(path,"selected",8);}
    run_fixture(&f,0);CHECK(!J.last.errors&&count('W')==64&&T.short_rename_requests==32);
    for(unsigned i=0;i<32;i++){name[0]=i<26?L'a'+i:L'0'+i-26;join(path,f.source,name);CHECK(content(path,"selected",8));join(path,f.target,name);CHECK(content(path,"selected",8));}
    no_stages(f.target);
}
static void test_single_unit_selected_copy_publication(void)
{
    fixture f=make_fixture(0,1,0);CHECK(CreateDirectoryW(f.target,NULL));wchar_t a[4096],b[4096],da[4096],db[4096];
    join(a,f.source,L"a");join(b,f.source,L"\x03bb");join(da,f.target,L"a");join(db,f.target,L"\x03bb");put(a,"selected",8);put(b,"selected",8);put(db,"previous",8);
    memset(&J,0,sizeof J);J.done=CreateEventW(NULL,TRUE,FALSE,NULL);const wchar_t *paths[]={L"a",L"\x03bb"};
    uint64_t id=fpe_job_submit_tree(engine,f.source,paths,2,f.target,FPE_CONFLICT_OVERWRITE,progress_test,event_test,NULL);CHECK(id);
    DWORD wait=WaitForSingleObject(J.done,30000);CHECK(wait==WAIT_OBJECT_0);if(wait!=WAIT_OBJECT_0){fpe_job_cancel(engine,id);CHECK(WaitForSingleObject(J.done,30000)==WAIT_OBJECT_0);}CHECK(J.finals==1);CloseHandle(J.done);
    CHECK(!J.last.errors&&content(a,"selected",8)&&content(b,"selected",8)&&content(da,"selected",8)&&content(db,"selected",8));CHECK(T.short_rename_requests==2);no_stages(f.target);
}

/* Stream ordinals come from the real CopyFileEx callbacks; named stream paths
 * come from their actual source handles. Duplicate tracking proves each flush
 * addresses the corresponding retained destination stream, not the base file. */
static void add_streams(fixture *f)
{
    wchar_t p[4096];_snwprintf(p,4096,L"%ls:alpha",f->source);put(p,"alpha bytes",11);
    _snwprintf(p,4096,L"%ls:empty",f->source);put(p,"",0);
    _snwprintf(p,4096,L"%ls:\x03bb",f->source);put(p,"unicode stream",14);T.observe_streams=1;
}
static void stream_bytes(const wchar_t *base)
{
    wchar_t p[4096];CHECK(content(base,"selected",8));_snwprintf(p,4096,L"%ls:alpha",base);CHECK(content(p,"alpha bytes",11));
    _snwprintf(p,4096,L"%ls:empty",base);CHECK(content(p,"",0));_snwprintf(p,4096,L"%ls:\x03bb",base);CHECK(content(p,"unicode stream",14));
}
static int stream_event(char kind,unsigned stream)
{for(unsigned i=0;i<T.count;i++)if(T.log[i].kind==kind&&T.log[i].stream==stream)return (int)i;return -1;}
static int publication_event(void)
{for(unsigned i=0;i<T.count;i++)if(T.log[i].kind=='R'&&!T.log[i].error)return (int)i;return -1;}
static unsigned publication_count(void)
{unsigned n=0;for(unsigned i=0;i<T.count;i++)n+=T.log[i].kind=='R'&&!T.log[i].error;return n;}
static void checked_stream_order(void)
{
    CHECK(T.seen_streams==4);int renamed=publication_event();CHECK(renamed>=0);unsigned named=0,empty=0;
    for(unsigned i=0;i<T.seen_streams;i++){
        unsigned id=T.streams[i].stream;int flush=stream_event('F',id);CHECK(flush>=0&&flush<renamed);
        if(flush>=0)CHECK(!T.log[flush].error&&T.log[flush].source);
        if(i+1<T.seen_streams){CHECK(flush>stream_event('B',T.streams[i+1].stream));CHECK(flush<stream_event('G',T.streams[i+1].stream));}
        if(id>1){int closed=stream_event('Q',id);CHECK(closed>flush&&closed<renamed);named++;empty+=T.streams[i].size==0;CHECK(wcsstr(T.streams[i].path,L":alpha")||wcsstr(T.streams[i].path,L":empty")||wcsstr(T.streams[i].path,L":\x03bb"));}
    }
    CHECK(named==3&&empty==1);CHECK(!T.pins_live);
}
static void test_named_streams_flush_each_handle_before_publication(void)
{
    for(int move=0;move<2;move++)for(int replace=0;replace<2;replace++){
        fixture f=make_fixture(move,0,1);add_streams(&f);if(replace){put(f.target,"previous",8);CHECK(SetFileAttributesW(f.target,FILE_ATTRIBUTE_READONLY));}
        CHECK(SetFileAttributesW(f.source,FILE_ATTRIBUTE_READONLY));run_fixture(&f,move);
        CHECK(!J.last.errors&&J.last.state==FPE_JOB_DONE);checked_stream_order();stream_bytes(f.target);CHECK(GetFileAttributesW(f.target)&FILE_ATTRIBUTE_READONLY);
        if(move){CHECK(!present(f.source));CHECK(find('S',1)>publication_event());}else stream_bytes(f.source);no_stages(f.dr);
    }
}
static void test_empty_final_named_stream_gets_checked_barrier(void)
{
    for(int fail=0;fail<2;fail++){
        fixture f=make_fixture(1,0,1);wchar_t p[4096];_snwprintf(p,4096,L"%ls:empty",f.source);put(p,"",0);T.observe_streams=1;if(fail){T.fail_stream=2;T.error=ERROR_IO_DEVICE;}run_fixture(&f,1);
        CHECK(T.seen_streams==2&&T.streams[1].size==0);int flush=stream_event('F',2);CHECK(flush>=0&&!T.pins_live);
        if(fail){CHECK(J.last.errors==1&&J.last.last_error==ERROR_IO_DEVICE&&!publication_count()&&!count('S'));CHECK(content(f.source,"selected",8)&&content(p,"",0)&&!present(f.target));}
        else{CHECK(!J.last.errors&&!present(f.source));CHECK(flush<publication_event()&&stream_event('Q',2)<publication_event());_snwprintf(p,4096,L"%ls:empty",f.target);CHECK(content(f.target,"selected",8)&&content(p,"",0));}no_stages(f.dr);
    }
}
static void test_named_stream_barrier_failures_retain_sources_and_targets(void)
{
    DWORD errors[]={ERROR_DISK_FULL,ERROR_ACCESS_DENIED,ERROR_NOT_SUPPORTED,ERROR_IO_DEVICE};
    for(unsigned error=0;error<4;error++)for(unsigned stream=1;stream<=4;stream++){
        fixture f=make_fixture(1,0,1);add_streams(&f);put(f.target,"previous",8);T.fail_stream=stream;T.error=errors[error];run_fixture(&f,1);
        CHECK(J.last.errors==1&&J.last.last_error==errors[error]&&J.last.state==FPE_JOB_DONE);CHECK(!publication_count()&&!count('S')&&!T.pins_live);
        int flush=stream_event('F',stream);CHECK(flush>=0);if(flush>=0)CHECK(T.log[flush].error==errors[error]);stream_bytes(f.source);CHECK(content(f.target,"previous",8));no_stages(f.dr);
    }
}
static void test_named_stream_duplicate_and_close_failures_stop_publication(void)
{
    for(int close=0;close<2;close++)for(unsigned stream=2;stream<=4;stream++){
        fixture f=make_fixture(1,0,1);add_streams(&f);put(f.target,"previous",8);T.error=ERROR_ACCESS_DENIED;
        if(close)T.fail_close=stream;else T.fail_duplicate=stream;run_fixture(&f,1);
        CHECK(J.last.errors==1&&J.last.last_error==ERROR_ACCESS_DENIED);CHECK(!publication_count()&&!count('S')&&!T.pins_live);stream_bytes(f.source);CHECK(content(f.target,"previous",8));no_stages(f.dr);
    }
}
static void test_named_stream_cancellation_preserves_unpublished_target(void)
{
    for(unsigned stream=1;stream<=4;stream++){
        fixture f=make_fixture(1,0,1);add_streams(&f);put(f.target,"previous",8);T.cancel_stream=stream;run_fixture(&f,1);
        CHECK(J.last.state==FPE_JOB_CANCELLED&&!J.last.errors);CHECK(!publication_count()&&!count('S')&&!T.pins_live);stream_bytes(f.source);CHECK(content(f.target,"previous",8));no_stages(f.dr);
    }
}
static void test_named_stream_flush_error_precedes_cancel_and_close(void)
{
    fixture f=make_fixture(1,0,1);add_streams(&f);T.fail_stream=T.cancel_stream=T.fail_close=4;T.error=ERROR_DISK_FULL;T.close_error=ERROR_ACCESS_DENIED;run_fixture(&f,1);
    CHECK(J.last.errors==1&&J.last.last_error==ERROR_DISK_FULL&&J.last.state==FPE_JOB_DONE);CHECK(!publication_count()&&!count('S')&&!T.pins_live);stream_bytes(f.source);CHECK(!present(f.target));no_stages(f.dr);
}
static void test_copy_failure_closes_unfinished_named_stream_without_commit(void)
{
    fixture f=make_fixture(1,0,1);add_streams(&f);put(f.target,"previous",8);T.stop_stream=2;run_fixture(&f,1);
    CHECK(J.last.errors==1&&J.last.last_error==ERROR_REQUEST_ABORTED&&J.last.state==FPE_JOB_DONE);CHECK(!publication_count()&&!count('S')&&!T.pins_live);
    CHECK(stream_event('Q',2)>=0&&stream_event('F',2)<0);stream_bytes(f.source);CHECK(content(f.target,"previous",8));no_stages(f.dr);
}
static void test_named_stream_failure_stops_later_sources(void)
{
    fixture f=make_fixture(1,0,1);add_streams(&f);wchar_t later[4096],out[4096];join(later,f.sr,L"later");join(out,f.dr,L"later");put(later,"selected",8);T.fail_stream=4;T.error=ERROR_IO_DEVICE;
    const wchar_t *sources[]={f.source,later};run_sources(&f,1,sources,2);CHECK(J.last.errors==1&&J.last.last_error==ERROR_IO_DEVICE);CHECK(!publication_count()&&!count('S')&&!T.pins_live);
    stream_bytes(f.source);CHECK(content(later,"selected",8)&&!present(out)&&!present(f.target));no_stages(f.dr);
}
static void test_selected_copy_checks_each_named_stream(void)
{
    for(int fail=0;fail<2;fail++){
        fixture f=make_fixture(0,0,1);add_streams(&f);put(f.target,"previous",8);if(fail){T.fail_stream=4;T.error=ERROR_IO_DEVICE;}
        memset(&J,0,sizeof J);J.done=CreateEventW(NULL,TRUE,FALSE,NULL);T.engine=engine;const wchar_t *paths[]={L"payload"};
        uint64_t id=fpe_job_submit_tree(engine,f.sr,paths,1,f.dr,FPE_CONFLICT_OVERWRITE,progress_test,event_test,NULL);CHECK(id);DWORD wait=WaitForSingleObject(J.done,30000);CHECK(wait==WAIT_OBJECT_0);if(wait!=WAIT_OBJECT_0){fpe_job_cancel(engine,id);CHECK(WaitForSingleObject(J.done,30000)==WAIT_OBJECT_0);}CloseHandle(J.done);CHECK(J.finals==1);dump_stream_trace();
        stream_bytes(f.source);CHECK(!T.pins_live);if(fail){CHECK(J.last.errors==1&&J.last.last_error==ERROR_IO_DEVICE);CHECK(content(f.target,"previous",8)&&!publication_count());}else{CHECK(!J.last.errors);checked_stream_order();stream_bytes(f.target);}no_stages(f.dr);
    }
}

typedef void (*case_fn)(void);
int wmain(int argc,wchar_t **argv)
{
    if(argc!=3)return 2;root=argv[1];other=argv[2];wchar_t a[4096],b[4096];DWORD va=0,vb=0;
    CHECK(GetVolumePathNameW(root,a,4096)&&GetVolumePathNameW(other,b,4096));CHECK(GetVolumeInformationW(a,NULL,0,&va,NULL,NULL,NULL,0)&&GetVolumeInformationW(b,NULL,0,&vb,NULL,NULL,NULL,0));CHECK(va!=vb);if(failed)return 2;
    engine=fpe_engine_create(NULL);CHECK(engine);if(!engine)return 1;
    case_fn tests[]={test_copy_failure_closes_unfinished_named_stream_without_commit,test_named_stream_failure_stops_later_sources,test_empty_final_named_stream_gets_checked_barrier,test_named_streams_flush_each_handle_before_publication,test_named_stream_barrier_failures_retain_sources_and_targets,test_named_stream_duplicate_and_close_failures_stop_publication,test_named_stream_cancellation_preserves_unpublished_target,test_named_stream_flush_error_precedes_cancel_and_close,test_selected_copy_checks_each_named_stream,test_rename_request_minimum_size_and_utf16_lengths,test_single_unit_file_copy_move_and_hardlink_sources,test_single_unit_directory_copy_and_move,test_single_unit_batch_publication,test_single_unit_selected_copy_publication,test_directory_streams_refuse_before_descent,test_directory_streams_added_during_children_retain_source,test_directory_streams_rechecked_on_removal_handle,test_directory_stream_inventory_failures_keep_source,test_directory_stream_inventory_growth_and_destination_streams,test_directory_rename_retains_own_streams,test_file_copy_flush_order,test_empty_and_readonly_copies,test_prepublication_flush_errors_preserve_target,test_postpublication_flush_error_keeps_copy,test_copy_directory_flush_failure_is_reported,test_cancel_before_and_after_publication,test_flush_error_precedes_cancel,test_cross_volume_move_order,test_cross_volume_file_errors_retain_source,test_cross_volume_destination_fence_retains_source,test_source_parent_error_after_unlink_keeps_destination,test_source_removal_error_keeps_both,test_same_volume_rename_namespace_order,test_rename_fence_errors_keep_correct_name,test_folder_copy_final_fences,test_initial_folder_error_prevents_descent,test_final_folder_error_retains_remaining_source,test_merge_move_removes_only_after_final_fences,test_late_source_and_target_changes_are_rejected,test_reparse_and_restore_failure_abort_walk,test_batch_order_bounds_and_tail,test_batch_writeout_failure_prevents_publication,test_batch_prepublication_fence_failure_stops_selections,test_batch_postpublication_writeout_failure_keeps_prefix,test_batch_postpublication_fence_failure_keeps_batch,test_batch_cancellation_across_commit_phases,test_batch_error_precedes_cancellation,test_batch_late_target_is_never_replaced,test_batch_missing_api_uses_full_file_barriers,test_batch_unsupported_filesystem_uses_full_file_barriers,test_batch_replacement_flushes_pending_stages_first,test_batch_nested_directories_finish_their_own_fences,test_batch_final_directory_failure_preserves_copies,test_batch_pending_writeout_requires_completed_wait,test_batch_wrong_volume_is_rejected_before_writeout,test_batch_empty_and_readonly_files_keep_metadata,test_batch_extra_stream_uses_ordinary_writer,test_batch_large_file_retains_ordinary_barriers,test_batch_remote_drive_uses_full_file_barriers,test_batch_skipped_bytes_survive_late_directory_error};
    const char *names[]={"test_copy_failure_closes_unfinished_named_stream_without_commit","test_named_stream_failure_stops_later_sources","test_empty_final_named_stream_gets_checked_barrier","test_named_streams_flush_each_handle_before_publication","test_named_stream_barrier_failures_retain_sources_and_targets","test_named_stream_duplicate_and_close_failures_stop_publication","test_named_stream_cancellation_preserves_unpublished_target","test_named_stream_flush_error_precedes_cancel_and_close","test_selected_copy_checks_each_named_stream","test_rename_request_minimum_size_and_utf16_lengths","test_single_unit_file_copy_move_and_hardlink_sources","test_single_unit_directory_copy_and_move","test_single_unit_batch_publication","test_single_unit_selected_copy_publication","test_directory_streams_refuse_before_descent","test_directory_streams_added_during_children_retain_source","test_directory_streams_rechecked_on_removal_handle","test_directory_stream_inventory_failures_keep_source","test_directory_stream_inventory_growth_and_destination_streams","test_directory_rename_retains_own_streams","test_file_copy_flush_order","test_empty_and_readonly_copies","test_prepublication_flush_errors_preserve_target","test_postpublication_flush_error_keeps_copy","test_copy_directory_flush_failure_is_reported","test_cancel_before_and_after_publication","test_flush_error_precedes_cancel","test_cross_volume_move_order","test_cross_volume_file_errors_retain_source","test_cross_volume_destination_fence_retains_source","test_source_parent_error_after_unlink_keeps_destination","test_source_removal_error_keeps_both","test_same_volume_rename_namespace_order","test_rename_fence_errors_keep_correct_name","test_folder_copy_final_fences","test_initial_folder_error_prevents_descent","test_final_folder_error_retains_remaining_source","test_merge_move_removes_only_after_final_fences","test_late_source_and_target_changes_are_rejected","test_reparse_and_restore_failure_abort_walk","test_batch_order_bounds_and_tail","test_batch_writeout_failure_prevents_publication","test_batch_prepublication_fence_failure_stops_selections","test_batch_postpublication_writeout_failure_keeps_prefix","test_batch_postpublication_fence_failure_keeps_batch","test_batch_cancellation_across_commit_phases","test_batch_error_precedes_cancellation","test_batch_late_target_is_never_replaced","test_batch_missing_api_uses_full_file_barriers","test_batch_unsupported_filesystem_uses_full_file_barriers","test_batch_replacement_flushes_pending_stages_first","test_batch_nested_directories_finish_their_own_fences","test_batch_final_directory_failure_preserves_copies","test_batch_pending_writeout_requires_completed_wait","test_batch_wrong_volume_is_rejected_before_writeout","test_batch_empty_and_readonly_files_keep_metadata","test_batch_extra_stream_uses_ordinary_writer","test_batch_large_file_retains_ordinary_barriers","test_batch_remote_drive_uses_full_file_barriers","test_batch_skipped_bytes_survive_late_directory_error"};
    for(unsigned i=0;i<sizeof tests/sizeof *tests;i++){unsigned before=failed;uint64_t started=GetTickCount64();tests[i]();printf("CASE %s %s %llu\n",names[i],before==failed?"passed":"failed",(unsigned long long)(GetTickCount64()-started));fflush(stdout);}
    fpe_engine_destroy(engine);fpe_wsl_shutdown();printf("%u passed, %u failed; 60 native durability selectors\n",passed,failed);return failed?1:0;
}
