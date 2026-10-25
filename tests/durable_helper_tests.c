/* Primary: engine integration. Owner: privileged Windows durability boundary.
 * Exercise the real helper's object checks without granting a general file API. */
#define wmain durable_program_main
#include "../tools/fpdurable.c"
#undef wmain
#include <stdio.h>
#include <winioctl.h>
static unsigned passed,failed;static const wchar_t *root;
#define CHECK(x) do{if(x)passed++;else{failed++;printf("FAIL %d %s err=%lu\n",__LINE__,#x,(unsigned long)GetLastError());}}while(0)
static HANDLE pin(const wchar_t *path)
{return CreateFileW(path,FILE_READ_ATTRIBUTES|FILE_TRAVERSE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);}
static void test_durable_helper_flushes_only_captured_directory(void)
{
    HANDLE h=pin(root);CHECK(h!=INVALID_HANDLE_VALUE);fpd_message q={.handle=(uintptr_t)h};CHECK(!identity(h,&q));CHECK(!flush(GetCurrentProcess(),&q));CloseHandle(h);
}
static void test_durable_helper_rejects_wrong_identity_and_invalid_handle(void)
{
    HANDLE h=pin(root);CHECK(h!=INVALID_HANDLE_VALUE);fpd_message q={.handle=(uintptr_t)h};CHECK(!identity(h,&q));q.low^=1;CHECK(flush(GetCurrentProcess(),&q)==ERROR_FILE_INVALID);CloseHandle(h);q.handle=0;CHECK(flush(GetCurrentProcess(),&q)==ERROR_INVALID_HANDLE);
}
static void test_durable_helper_rejects_file_and_pipe_objects(void)
{
    wchar_t path[4096];_snwprintf(path,4096,L"%ls\\helper-sentinel",root);HANDLE h=CreateFileW(path,GENERIC_READ|GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);CHECK(h!=INVALID_HANDLE_VALUE);
    DWORD n;CHECK(WriteFile(h,"preserved",9,&n,NULL)&&n==9);fpd_message q={.handle=(uintptr_t)h};CHECK(flush(GetCurrentProcess(),&q)==ERROR_CANT_ACCESS_FILE);
    CHECK(SetFilePointer(h,0,NULL,FILE_BEGIN)==0);char bytes[9];CHECK(ReadFile(h,bytes,9,&n,NULL)&&n==9&&!memcmp(bytes,"preserved",9));CloseHandle(h);CHECK(DeleteFileW(path));
    HANDLE read,write;CHECK(CreatePipe(&read,&write,NULL,0));q.handle=(uintptr_t)read;CHECK(flush(GetCurrentProcess(),&q)==ERROR_NOT_SUPPORTED);CloseHandle(read);CloseHandle(write);
}
static void test_durable_helper_respects_external_write_barrier(void)
{
    HANDLE h=pin(root),barrier=CreateFileW(root,FILE_READ_ATTRIBUTES|FILE_TRAVERSE,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    CHECK(h!=INVALID_HANDLE_VALUE&&barrier!=INVALID_HANDLE_VALUE);fpd_message q={.handle=(uintptr_t)h};CHECK(!identity(h,&q));CHECK(flush(GetCurrentProcess(),&q)==ERROR_SHARING_VIOLATION);CloseHandle(barrier);CHECK(!flush(GetCurrentProcess(),&q));CloseHandle(h);
}
static void test_durable_helper_rejects_directory_reparse_object(void)
{
    wchar_t path[4096];_snwprintf(path,4096,L"%ls\\helper-junction",root);CHECK(CreateDirectoryW(path,NULL));
    HANDLE h=CreateFileW(path,GENERIC_WRITE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);CHECK(h!=INVALID_HANDLE_VALUE);
    struct {DWORD tag;WORD length,reserved,suboff,sublen,printoff,printlen;wchar_t path[8192];} data={0};data.tag=IO_REPARSE_TAG_MOUNT_POINT;
    _snwprintf(data.path,8192,L"\\??\\%ls",root);data.sublen=(WORD)(wcslen(data.path)*sizeof(wchar_t));data.printoff=data.sublen+sizeof(wchar_t);
    wcscpy((wchar_t*)((char*)data.path+data.printoff),root);data.printlen=(WORD)(wcslen(root)*sizeof(wchar_t));data.length=8+data.printoff+data.printlen+sizeof(wchar_t);
    DWORD done;CHECK(DeviceIoControl(h,FSCTL_SET_REPARSE_POINT,&data,data.length+8,NULL,0,&done,NULL));
    fpd_message q={.handle=(uintptr_t)h};CHECK(flush(GetCurrentProcess(),&q)==ERROR_CANT_ACCESS_FILE);CloseHandle(h);CHECK(RemoveDirectoryW(path));
}
static void test_durable_helper_rejects_invalid_arguments_and_unelevated_launch(void)
{
    wchar_t *args[]={L"folderwing-durable.exe",L"1",L"1",L"1",L"00000000000000000000000000000000"};CHECK(durable_program_main(1,args)==2);
    wchar_t *bad[]={L"-1",L"4294967296",L"abc",L""};for(unsigned i=0;i<4;i++){args[1]=bad[i];CHECK(durable_program_main(5,args)==2);}args[1]=L"1";args[4]=L"../bad";CHECK(durable_program_main(5,args)==2);
    args[4]=L"00000000000000000000000000000000";CHECK(durable_program_main(5,args)==3);
}
int wmain(int argc,wchar_t **argv)
{
    if(argc!=2)return 2;root=argv[1];typedef void(*fn)(void);fn tests[]={test_durable_helper_flushes_only_captured_directory,test_durable_helper_rejects_wrong_identity_and_invalid_handle,test_durable_helper_rejects_file_and_pipe_objects,test_durable_helper_respects_external_write_barrier,test_durable_helper_rejects_directory_reparse_object,test_durable_helper_rejects_invalid_arguments_and_unelevated_launch};
    const char *names[]={"test_durable_helper_flushes_only_captured_directory","test_durable_helper_rejects_wrong_identity_and_invalid_handle","test_durable_helper_rejects_file_and_pipe_objects","test_durable_helper_respects_external_write_barrier","test_durable_helper_rejects_directory_reparse_object","test_durable_helper_rejects_invalid_arguments_and_unelevated_launch"};
    for(unsigned i=0;i<sizeof tests/sizeof *tests;i++){unsigned before=failed;uint64_t start=GetTickCount64();tests[i]();printf("CASE %s %s %llu\n",names[i],failed==before?"passed":"failed",(unsigned long long)(GetTickCount64()-start));fflush(stdout);}
    printf("%u passed, %u failed; 6 durable helper selectors\n",passed,failed);return failed?1:0;
}
