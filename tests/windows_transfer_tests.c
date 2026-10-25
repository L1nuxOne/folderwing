/* Windows stream persistence tests. The API wrappers exist only in this
 * executable; the production writer is included verbatim with its engine.
 * Run with an owned Windows directory and an owned WSL UNC source directory. */
#include "../src/internal.h"
#include "../src/wslproto.h"
#include <stdio.h>
#include <string.h>
#include <sddl.h>

static int passed,failed;
static int group_testing;
static fpe_err group_test_fault(const char *tag,fpe_err error);
#define GROUP_FAULT(tag,error) group_test_fault(tag,error)
static struct {
    void *reader;volatile LONG *stop;
    const char *fail,*cancel;unsigned after,seen,checks,writeouts,pre,post,parent,renames,ordinary;
    unsigned check_error_at,bad_check;int32_t check_error;wslp_group result;unsigned replies;
    char order[128];unsigned at;
} G;
static void group_order(char c){if(G.at+1<sizeof G.order)G.order[G.at++]=c,G.order[G.at]=0;}
#define CHECK(x) do { if(x)passed++;else{failed++;printf("FAIL %s:%d %s (winerr=%lu)\n",__FILE__,__LINE__,#x,(unsigned long)GetLastError());} } while(0)
static struct {
    HANDLE file;BY_HANDLE_FILE_INFORMATION identity;
    unsigned flushes,renames,closes,acks,fail_flush,cancel_flush;
    DWORD flush_error,metadata_error,rename_error;
    int32_t ack_error;
    volatile LONG *stop;
    fpe_engine *engine;uint64_t job;
    const wchar_t *mutation_target;unsigned mutation;int mutation_done,check_commit_lock;
    HANDLE concurrent_writer;
    unsigned dacl_queries,dacl_sets;
    unsigned directory_flushes,fail_directory_flush,cancel_directory_flush;
    int setup_dacl,fail_dacl_read,fail_dacl_write,cancel_dacl;
    PSECURITY_DESCRIPTOR expected_dacl;
    unsigned source_checks,source_acks,fail_source_at,add_stream_at;
    DWORD source_query_error;int malformed_stream;
    int32_t source_ack_error;const wchar_t *source_path;
    char order[64];unsigned at;
} T;
static void mutate_destination(void);
static void add_source_stream(void);
static int same_dacl_handle(HANDLE file,PSECURITY_DESCRIPTOR expected);
static void setup_dacl_test(const wchar_t *path,int kind);
static PSECURITY_DESCRIPTOR read_dacl_path(const wchar_t *path);
static void observe(char c){if(T.at+1<sizeof T.order)T.order[T.at++]=c,T.order[T.at]=0;else CHECK(0);}
static HANDLE WINAPI transfer_create(LPCWSTR name,DWORD access,DWORD sharing,LPSECURITY_ATTRIBUTES security,DWORD disposition,DWORD attrs,HANDLE template)
{
    HANDLE file=CreateFileW(name,access,sharing,security,disposition,attrs,template);
    if(file!=INVALID_HANDLE_VALUE&&disposition==CREATE_NEW&&wcsstr(name,L"\\.openpilot-host-")){
        T.file=file;CHECK(GetFileInformationByHandle(file,&T.identity));
    }
    return file;
}
static BOOL WINAPI transfer_time(HANDLE file,const FILETIME *created,const FILETIME *accessed,const FILETIME *modified)
{
    if(group_testing)return SetFileTime(file,created,accessed,modified);
    observe('M');if(T.metadata_error==1){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    return SetFileTime(file,created,accessed,modified);
}
static BOOL WINAPI transfer_info(HANDLE file,FILE_INFO_BY_HANDLE_CLASS kind,LPVOID info,DWORD size)
{
    if(group_testing)return SetFileInformationByHandle(file,kind,info,size);
    if(file==T.file&&kind==FileBasicInfo){observe('B');if(T.metadata_error==2){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}}
    return SetFileInformationByHandle(file,kind,info,size);
}
static BOOL WINAPI transfer_get_info(HANDLE file,FILE_INFO_BY_HANDLE_CLASS kind,LPVOID info,DWORD size)
{
    if(kind==FileStreamInfo){
        T.source_checks++;
        if(T.add_stream_at==T.source_checks)add_source_stream();
        if(T.fail_source_at==T.source_checks){SetLastError(T.source_query_error);return FALSE;}
        if(T.malformed_stream){memset(info,0,size);((FILE_STREAM_INFO *)info)->StreamNameLength=size+2;return TRUE;}
    }
    return GetFileInformationByHandleEx(file,kind,info,size);
}
static BOOL WINAPI transfer_flush(HANDLE file)
{
    if(group_testing){G.ordinary++;return FlushFileBuffers(file);}
    BY_HANDLE_FILE_INFORMATION kind;
    if(GetFileInformationByHandle(file,&kind)&&(kind.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)){
        T.directory_flushes++;
        if(T.cancel_directory_flush==T.directory_flushes&&T.stop)InterlockedExchange(T.stop,1);
        if(T.fail_directory_flush==T.directory_flushes){SetLastError(ERROR_WRITE_FAULT);return FALSE;}
        return FlushFileBuffers(file);
    }
    T.flushes++;observe((char)('0'+T.flushes));CHECK(file==T.file);
    if(T.expected_dacl)CHECK(same_dacl_handle(file,T.expected_dacl));
    BY_HANDLE_FILE_INFORMATION id;CHECK(GetFileInformationByHandle(file,&id));
    CHECK(id.dwVolumeSerialNumber==T.identity.dwVolumeSerialNumber&&id.nFileIndexHigh==T.identity.nFileIndexHigh&&id.nFileIndexLow==T.identity.nFileIndexLow);
    if(T.cancel_flush==T.flushes){
        if(T.stop)InterlockedExchange(T.stop,1);
        if(T.engine)fpe_job_cancel(T.engine,T.job);
    }
    if(T.fail_flush==T.flushes){SetLastError(T.flush_error);return FALSE;}
    BOOL ok=FlushFileBuffers(file);
    if(ok&&T.flushes==1&&T.mutation)mutate_destination();
    return ok;
}
static DWORD transfer_rename(HANDLE file,HANDLE parent,const wchar_t *name,int replace,int readonly)
{
    if(group_testing){G.renames++;group_order('r');return fpe_win_rename_handle(file,parent,name,replace,readonly);}
    T.renames++;observe('R');CHECK(file==T.file);
    if(T.check_commit_lock){
        HANDLE writer=CreateFileW(T.mutation_target,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
        CHECK(writer==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION);
        if(writer!=INVALID_HANDLE_VALUE)CloseHandle(writer);
    }
    return T.rename_error?T.rename_error:fpe_win_rename_handle(file,parent,name,replace,readonly);
}
static BOOL WINAPI transfer_close(HANDLE file)
{
    if(group_testing)return CloseHandle(file);
    if(file==T.file){T.closes++;observe('C');T.file=NULL;}
    return CloseHandle(file);
}
static BOOL group_test_write(LPCVOID bytes,DWORD size);
static BOOL WINAPI transfer_write(HANDLE file,LPCVOID bytes,DWORD size,LPDWORD wrote,LPOVERLAPPED overlapped)
{
    if(group_testing){CHECK(group_test_write(bytes,size));return WriteFile(file,bytes,size,wrote,overlapped);}
    if(size>=sizeof(wslp_head)+8){
        wslp_head h;memcpy(&h,bytes,sizeof h);
        if(h.type==WSLP_GROUP_COMMITTED&&h.id==1&&h.size==size-sizeof h){
            wslp_group g={0};CHECK(h.size<=sizeof g);if(h.size<=sizeof g){memcpy(&g,(const char*)bytes+sizeof h,h.size);CHECK(g.count<=8&&h.size==8+g.count*sizeof(wslp_file_result));
                for(unsigned i=0;i<g.count&&i<8;i++){T.acks++;observe('A');T.ack_error=g.files[i].error;if(!g.files[i].error)CHECK(T.flushes==2&&T.renames==1&&T.closes==1);}}
        }
    }
    if(size==sizeof(wslp_head)+sizeof(int32_t)){
        wslp_head h;memcpy(&h,bytes,sizeof h);
        if(h.type==WSLP_SOURCE_ACK&&h.size==sizeof(int32_t)&&h.id==1){T.source_acks++;memcpy(&T.source_ack_error,(const char *)bytes+sizeof h,sizeof T.source_ack_error);}
        if(h.type==WSLP_FILE_ACK&&h.size==sizeof(int32_t)&&h.id==1){
            T.acks++;observe('A');memcpy(&T.ack_error,(const char *)bytes+sizeof h,sizeof T.ack_error);
        }
    }
    if(size==sizeof(wslp_head)+sizeof(wslp_file_result)){
        wslp_head h;wslp_file_result result;memcpy(&h,bytes,sizeof h);memcpy(&result,(const char*)bytes+sizeof h,sizeof result);
        if(h.type==WSLP_FILE_COMMITTED&&h.id==1&&h.size==sizeof result){
            T.acks++;observe('A');T.ack_error=result.error;
            if(!result.error)CHECK(T.flushes==2&&T.renames==1&&T.closes==1);
        }
    }
    return WriteFile(file,bytes,size,wrote,overlapped);
}
static BOOL WINAPI transfer_get_security(HANDLE file,SECURITY_INFORMATION flags,PSECURITY_DESCRIPTOR sd,DWORD cap,LPDWORD needed)
{
    T.dacl_queries++;
    if(T.fail_dacl_read){if(needed)*needed=0;SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    return GetKernelObjectSecurity(file,flags,sd,cap,needed);
}
static BOOL WINAPI transfer_set_security(HANDLE file,SECURITY_INFORMATION flags,PSECURITY_DESCRIPTOR sd)
{
    T.dacl_sets++;CHECK(file==T.file);
    if(T.fail_dacl_write){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    BOOL ok=SetKernelObjectSecurity(file,flags,sd);
    if(ok&&T.cancel_dacl){if(T.stop)InterlockedExchange(T.stop,1);if(T.engine)fpe_job_cancel(T.engine,T.job);}
    return ok;
}
#define CreateFileW transfer_create
#define SetFileTime transfer_time
#define SetFileInformationByHandle transfer_info
#define GetFileInformationByHandleEx transfer_get_info
#define FlushFileBuffers transfer_flush
#define fpe_win_rename_handle transfer_rename
#define CloseHandle transfer_close
#define WriteFile transfer_write
#define GetKernelObjectSecurity transfer_get_security
#define SetKernelObjectSecurity transfer_set_security
#include "../src/wsl.c"
#undef CreateFileW
#undef SetFileTime
#undef SetFileInformationByHandle
#undef GetFileInformationByHandleEx
#undef FlushFileBuffers
#undef fpe_win_rename_handle
#undef CloseHandle
#undef WriteFile
#undef GetKernelObjectSecurity
#undef SetKernelObjectSecurity
static fpe_err group_test_fault(const char *tag,fpe_err error)
{
    if(!group_testing)return error;
    if(!strcmp(tag,"group.pre_writeout")){G.writeouts++;group_order('w');}
    if(!strcmp(tag,"group.pre_fence")){G.pre++;group_order('f');}
    if(!strcmp(tag,"group.post_writeout"))group_order('p');
    if(!strcmp(tag,"group.post_fence")){G.post++;group_order('F');}
    if(!strcmp(tag,"group.namespace")){G.parent++;group_order('D');}
    if((G.fail&&!strcmp(tag,G.fail))||(G.cancel&&!strcmp(tag,G.cancel))){
        if(++G.seen==G.after){if(G.cancel&&G.stop)InterlockedExchange(G.stop,1);if(G.fail)return error?error:ERROR_WRITE_FAULT;}
    }
    return error;
}
static BOOL group_test_write(LPCVOID bytes,DWORD size)
{
    if(size<sizeof(wslp_head))return FALSE;wslp_head h;memcpy(&h,bytes,sizeof h);
    if(h.id!=1||h.size!=size-sizeof h)return FALSE;
    if(h.type==WSLP_GROUP_CHECK){
        if(h.size!=sizeof(wslp_file_result))return FALSE;wslp_file_result check;memcpy(&check,(const char*)bytes+sizeof h,sizeof check);
        G.checks++;group_order('q');if(check.error||check.pad)return FALSE;
        if(G.check_error_at==G.checks)check.error=G.check_error;
        if(G.bad_check==1)check.token++;if(G.bad_check==2)check.pad=1;
        job_reader *r=G.reader;r->head=(wslp_head){WSLP_GROUP_CHECKED,sizeof check,G.bad_check==3?2:1};memcpy(r->buf,&check,sizeof check);SetEvent(r->ready);
    }else if(h.type==WSLP_GROUP_COMMITTED){
        if(h.size>sizeof G.result)return FALSE;memcpy(&G.result,(const char*)bytes+sizeof h,h.size);G.replies++;group_order('A');
        if(G.renames&&G.pre)CHECK(G.post==1&&G.parent==1);
    }else if(h.type!=WSLP_JOB_CANCEL)return FALSE;
    return TRUE;
}

static const wchar_t *windows_root,*linux_root;
static unsigned serial;
static const char payload[]="complete Windows stream";
static const int64_t stamp=1700000000123456700LL;
static PSECURITY_DESCRIPTOR read_dacl_handle(HANDLE file)
{
    DWORD size=0;GetKernelObjectSecurity(file,DACL_SECURITY_INFORMATION,NULL,0,&size);CHECK(size!=0);
    PSECURITY_DESCRIPTOR sd=fpe_alloc(size);CHECK(sd!=NULL);
    if(sd&&!GetKernelObjectSecurity(file,DACL_SECURITY_INFORMATION,sd,size,&size)){CHECK(0);fpe_free(sd);return NULL;}return sd;
}
static PSECURITY_DESCRIPTOR read_dacl_path(const wchar_t *path)
{
    HANDLE file=CreateFileW(path,READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    CHECK(file!=INVALID_HANDLE_VALUE);if(file==INVALID_HANDLE_VALUE)return NULL;
    PSECURITY_DESCRIPTOR sd=read_dacl_handle(file);CHECK(CloseHandle(file));return sd;
}
static int same_dacl(PSECURITY_DESCRIPTOR a,PSECURITY_DESCRIPTOR b)
{
    if(!a||!b)return 0;BOOL ap,ad,bp,bd;PACL aa,ba;SECURITY_DESCRIPTOR_CONTROL ac,bc;DWORD revision;
    if(!GetSecurityDescriptorDacl(a,&ap,&aa,&ad)||!GetSecurityDescriptorDacl(b,&bp,&ba,&bd)||
       !GetSecurityDescriptorControl(a,&ac,&revision)||!GetSecurityDescriptorControl(b,&bc,&revision))return 0;
    /* AUTO_INHERITED bookkeeping may differ. Compare protection and actual ACEs. */
    return ap==bp&&(ac&SE_DACL_PROTECTED)==(bc&SE_DACL_PROTECTED)&&
        ((!aa&&!ba)||(aa&&ba&&aa->AclSize==ba->AclSize&&!memcmp(aa,ba,aa->AclSize)));
}
static int same_dacl_handle(HANDLE file,PSECURITY_DESCRIPTOR expected)
{PSECURITY_DESCRIPTOR sd=read_dacl_handle(file);int same=same_dacl(sd,expected);fpe_free(sd);return same;}
static int same_dacl_path(const wchar_t *path,PSECURITY_DESCRIPTOR expected)
{PSECURITY_DESCRIPTOR sd=read_dacl_path(path);int same=same_dacl(sd,expected);fpe_free(sd);return same;}
static void apply_dacl_test(const wchar_t *path,PSECURITY_DESCRIPTOR sd)
{
    HANDLE file=CreateFileW(path,WRITE_DAC,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    CHECK(file!=INVALID_HANDLE_VALUE);if(file==INVALID_HANDLE_VALUE)return;
    SECURITY_DESCRIPTOR_CONTROL control;DWORD revision;CHECK(GetSecurityDescriptorControl(sd,&control,&revision));
    CHECK(SetKernelObjectSecurity(file,DACL_SECURITY_INFORMATION|((control&SE_DACL_PROTECTED)?PROTECTED_DACL_SECURITY_INFORMATION:UNPROTECTED_DACL_SECURITY_INFORMATION),sd));CHECK(CloseHandle(file));
}
static void setup_dacl_test(const wchar_t *path,int kind)
{
    if(kind==4){
        /* A NULL DACL requires an absolute descriptor for the setter API. */
        SECURITY_DESCRIPTOR sd;CHECK(InitializeSecurityDescriptor(&sd,SECURITY_DESCRIPTOR_REVISION));
        CHECK(SetSecurityDescriptorDacl(&sd,TRUE,NULL,FALSE));
        CHECK(SetSecurityDescriptorControl(&sd,SE_DACL_PROTECTED,SE_DACL_PROTECTED));
        apply_dacl_test(path,&sd);return;
    }
    HANDLE token=NULL;DWORD size=0;CHECK(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token));
    GetTokenInformation(token,TokenUser,NULL,0,&size);TOKEN_USER *user=fpe_alloc(size);CHECK(user!=NULL);
    if(!user){if(token)CloseHandle(token);return;}
    CHECK(GetTokenInformation(token,TokenUser,user,size,&size));LPWSTR sid=NULL;CHECK(ConvertSidToStringSidW(user->User.Sid,&sid));
    if(sid){
        wchar_t sddl[1024];const wchar_t *inherit=kind==5?L"OICI":L"";
        _snwprintf(sddl,1024,L"D:%ls%ls(A;%ls;FA;;;SY)(A;%ls;FA;;;%ls)%ls",kind==3?L"":L"P",kind==2?L"(D;;0x2;;;WD)":L"",inherit,inherit,sid,kind==5?L"(A;OICI;GRGX;;;WD)":L"");
        PSECURITY_DESCRIPTOR sd=NULL;CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl,SDDL_REVISION_1,&sd,NULL));
        if(sd){
            apply_dacl_test(path,sd);LocalFree(sd);
        }
        LocalFree(sid);
    }
    fpe_free(user);if(token)CHECK(CloseHandle(token));
}
static void path_join_test(wchar_t *out,const wchar_t *root,const wchar_t *leaf)
{_snwprintf(out,4096,L"%ls\\%ls",root,leaf);out[4095]=0;}
static int exists_test(const wchar_t *path){return GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES;}
static int contents_test(const wchar_t *path,const char *bytes,size_t size)
{
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    if(file==INVALID_HANDLE_VALUE)return 0;
    char data[256];DWORD got=0;int ok=ReadFile(file,data,sizeof data,&got,NULL)&&got==size&&!memcmp(data,bytes,size);CloseHandle(file);return ok;
}
static void write_test(const wchar_t *path,const char *bytes,DWORD size)
{
    HANDLE file=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);CHECK(file!=INVALID_HANDLE_VALUE);
    if(file==INVALID_HANDLE_VALUE)return;
    DWORD wrote=0;CHECK(WriteFile(file,bytes,size,&wrote,NULL)&&wrote==size);
    uint64_t ticks=(uint64_t)(stamp/100+116444736000000000LL);FILETIME time={(DWORD)ticks,(DWORD)(ticks>>32)};
    CHECK(SetFileTime(file,NULL,&time,&time));CHECK(CloseHandle(file));
}
static void add_source_stream(void)
{
    wchar_t stream[4096];_snwprintf(stream,4096,L"%ls:late",T.source_path);write_test(stream,"named data",10);
}
static void mutate_destination(void)
{
    const wchar_t *target=T.mutation_target;CHECK(target!=NULL&&!T.mutation_done);T.mutation_done=1;
    if(T.mutation==2||T.mutation==3||T.mutation==7)CHECK(DeleteFileW(target));
    if(T.mutation==2)return;
    if(T.mutation==7){CHECK(CreateDirectoryW(target,NULL));return;}
    if(T.mutation==5){CHECK(SetFileAttributesW(target,FILE_ATTRIBUTE_HIDDEN));return;}
    HANDLE file=CreateFileW(target,GENERIC_WRITE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,
        T.mutation==3||T.mutation==4?CREATE_NEW:OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    CHECK(file!=INVALID_HANDLE_VALUE);if(file==INVALID_HANDLE_VALUE)return;
    if(T.mutation==6){T.concurrent_writer=file;return;}
    FILETIME at,mt;CHECK(GetFileTime(file,NULL,&at,&mt));
    DWORD wrote=0;CHECK(WriteFile(file,"new",3,&wrote,NULL)&&wrote==3);CHECK(SetEndOfFile(file));
    if(T.mutation==1)CHECK(SetFileTime(file,NULL,&at,&mt)); /* restored mtime, changed content */
    CHECK(CloseHandle(file));
}
static void no_workspace(void)
{
    wchar_t pattern[4096];path_join_test(pattern,windows_root,L".openpilot-host-*.tmp");WIN32_FIND_DATAW data;
    HANDLE find=FindFirstFileW(pattern,&data);CHECK(find==INVALID_HANDLE_VALUE);if(find!=INVALID_HANDLE_VALUE)FindClose(find);
}
static void direct_begin(windows_file *w,wchar_t *target,int replace,int empty)
{
    char suffix[100];snprintf(suffix,sizeof suffix,"/direct-%u.bin",++serial);
    wchar_t leaf[100];_snwprintf(leaf,100,L"direct-%u.bin",serial);path_join_test(target,windows_root,leaf);
    if(replace){
        write_test(target,"old",3);
        if(T.setup_dacl){setup_dacl_test(target,T.setup_dacl);T.expected_dacl=read_dacl_path(target);}
        /* Give the captured change-time version a deterministic baseline.
         * NTFS timestamps can otherwise collide for freshly created fixtures. */
        HANDLE h=CreateFileW(target,FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
        CHECK(h!=INVALID_HANDLE_VALUE);
        if(h!=INVALID_HANDLE_VALUE){FILE_BASIC_INFO info={0};info.ChangeTime.QuadPart=stamp/100+116444736000000000LL;CHECK(SetFileInformationByHandle(h,FileBasicInfo,&info,sizeof info));CHECK(CloseHandle(h));}
    }
    wslp_file f={.atime=stamp,.mtime=stamp,.replace=(uint32_t)replace,.path_bytes=(uint32_t)strlen(suffix)};
    windows_begin(w,&f,(const uint8_t *)suffix,windows_root,0);CHECK(!w->err&&w->created&&w->file!=INVALID_HANDLE_VALUE);
    if(w->err)return;
    DWORD wrote=0,size=empty?0:(DWORD)strlen(payload);CHECK(WriteFile(w->file,payload,size,&wrote,NULL)&&wrote==size);
}
static void check_direct_cleanup(windows_file *w)
{CHECK(!w->active&&!w->created&&!w->file&&!w->previous&&!w->dest&&!w->temp);CHECK(T.closes==1);no_workspace();}

static void test_flush_order_and_final_metadata(void)
{
    for(int replace=0;replace<2;replace++)for(int empty=0;empty<2;empty++){
        memset(&T,0,sizeof T);windows_file w={0};wchar_t target[4096];direct_begin(&w,target,replace,empty);
        CHECK(!windows_end(&w,0,NULL));CHECK(!strcmp(T.order,"MB1R2C"));CHECK(T.flushes==2&&T.renames==1);
        HANDLE file=CreateFileW(target,FILE_READ_ATTRIBUTES,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);FILETIME at,mt;
        CHECK(file!=INVALID_HANDLE_VALUE&&GetFileTime(file,NULL,&at,&mt));
        CHECK((((uint64_t)mt.dwHighDateTime<<32)|mt.dwLowDateTime)==(uint64_t)(stamp/100+116444736000000000LL));
        CHECK((((uint64_t)at.dwHighDateTime<<32)|at.dwLowDateTime)==(uint64_t)(stamp/100+116444736000000000LL));
        if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
        CHECK(contents_test(target,payload,empty?0:strlen(payload)));
        CHECK(!(GetFileAttributesW(target)&(FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_TEMPORARY)));
        check_direct_cleanup(&w);CHECK(DeleteFileW(target));
    }
}
static void test_prepublication_flush_failure(void)
{
    DWORD errors[]={ERROR_WRITE_FAULT,ERROR_DISK_FULL,ERROR_NOT_SUPPORTED,ERROR_ACCESS_DENIED};int posix[]={5,28,95,13};
    for(unsigned e=0;e<4;e++)for(int replace=0;replace<2;replace++)for(int empty=0;empty<2;empty++){
        memset(&T,0,sizeof T);T.fail_flush=1;T.flush_error=errors[e];windows_file w={0};wchar_t target[4096];direct_begin(&w,target,replace,empty);
        DWORD err=windows_end(&w,0,NULL);CHECK(err==errors[e]&&errno_windows(err)==posix[e]);
        CHECK(!strcmp(T.order,"MB1C")&&T.flushes==1&&!T.renames);
        CHECK(replace?contents_test(target,"old",3):!exists_test(target));check_direct_cleanup(&w);
        if(replace)CHECK(DeleteFileW(target));
    }
}
static void test_postpublication_flush_failure(void)
{
    DWORD errors[]={ERROR_WRITE_FAULT,ERROR_DISK_FULL,ERROR_NOT_SUPPORTED,ERROR_ACCESS_DENIED};
    for(unsigned e=0;e<4;e++)for(int replace=0;replace<2;replace++)for(int empty=0;empty<2;empty++){
        memset(&T,0,sizeof T);T.fail_flush=2;T.flush_error=errors[e];windows_file w={0};wchar_t target[4096];direct_begin(&w,target,replace,empty);
        CHECK(windows_end(&w,0,NULL)==errors[e]);CHECK(!strcmp(T.order,"MB1R2C"));
        CHECK(contents_test(target,payload,empty?0:strlen(payload)));check_direct_cleanup(&w);CHECK(DeleteFileW(target));
    }
}
static void test_existing_errors_abort_and_cleanup(void)
{
    for(unsigned cause=0;cause<7;cause++){
        memset(&T,0,sizeof T);windows_file w={0};wchar_t target[4096];direct_begin(&w,target,1,0);volatile LONG stop=cause==6;
        DWORD expected=ERROR_ACCESS_DENIED;
        if(cause==0)w.err=ERROR_DISK_FULL,expected=ERROR_DISK_FULL;
        if(cause==1)expected=ERROR_CANCELLED;
        if(cause==2)T.metadata_error=1;
        if(cause==3)T.metadata_error=2;
        if(cause==4)T.rename_error=ERROR_ACCESS_DENIED;
        if(cause==5)w.err=ERROR_WRITE_FAULT,expected=ERROR_WRITE_FAULT;
        if(cause==6)expected=ERROR_CANCELLED;
        CHECK(windows_end(&w,cause==1||cause==5,&stop)==expected);
        CHECK(T.flushes==(cause==4?1u:0u));CHECK(T.renames==(cause==4?1u:0u));
        CHECK(contents_test(target,"old",3));check_direct_cleanup(&w);CHECK(DeleteFileW(target));
    }
}
static void test_cancellation_during_flush(void)
{
    for(unsigned boundary=1;boundary<=2;boundary++)for(int fail=0;fail<2;fail++){
        memset(&T,0,sizeof T);volatile LONG stop=0;T.stop=&stop;T.cancel_flush=boundary;
        if(fail)T.fail_flush=boundary,T.flush_error=ERROR_WRITE_FAULT;
        windows_file w={0};wchar_t target[4096];direct_begin(&w,target,1,0);
        CHECK(windows_end(&w,0,&stop)==(fail?ERROR_WRITE_FAULT:ERROR_CANCELLED));CHECK(stop==1);
        CHECK(T.flushes==boundary&&T.renames==(boundary==1?0u:1u));
        CHECK(contents_test(target,boundary==1?"old":payload,boundary==1?3:strlen(payload)));
        check_direct_cleanup(&w);CHECK(DeleteFileW(target));
    }
}
static void test_guarded_parent_publication(void)
{
    HANDLE guard=CreateFileW(windows_root,FILE_TRAVERSE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    CHECK(guard!=INVALID_HANDLE_VALUE);memset(&T,0,sizeof T);windows_file w={0};wchar_t target[4096];direct_begin(&w,target,1,0);
    CHECK(!windows_end(&w,0,NULL));CHECK(!strcmp(T.order,"MB1R2C"));CHECK(contents_test(target,payload,strlen(payload)));
    check_direct_cleanup(&w);if(guard!=INVALID_HANDLE_VALUE)CloseHandle(guard);CHECK(DeleteFileW(target));
}
static void test_destination_version_changes_are_preserved(void)
{
    for(int during_flush=0;during_flush<2;during_flush++)for(unsigned mode=1;mode<=7;mode++){
        memset(&T,0,sizeof T);windows_file w={0};wchar_t target[4096];direct_begin(&w,target,mode!=4,0);
        T.mutation_target=target;T.mutation=mode;if(!during_flush){mutate_destination();T.mutation=0;}
        DWORD expected=mode==2?ERROR_FILE_NOT_FOUND:mode==4?ERROR_ALREADY_EXISTS:mode==6?ERROR_SHARING_VIOLATION:ERROR_FILE_INVALID;
        DWORD actual=windows_end(&w,0,NULL);
        unsigned attempts=mode==4?1u:0u; /* Exclusive rename itself rejects late creation. */
        if(actual!=expected||!T.mutation_done||T.renames!=attempts||T.flushes!=1)printf("RACE mode=%u during_flush=%d error=%lu expected=%lu mutation=%d renames=%u flushes=%u\n",mode,during_flush,(unsigned long)actual,(unsigned long)expected,T.mutation_done,T.renames,T.flushes);
        CHECK(actual==expected);CHECK(T.mutation_done&&T.renames==attempts&&T.flushes==1);
        if(T.concurrent_writer){CHECK(CloseHandle(T.concurrent_writer));T.concurrent_writer=NULL;}
        if(mode==2)CHECK(!exists_test(target));
        else if(mode==7){CHECK(GetFileAttributesW(target)&FILE_ATTRIBUTE_DIRECTORY);CHECK(RemoveDirectoryW(target));}
        else{CHECK(contents_test(target,mode==5||mode==6?"old":"new",3));CHECK(SetFileAttributesW(target,FILE_ATTRIBUTE_NORMAL));CHECK(DeleteFileW(target));}
        check_direct_cleanup(&w);
    }
}
static void test_destination_commit_blocks_new_writers(void)
{
    memset(&T,0,sizeof T);windows_file w={0};wchar_t target[4096];direct_begin(&w,target,1,0);
    T.mutation_target=target;T.check_commit_lock=1;
    CHECK(!windows_end(&w,0,NULL));CHECK(T.renames==1);CHECK(contents_test(target,payload,strlen(payload)));
    check_direct_cleanup(&w);CHECK(DeleteFileW(target));
}
static void test_overwrite_dacl_is_flushed_before_publication(void)
{
    PSECURITY_DESCRIPTOR parent=read_dacl_path(windows_root);setup_dacl_test(windows_root,5);
    for(int kind=1;kind<=4;kind++){
        memset(&T,0,sizeof T);T.setup_dacl=kind;windows_file w={0};wchar_t target[4096];direct_begin(&w,target,1,0);
        CHECK(!windows_end(&w,0,NULL));CHECK(T.dacl_queries==2&&T.dacl_sets==1&&T.flushes==2&&T.renames==1);
        CHECK(same_dacl_path(target,T.expected_dacl));CHECK(contents_test(target,payload,strlen(payload)));
        check_direct_cleanup(&w);fpe_free(T.expected_dacl);T.expected_dacl=NULL;CHECK(DeleteFileW(target));
    }
    apply_dacl_test(windows_root,parent);fpe_free(parent);
}
static void test_dacl_errors_and_cancellation_never_publish(void)
{
    for(int cause=0;cause<5;cause++){
        memset(&T,0,sizeof T);T.setup_dacl=1;windows_file w={0};wchar_t target[4096];direct_begin(&w,target,1,0);volatile LONG stop=0;T.stop=&stop;
        if(cause==0)T.fail_dacl_read=1;
        if(cause==1)T.fail_dacl_write=1;
        if(cause==2)T.cancel_dacl=1;
        if(cause==3)T.fail_flush=1,T.flush_error=ERROR_WRITE_FAULT;
        if(cause==4)T.rename_error=ERROR_ACCESS_DENIED;
        CHECK(windows_end(&w,0,&stop)==(cause==2?ERROR_CANCELLED:cause==3?ERROR_WRITE_FAULT:ERROR_ACCESS_DENIED));
        CHECK(T.dacl_queries==(cause==0?1u:2u)&&T.dacl_sets==(cause==0?0u:1u));
        CHECK(T.flushes==(cause<3?0u:1u)&&T.renames==(cause==4?1u:0u));
        CHECK(contents_test(target,"old",3)&&same_dacl_path(target,T.expected_dacl));check_direct_cleanup(&w);
        fpe_free(T.expected_dacl);T.expected_dacl=NULL;CHECK(DeleteFileW(target));
    }
}

static fpe_engine *test_engine;
static struct {HANDLE done;fpe_job_progress last;unsigned finals,events;} J;
static void job_progress_test(void *user,const fpe_job_progress *p)
{
    T.job=p->job;
    if(p->state>=FPE_JOB_DONE){J.last=*p;J.last.current=NULL;J.finals++;SetEvent(J.done);}
}
static void job_event_test(void *user,const fpe_job_event *event){J.events++;}
static void bridge_begin(wchar_t *source,wchar_t *target,int empty)
{
    wchar_t leaf[100];_snwprintf(leaf,100,L"bridge-%u.bin",++serial);path_join_test(source,linux_root,leaf);path_join_test(target,windows_root,leaf);
    write_test(source,payload,empty?0:(DWORD)strlen(payload));write_test(target,"old",3);
    if(T.setup_dacl){setup_dacl_test(target,T.setup_dacl);T.expected_dacl=read_dacl_path(target);}
}
static void bridge_run(const wchar_t *source,int move)
{
    memset(&J,0,sizeof J);J.done=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(J.done!=NULL);
    const wchar_t *paths[]={source};
    uint64_t id;
    if(move==2){paths[0]=wcsrchr(source,L'\\')+1;id=fpe_job_submit_tree(test_engine,linux_root,paths,1,windows_root,FPE_CONFLICT_OVERWRITE,job_progress_test,job_event_test,NULL);}
    else id=fpe_job_submit_ex(test_engine,move?FPE_OP_MOVE:FPE_OP_COPY,paths,1,windows_root,FPE_CONFLICT_OVERWRITE,job_progress_test,job_event_test,NULL);
    CHECK(id!=0);DWORD wait=WaitForSingleObject(J.done,30000);CHECK(wait==WAIT_OBJECT_0);
    if(wait!=WAIT_OBJECT_0){fpe_job_cancel(test_engine,id);CHECK(WaitForSingleObject(J.done,30000)==WAIT_OBJECT_0);}
    CHECK(J.finals==1);CloseHandle(J.done);CHECK(T.acks==1&&T.closes==1);no_workspace();
}
static void test_wsl_move_requires_successful_flush_ack(void)
{
    for(int move=0;move<2;move++)for(unsigned boundary=1;boundary<=2;boundary++)for(int empty=0;empty<2;empty++){
        memset(&T,0,sizeof T);T.fail_flush=boundary;T.flush_error=ERROR_WRITE_FAULT;wchar_t source[4096],target[4096];bridge_begin(source,target,empty);bridge_run(source,move);
        CHECK(J.last.state==FPE_JOB_DONE&&J.last.errors==1&&J.events==1&&J.last.last_error==ERROR_IO_DEVICE);
        CHECK(T.ack_error==5&&T.flushes==boundary);CHECK(!strcmp(T.order,boundary==1?"MB1CA":"MB1R2CA"));
        CHECK(contents_test(source,payload,empty?0:strlen(payload)));
        CHECK(contents_test(target,boundary==1?"old":payload,boundary==1?3:empty?0:strlen(payload)));
        CHECK(DeleteFileW(source));CHECK(DeleteFileW(target));
    }
}
static void test_wsl_success_ack_and_source_removal(void)
{
    for(int move=0;move<2;move++)for(int empty=0;empty<2;empty++){
        memset(&T,0,sizeof T);wchar_t source[4096],target[4096];bridge_begin(source,target,empty);bridge_run(source,move);
        CHECK(J.last.state==FPE_JOB_DONE&&!J.last.errors&&!J.events);CHECK(!T.ack_error&&T.flushes==2&&T.renames==1);CHECK(!strcmp(T.order,"MB1R2CA"));
        CHECK(contents_test(target,payload,empty?0:strlen(payload)));
        if(move)CHECK(!exists_test(source));else{CHECK(contents_test(source,payload,empty?0:strlen(payload)));CHECK(DeleteFileW(source));}
        CHECK(DeleteFileW(target));
    }
}
static void test_wsl_single_unit_destinations_copy_move_and_selected(void)
{
    const wchar_t *names[]={L"a",L"\x03bb"};
    for(unsigned i=0;i<2;i++)for(int route=0;route<3;route++)for(int replace=0;replace<2;replace++){
        memset(&T,0,sizeof T);wchar_t oldsource[4096],oldtarget[4096],source[4096],target[4096];bridge_begin(oldsource,oldtarget,0);
        path_join_test(source,linux_root,names[i]);path_join_test(target,windows_root,names[i]);
        CHECK(MoveFileW(oldsource,source));CHECK(DeleteFileW(oldtarget));if(replace)write_test(target,"old",3);
        bridge_run(source,route);CHECK(!J.last.errors&&J.last.state==FPE_JOB_DONE&&!J.events);
        CHECK(T.acks==1&&!T.ack_error&&T.flushes==2&&T.renames==1);CHECK(contents_test(target,payload,strlen(payload)));
        if(route==1)CHECK(!exists_test(source));else{CHECK(contents_test(source,payload,strlen(payload)));CHECK(DeleteFileW(source));}
        CHECK(DeleteFileW(target));
    }
}
static void test_wsl_cancellation_during_flush_retains_source(void)
{
    for(unsigned boundary=1;boundary<=2;boundary++){
        memset(&T,0,sizeof T);T.engine=test_engine;T.cancel_flush=boundary;wchar_t source[4096],target[4096];bridge_begin(source,target,0);bridge_run(source,1);
        CHECK(J.last.state==FPE_JOB_CANCELLED);CHECK(T.ack_error==125&&T.flushes==boundary);
        CHECK(contents_test(source,payload,strlen(payload)));CHECK(contents_test(target,boundary==1?"old":payload,boundary==1?3:strlen(payload)));
        CHECK(DeleteFileW(source));CHECK(DeleteFileW(target));
    }
}
static void test_wsl_selected_copy_flush_ack(void)
{
    for(unsigned boundary=0;boundary<=2;boundary++){
        memset(&T,0,sizeof T);T.fail_flush=boundary;T.flush_error=ERROR_WRITE_FAULT;wchar_t source[4096],target[4096];bridge_begin(source,target,0);bridge_run(source,2);
        CHECK(J.last.state==FPE_JOB_DONE&&J.last.errors==(boundary?1u:0u));CHECK(T.ack_error==(boundary?5:0));
        CHECK(!strcmp(T.order,boundary==1?"MB1CA":"MB1R2CA"));CHECK(contents_test(source,payload,strlen(payload)));
        CHECK(contents_test(target,boundary==1?"old":payload,boundary==1?3:strlen(payload)));CHECK(DeleteFileW(source));CHECK(DeleteFileW(target));
    }
}
static void test_wsl_destination_races_reject_ack_and_preserve_source(void)
{
    for(int move=0;move<3;move++){
        memset(&T,0,sizeof T);wchar_t source[4096],target[4096];bridge_begin(source,target,0);
        T.mutation_target=target;T.mutation=1;bridge_run(source,move);
        CHECK(T.mutation_done&&T.ack_error==116&&T.renames==0);CHECK(T.flushes==1);
        CHECK(J.last.state==FPE_JOB_DONE&&J.last.errors==1&&J.events==1&&J.last.last_error==ERROR_FILE_INVALID);
        CHECK(contents_test(source,payload,strlen(payload)));CHECK(contents_test(target,"new",3));
        CHECK(DeleteFileW(source));CHECK(DeleteFileW(target));
    }
}
static void test_wsl_overwrite_dacl_and_failed_security_ack(void)
{
    PSECURITY_DESCRIPTOR parent=read_dacl_path(windows_root);setup_dacl_test(windows_root,5);
    for(int move=0;move<3;move++)for(int cause=0;cause<4;cause++){
        memset(&T,0,sizeof T);T.setup_dacl=1;wchar_t source[4096],target[4096];bridge_begin(source,target,0);
        if(cause==1)T.fail_dacl_read=1;
        if(cause==2)T.fail_dacl_write=1;
        if(cause==3)T.cancel_dacl=1,T.engine=test_engine;
        bridge_run(source,move);
        CHECK(T.dacl_queries==(cause==1?1u:2u)&&T.dacl_sets==(cause==1?0u:1u));
        CHECK(T.ack_error==(cause==3?125:cause?13:0));CHECK(T.renames==(cause?0u:1u)&&T.flushes==(cause?0u:2u));
        CHECK(J.last.state==(cause==3?FPE_JOB_CANCELLED:FPE_JOB_DONE));if(cause!=3)CHECK(J.last.errors==(cause?1u:0u));
        CHECK(contents_test(target,cause?"old":payload,cause?3:strlen(payload))&&same_dacl_path(target,T.expected_dacl));
        if(!cause&&move==1)CHECK(!exists_test(source));else{CHECK(contents_test(source,payload,strlen(payload)));CHECK(DeleteFileW(source));}
        fpe_free(T.expected_dacl);T.expected_dacl=NULL;CHECK(DeleteFileW(target));
    }
    apply_dacl_test(windows_root,parent);fpe_free(parent);
}
static void test_windows_source_inventory_is_fail_closed(void)
{
    memset(&T,0,sizeof T);wchar_t file[4096],stream[4096],directory[4096];
    path_join_test(file,windows_root,L"inventory-node");path_join_test(directory,windows_root,L"inventory-directory");
    write_test(file,"default bytes",13);CHECK(!windows_source_streams(file));CHECK(CreateDirectoryW(directory,NULL));CHECK(!windows_source_streams(directory));
    _snwprintf(stream,4096,L"%ls:empty",file);write_test(stream,"",0);CHECK(windows_source_streams(file)==ERROR_NOT_SUPPORTED);CHECK(windows_source_streams(stream)==ERROR_NOT_SUPPORTED);CHECK(DeleteFileW(stream));
    _snwprintf(stream,4096,L"%ls:meta",directory);write_test(stream,"directory data",14);CHECK(windows_source_streams(directory)==ERROR_NOT_SUPPORTED);CHECK(DeleteFileW(stream));
    memset(&T,0,sizeof T);T.fail_source_at=1;T.source_query_error=ERROR_ACCESS_DENIED;CHECK(windows_source_streams(file)==ERROR_ACCESS_DENIED);
    memset(&T,0,sizeof T);T.fail_source_at=1;T.source_query_error=ERROR_MORE_DATA;CHECK(!windows_source_streams(file));CHECK(T.source_checks==2);
    memset(&T,0,sizeof T);T.malformed_stream=1;CHECK(windows_source_streams(file)==ERROR_INVALID_DATA);
    memset(&T,0,sizeof T);CHECK(DeleteFileW(file));CHECK(RemoveDirectoryW(directory));
}
static void test_windows_source_request_rejects_foreign_paths(void)
{
    memset(&T,0,sizeof T);const wchar_t *sources[]={windows_root};uint32_t flags[]={0,1};
    wslp_source_check q={1,0};CHECK(!windows_source_request(&q,NULL,sources,1,linux_root,flags,0));
    q.root=0;CHECK(windows_source_request(&q,NULL,sources,1,linux_root,flags,0)==ERROR_INVALID_DATA);
    q.root=2;CHECK(windows_source_request(&q,NULL,sources,1,linux_root,flags,0)==ERROR_INVALID_DATA);q.root=1;
    const char *bad[]={"/../escape","//absolute","/bad:stream","/trailing.","/a/../../outside","/","relative","/a\\b"};
    for(unsigned i=0;i<sizeof bad/sizeof *bad;i++){q.path_bytes=(uint32_t)strlen(bad[i]);CHECK(windows_source_request(&q,(const uint8_t *)bad[i],sources,1,linux_root,flags,0)!=0);}
    const uint8_t nul[]={'/','a',0,'b'},invalid[]={'/',0xff};q.path_bytes=sizeof nul;CHECK(windows_source_request(&q,nul,sources,1,linux_root,flags,0)!=0);q.path_bytes=sizeof invalid;CHECK(windows_source_request(&q,invalid,sources,1,linux_root,flags,0)!=0);
    flags[1]=0;q.path_bytes=0;CHECK(windows_source_request(&q,NULL,sources,1,linux_root,flags,0)==ERROR_INVALID_DATA);
}
static void source_run(const wchar_t *source,int mode)
{
    memset(&J,0,sizeof J);J.done=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(J.done!=NULL);const wchar_t *paths[]={source};uint64_t id;
    if(mode==2){paths[0]=wcsrchr(source,L'\\')+1;id=fpe_job_submit_tree(test_engine,windows_root,paths,1,linux_root,FPE_CONFLICT_OVERWRITE,job_progress_test,job_event_test,NULL);}
    else id=fpe_job_submit_ex(test_engine,mode?FPE_OP_MOVE:FPE_OP_COPY,paths,1,linux_root,FPE_CONFLICT_OVERWRITE,job_progress_test,job_event_test,NULL);
    CHECK(id!=0);DWORD wait=WaitForSingleObject(J.done,30000);CHECK(wait==WAIT_OBJECT_0);
    if(wait!=WAIT_OBJECT_0){fpe_job_cancel(test_engine,id);CHECK(WaitForSingleObject(J.done,30000)==WAIT_OBJECT_0);}CHECK(J.finals==1);CHECK(CloseHandle(J.done));
}
static void test_windows_named_streams_never_disappear_in_linux_moves(void)
{
    for(int mode=0;mode<3;mode++)for(int cause=0;cause<7;cause++){
        if(cause==6&&mode!=1)continue;
        memset(&T,0,sizeof T);wchar_t source[4096],target[4096],stream[4096],leaf[100];
        _snwprintf(leaf,100,L"source-check-%u.bin",++serial);path_join_test(source,windows_root,leaf);path_join_test(target,linux_root,leaf);
        write_test(source,payload,(DWORD)strlen(payload));write_test(target,"old",3);
        _snwprintf(stream,4096,L"%ls:extra",source);
        if(cause==1||cause==2)write_test(stream,"named bytes",cause==2?0:11);
        if(cause==3){T.fail_source_at=1;T.source_query_error=ERROR_ACCESS_DENIED;}
        if(cause==4){T.fail_source_at=2;T.source_query_error=ERROR_IO_DEVICE;}
        if(cause==5||cause==6){T.add_stream_at=cause==5?2:3;T.source_path=source;_snwprintf(stream,4096,L"%ls:late",source);}
        source_run(source,mode);
        CHECK(J.last.state==FPE_JOB_DONE&&J.last.errors==(cause?1u:0u));CHECK(J.events==(cause?1u:0u));
        CHECK(T.source_ack_error==(cause==3?13:cause==4?5:cause?95:0));
        CHECK(contents_test(target,cause&&cause!=6?"old":payload,cause&&cause!=6?3:strlen(payload)));
        CHECK(T.source_acks==(cause==1||cause==2||cause==3?1u:cause==6||(!cause&&mode==1)?3u:2u));
        if(!cause&&mode==1)CHECK(!exists_test(source));else{
            CHECK(contents_test(source,payload,strlen(payload)));
            if(cause==1||cause==2||cause==5||cause==6){CHECK(contents_test(stream,cause==1||cause==2?"named bytes":"named data",cause==2?0:cause==1?11:10));CHECK(DeleteFileW(stream));}
            CHECK(DeleteFileW(source));
        }
        CHECK(DeleteFileW(target));
    }
}
static void test_windows_tree_stream_refusal_retains_unsupported_nodes(void)
{
    for(int directory_stream=0;directory_stream<2;directory_stream++){
        memset(&T,0,sizeof T);wchar_t source[4096],target[4096],child[4096],stream[4096],plain[4096],copied[4096],leaf[100];
        _snwprintf(leaf,100,L"source-tree-%u",++serial);path_join_test(source,windows_root,leaf);path_join_test(target,linux_root,leaf);CHECK(CreateDirectoryW(source,NULL));
        path_join_test(child,source,L"named-child");path_join_test(plain,source,L"plain-child");write_test(child,"retained",8);write_test(plain,"complete",8);
        _snwprintf(stream,4096,L"%ls:metadata",directory_stream?source:child);write_test(stream,"stream",6);
        source_run(source,1);CHECK(J.last.state==FPE_JOB_DONE&&J.last.errors==1&&J.last.last_error==ERROR_NOT_SUPPORTED);
        CHECK(contents_test(stream,"stream",6)&&contents_test(child,"retained",8));
        if(directory_stream){CHECK(!exists_test(target));CHECK(contents_test(plain,"complete",8));CHECK(DeleteFileW(plain));}
        else{CHECK(!exists_test(plain));path_join_test(copied,target,L"plain-child");CHECK(contents_test(copied,"complete",8));CHECK(DeleteFileW(copied));path_join_test(copied,target,L"named-child");CHECK(!exists_test(copied));CHECK(RemoveDirectoryW(target));}
        CHECK(DeleteFileW(stream));CHECK(DeleteFileW(child));CHECK(RemoveDirectoryW(source));
    }
}
/* The included production WSL bridge also owns metadata replies. These local
 * pipe cases need no helper faults and never alter the user distro/session. */
static int metadata_pipe(metadata_reader *r,HANDLE *input,HANDLE *output)
{
    memset(r,0,sizeof *r);*input=*output=NULL;
    if(!CreatePipe(input,output,NULL,4096))return 0;
    fpe_err err=metadata_reader_start(r,*input);CHECK(!err);
    if(err){CloseHandle(*input);CloseHandle(*output);return 0;}return 1;
}
static void metadata_pipe_close(metadata_reader *r,HANDLE input,HANDLE output)
{
    metadata_reader_stop(r);CHECK(!r->thread&&!r->wake&&!r->ready);
    metadata_reader_stop(r);CHECK(CloseHandle(input));if(output)CHECK(CloseHandle(output));
}
typedef struct metadata_fragments {HANDLE pipe;wslp_head head;wslp_permission_result result;} metadata_fragments;
static DWORD WINAPI metadata_fragment_writer(void *arg)
{
    metadata_fragments *f=arg;DWORD wrote;
    for(unsigned i=0;i<sizeof f->head;i++){
        if(!WriteFile(f->pipe,(BYTE *)&f->head+i,1,&wrote,NULL)||wrote!=1)return 1;Sleep(1);
    }
    for(unsigned i=0;i<sizeof f->result;i+=31){
        DWORD n=(DWORD)sizeof f->result-i;if(n>31)n=31;
        if(!WriteFile(f->pipe,(BYTE *)&f->result+i,n,&wrote,NULL)||wrote!=n)return 2;Sleep(1);
    }
    return 0;
}
static void test_metadata_reader_fragmented_and_repeated_replies(void)
{
    metadata_reader r;HANDLE input,output;if(!metadata_pipe(&r,&input,&output)){CHECK(0);return;}
    metadata_fragments f={.pipe=output,.head={WSLP_PERMISSION_RESULT,sizeof(wslp_permission_result),1},.result={.state={.inode=123,.mode=0640}}};
    HANDLE writer=CreateThread(NULL,0,metadata_fragment_writer,&f,0,NULL);CHECK(writer!=NULL);
    wslp_permission_result got={0};CHECK(!metadata_receive(&r,WSLP_PERMISSION_RESULT,1,&got,sizeof got,3000));
    CHECK(!memcmp(&got,&f.result,sizeof got));
    if(writer){CHECK(WaitForSingleObject(writer,3000)==WAIT_OBJECT_0);DWORD code=1;CHECK(GetExitCodeThread(writer,&code)&&!code);CHECK(CloseHandle(writer));}
    HANDLE original=r.thread;
    for(unsigned id=2;id<=65;id++){
        wslp_head head={WSLP_PERMISSION_RESULT,sizeof got,id};f.result.state.inode=id;DWORD wrote;
        CHECK(WriteFile(output,&head,sizeof head,&wrote,NULL)&&wrote==sizeof head);
        CHECK(WriteFile(output,&f.result,sizeof f.result,&wrote,NULL)&&wrote==sizeof f.result);
        if(id==65){CHECK(CloseHandle(output));output=NULL;}
        CHECK(!metadata_receive(&r,WSLP_PERMISSION_RESULT,id,&got,sizeof got,3000));
        CHECK(got.state.inode==id&&r.thread==original);
    }
    metadata_pipe_close(&r,input,output);
}
static void test_metadata_reader_rejects_headers_before_body(void)
{
    for(unsigned bad=0;bad<5;bad++){
        metadata_reader r;HANDLE input,output;if(!metadata_pipe(&r,&input,&output)){CHECK(0);return;}
        wslp_permission_result got;memset(&got,0x5a,sizeof got);wslp_permission_result before=got;
        wslp_head head={WSLP_PERMISSION_RESULT,sizeof got,3};DWORD wrote;
        if(bad==0)head.type=WSLP_OWNERSHIP_RESULT;
        if(bad==1)head.id=2;if(bad==2)head.size=0;if(bad==3)head.size=sizeof got+1;if(bad==4)head.size=UINT32_MAX;
        CHECK(WriteFile(output,&head,sizeof head,&wrote,NULL)&&wrote==sizeof head);
        /* Writer remains open and sends no body: reject the header rather
         * than reading its advertised size or waiting for EOF/timeout. */
        CHECK(metadata_receive(&r,WSLP_PERMISSION_RESULT,3,&got,sizeof got,1000)==ERROR_INVALID_DATA);
        CHECK(!memcmp(&got,&before,sizeof got));
        metadata_pipe_close(&r,input,output);
    }
}
static void test_metadata_reader_truncated_replies(void)
{
    for(unsigned body=0;body<2;body++){
        metadata_reader r;HANDLE input,output;if(!metadata_pipe(&r,&input,&output)){CHECK(0);return;}
        wslp_head head={WSLP_PERMISSION_RESULT,sizeof(wslp_permission_result),1};DWORD wrote;
        CHECK(WriteFile(output,&head,body?sizeof head:3,&wrote,NULL));
        if(body){BYTE bytes[3]={1,2,3};CHECK(WriteFile(output,bytes,sizeof bytes,&wrote,NULL));}
        CHECK(CloseHandle(output));wslp_permission_result got={0};
        CHECK(metadata_receive(&r,WSLP_PERMISSION_RESULT,1,&got,sizeof got,1000)==ERROR_BROKEN_PIPE);
        metadata_pipe_close(&r,input,NULL);
    }
}
static void test_metadata_reader_timeout_and_shutdown(void)
{
    for(unsigned phase=0;phase<3;phase++)for(unsigned repeat=0;repeat<12;repeat++){
        metadata_reader r;HANDLE input,output;if(!metadata_pipe(&r,&input,&output)){CHECK(0);return;}
        if(phase){
            wslp_head head={WSLP_PERMISSION_RESULT,sizeof(wslp_permission_result),1};DWORD wrote;
            if(phase==2){CHECK(WriteFile(output,&head,sizeof head,&wrote,NULL));}
            wslp_permission_result got={0};
            CHECK(metadata_receive(&r,WSLP_PERMISSION_RESULT,1,&got,sizeof got,repeat%2?5:0)==ERROR_TIMEOUT);
        }
        /* Idle wake, blocked header and blocked body must all drain before
         * releasing the pipe/storage. A zero-timeout wait also races startup. */
        metadata_pipe_close(&r,input,output);
    }
}
static void prepared_direct(windows_prepare *q,windows_file *w,wchar_t *target,uint64_t token)
{
    char suffix[100];snprintf(suffix,sizeof suffix,"/prepared-%u.bin",++serial);
    wchar_t leaf[100];_snwprintf(leaf,100,L"prepared-%u.bin",serial);path_join_test(target,windows_root,leaf);
    wslp_prepared_file f={.file={.atime=stamp,.mtime=stamp,.path_bytes=(uint32_t)strlen(suffix)},.token=token,.bytes=strlen(payload)};
    CHECK(!windows_prepare_begin(q,w,&f,(const uint8_t*)suffix,windows_root,1));CHECK(!w->err&&w->created);
    DWORD wrote=0;CHECK(WriteFile(w->file,payload,(DWORD)strlen(payload),&wrote,NULL)&&wrote==strlen(payload));
    CHECK(!windows_prepare_data(q,wrote));wslp_file_result credit=windows_prepare_end(q,w,0,NULL);
    CHECK(credit.token==token&&!credit.error&&!credit.pad);CHECK(!exists_test(target));CHECK(!w->active);
}
static void prepared_observe(windows_prepare *q)
{
    memset(&T,0,sizeof T);CHECK(q->count!=0);T.file=q->slots[0].file.file;
    CHECK(GetFileInformationByHandle(T.file,&T.identity));
}
static void test_prepared_windows_credits_and_serial_durable_commits(void)
{
    for(unsigned width=2;width<=8;width*=2){
        windows_prepare q={.width=width};windows_file w={0};wchar_t targets[8][4096];memset(&T,0,sizeof T);
        for(unsigned i=0;i<width;i++){prepared_direct(&q,&w,targets[i],i+1);CHECK(q.count==i+1&&q.active_token==0);CHECK(!T.flushes&&!T.renames);}
        for(unsigned i=0;i<width;i++){
            prepared_observe(&q);wslp_file_result command={i+1,0,0},result={0};CHECK(!windows_prepare_commit(&q,&command,NULL,&result));
            CHECK(result.token==i+1&&!result.error&&!result.pad);CHECK(q.count==width-i-1);
            CHECK(T.flushes==2&&T.renames==1&&T.closes==1&&T.directory_flushes==1);CHECK(!strcmp(T.order,"MB1R2C"));
            CHECK(contents_test(targets[i],payload,strlen(payload)));
            HANDLE file=CreateFileW(targets[i],FILE_READ_ATTRIBUTES,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);FILETIME mt;
            CHECK(file!=INVALID_HANDLE_VALUE&&GetFileTime(file,NULL,NULL,&mt));CHECK(((uint64_t)mt.dwHighDateTime<<32|mt.dwLowDateTime)==(uint64_t)(stamp/100+116444736000000000LL));if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
        }
        no_workspace();for(unsigned i=0;i<width;i++)CHECK(DeleteFileW(targets[i]));windows_abort(&w);windows_prepare_abort(&q);
    }
}
static void test_prepared_windows_bounds_tokens_and_cleanup(void)
{
    windows_prepare q={.width=2};windows_file w={0};wchar_t target[4096];memset(&T,0,sizeof T);prepared_direct(&q,&w,target,1);
    wslp_file_result invalid={2,0,0},result;CHECK(windows_prepare_commit(&q,&invalid,NULL,&result)==ERROR_INVALID_DATA);CHECK(q.count==1&&!T.flushes);
    invalid=(wslp_file_result){1,0,1};CHECK(windows_prepare_commit(&q,&invalid,NULL,&result)==ERROR_INVALID_DATA);
    invalid=(wslp_file_result){1,-1,0};CHECK(windows_prepare_commit(&q,&invalid,NULL,&result)==ERROR_INVALID_DATA);
    invalid=(wslp_file_result){1,4096,0};CHECK(windows_prepare_commit(&q,&invalid,NULL,&result)==ERROR_INVALID_DATA);
    for(unsigned cause=0;cause<6;cause++){
        const uint8_t suffix[]="/invalid.bin";wslp_prepared_file f={.file={.path_bytes=12},.token=2,.bytes=4};
        if(cause==0)f.bytes=WSLP_PREPARE_BYTES+1;if(cause==1)f.token=1;if(cause==2)f.file.replace=1;if(cause==3)f.file.pad=1;if(cause==4)f.bytes=0;if(cause==5)q.width=1;
        CHECK(windows_prepare_begin(&q,&w,&f,suffix,windows_root,0)==ERROR_INVALID_DATA);CHECK(!w.active);q.width=2;
    }
    prepared_observe(&q);windows_prepare_abort(&q);CHECK(!q.count&&!exists_test(target));CHECK(T.closes==1);no_workspace();
    /* Incorrect byte credit cannot retain a stage or publish a target. */
    const uint8_t suffix[]="/short-prepared.bin";wslp_prepared_file f={.file={.path_bytes=19},.token=2,.bytes=4};
    memset(&T,0,sizeof T);CHECK(!windows_prepare_begin(&q,&w,&f,suffix,windows_root,0));CHECK(!w.err);
    CHECK(windows_prepare_data(&q,5)==ERROR_INVALID_DATA);CHECK(!windows_prepare_data(&q,3));
    result=windows_prepare_end(&q,&w,0,NULL);CHECK(result.token==2&&result.error&&!q.count&&!w.active);CHECK(!T.renames&&!T.flushes);no_workspace();
}
static void test_prepared_windows_failed_barriers_cancel_and_destination_race(void)
{
    for(unsigned cause=0;cause<9;cause++){
        windows_prepare q={.width=2};windows_file w={0};wchar_t target[4096],second[4096];memset(&T,0,sizeof T);
        prepared_direct(&q,&w,target,1);prepared_direct(&q,&w,second,2);prepared_observe(&q);volatile LONG stop=0;T.stop=&stop;
        if(cause<2){T.fail_flush=cause+1;T.flush_error=ERROR_WRITE_FAULT;}
        if(cause==2)stop=1;if(cause==3)T.cancel_flush=1;if(cause==4)T.cancel_flush=2;
        if(cause==5)write_test(target,"foreign",7);
        if(cause==7)T.fail_directory_flush=1;if(cause==8)T.cancel_directory_flush=1;
        wslp_file_result command={1,cause==6?116:0,0},result={0};CHECK(!windows_prepare_commit(&q,&command,&stop,&result));CHECK(result.token==1&&result.error);
        if(cause==1||cause==4||cause==7||cause==8)CHECK(contents_test(target,payload,strlen(payload)));
        else if(cause==5)CHECK(contents_test(target,"foreign",7));else CHECK(!exists_test(target));
        CHECK(!exists_test(second));windows_prepare_abort(&q);windows_abort(&w);no_workspace();CHECK(!exists_test(second));
        if(exists_test(target))CHECK(DeleteFileW(target));
    }
}


/* Synthetic peer only; all held NTFS stages, writeouts, fences and renames are
 * real. The post-API hook injects reported errors/cancellation without replacing
 * the persistence operation. Real pipe framing has separate reader tests. */
static void group_trial(unsigned width)
{
    windows_prepare q={.width=width<2?2:width,.group=1};windows_file w={0};wchar_t targets[8][4096];
    job_reader r={.ready=CreateEventW(NULL,FALSE,FALSE,NULL),.consumed=CreateEventW(NULL,FALSE,FALSE,NULL),.buf=fpe_alloc(READ_BUF)};
    HANDLE input=NULL,output=NULL;CHECK(CreatePipe(&input,&output,NULL,4096));CHECK(r.ready&&r.consumed&&r.buf);
    G.reader=&r;G.replies=0;group_testing=1;memset(&T,0,sizeof T);
    for(unsigned i=0;i<width;i++)prepared_direct(&q,&w,targets[i],i+1);
    wslp_group command={.count=width};for(unsigned i=0;i<width;i++)command.files[i].token=i+1;
    int cancel_sent=0;fpe_err error=windows_group_commit(&q,&command,8+width*sizeof(wslp_file_result),output,&r,G.stop,&cancel_sent);
    CHECK(!q.count);CHECK(error==(G.bad_check?ERROR_INVALID_DATA:0));CHECK(G.replies==(G.bad_check?0:1));
    if(!G.bad_check){CHECK(G.result.count==width&&!G.result.pad);CHECK(G.at&&G.order[G.at-1]=='A');}
    if(width>1&&G.renames){CHECK(G.pre==1&&G.post==1&&G.parent==1);CHECK(strstr(G.order,"fqr")!=NULL);CHECK(strstr(G.order,"FDA")!=NULL||G.bad_check);}
    unsigned visible=0,credited=0;
    for(unsigned i=0;i<width;i++){
        if(!G.bad_check){CHECK(G.result.files[i].token==i+1&&!G.result.files[i].pad);if(!G.result.files[i].error)credited++;}
        if(exists_test(targets[i])){
            visible++;CHECK(contents_test(targets[i],payload,strlen(payload)));
            HANDLE file=CreateFileW(targets[i],FILE_READ_ATTRIBUTES,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);FILETIME mt;
            CHECK(file!=INVALID_HANDLE_VALUE&&GetFileTime(file,NULL,NULL,&mt));CHECK(((uint64_t)mt.dwHighDateTime<<32|mt.dwLowDateTime)==(uint64_t)(stamp/100+116444736000000000LL));
            if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);CHECK(!(GetFileAttributesW(targets[i])&(FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_TEMPORARY)));CHECK(DeleteFileW(targets[i]));
        }else if(!G.bad_check)CHECK(G.result.files[i].error!=0);
    }
    CHECK(visible==G.renames);CHECK(credited<=visible);
    if(G.fail&&(!strcmp(G.fail,"group.post_fence")||!strcmp(G.fail,"group.namespace")))CHECK(credited==0&&visible==width);
    if(G.fail&&(!strcmp(G.fail,"group.pre_fence")||!strcmp(G.fail,"group.pre_writeout")))CHECK(credited==0&&visible==0);
    if(G.cancel){CHECK(credited==visible);CHECK(visible==(!strcmp(G.cancel,"group.post_writeout")?1:0));CHECK(cancel_sent);}
    if(G.check_error_at)CHECK(visible==G.check_error_at-1&&credited==visible);
    if(!G.fail&&!G.cancel&&!G.check_error_at&&!G.bad_check)CHECK(credited==width);
    if(G.fail&&!strcmp(G.fail,"group.post_writeout"))CHECK(visible==G.after&&credited==G.after-1);
    no_workspace();windows_prepare_abort(&q);windows_abort(&w);job_reader_stop(&r);CHECK(CloseHandle(input));CHECK(CloseHandle(output));group_testing=0;memset(&T,0,sizeof T);
}
static void test_group_commit_order_and_metadata(void)
{
    wchar_t prior[32];DWORD n=GetEnvironmentVariableW(L"OPENPILOT_WINDOWS_GROUP_COMMIT",prior,32);CHECK(n<32);
    for(unsigned width=2;width<=8;width*=2){memset(&G,0,sizeof G);group_trial(width);CHECK(G.writeouts==width&&G.checks==width&&G.renames==width);}
    SetEnvironmentVariableW(L"OPENPILOT_WINDOWS_GROUP_COMMIT",NULL);CHECK(windows_group_requested(windows_root));
    SetEnvironmentVariableW(L"OPENPILOT_WINDOWS_GROUP_COMMIT",L"1");CHECK(windows_group_requested(windows_root));CHECK(!windows_group_requested(linux_root));
    SetEnvironmentVariableW(L"OPENPILOT_WINDOWS_GROUP_COMMIT",L"0");CHECK(!windows_group_requested(windows_root));
    SetEnvironmentVariableW(L"OPENPILOT_WINDOWS_GROUP_COMMIT",L"true");CHECK(!windows_group_requested(windows_root));SetEnvironmentVariableW(L"OPENPILOT_WINDOWS_GROUP_COMMIT",n&&n<32?prior:NULL);
}
static void test_group_commit_barrier_failures_and_cancel(void)
{
    const char *tags[]={"group.pre_writeout","group.pre_fence","group.post_writeout","group.post_fence","group.namespace"};
    for(unsigned width=4;width<=8;width*=2){
        for(unsigned i=0;i<5;i++){memset(&G,0,sizeof G);G.fail=tags[i];G.after=i==0||i==2?2:1;group_trial(width);CHECK(G.seen>=G.after);}
        for(unsigned i=0;i<2;i++){memset(&G,0,sizeof G);volatile LONG stop=0;G.stop=&stop;G.cancel=i?"group.post_writeout":"group.pre_writeout";G.after=1;group_trial(width);CHECK(stop==1);}
    }
}
static void test_group_commit_fresh_source_refusal_and_bad_replies(void)
{
    for(unsigned width=4;width<=8;width*=2){
        memset(&G,0,sizeof G);G.check_error_at=2;G.check_error=116;group_trial(width);
        for(unsigned bad=1;bad<=3;bad++){memset(&G,0,sizeof G);G.bad_check=bad;group_trial(width);CHECK(G.renames==0);}
    }
}
static void test_group_commit_malformed_commands_and_fallback(void)
{
    windows_prepare q={.width=4,.group=1};windows_file w={0};wchar_t targets[2][4096];memset(&T,0,sizeof T);
    for(unsigned i=0;i<2;i++)prepared_direct(&q,&w,targets[i],i+1);
    for(unsigned i=0;i<17;i++){
        wslp_group c={.count=2,.files={{1,0,0},{2,0,0}}};unsigned size=40;int cancel=0;q.group=1;q.active_token=0;q.count=2;q.width=4;
        switch(i){case 0:size=0;break;case 1:size=8;break;case 2:size=39;break;case 3:size=41;break;case 4:c.count=0;break;case 5:c.count=9;break;case 6:c.pad=1;break;case 7:c.files[0].token=9;break;case 8:c.files[1].token=1;break;case 9:c.files[0].pad=1;break;case 10:c.files[0].error=-1;break;case 11:c.files[0].error=4096;break;case 12:q.group=0;break;case 13:q.active_token=3;break;case 14:q.count=9;size=152;break;case 15:q.width=1;break;case 16:q.count=0;size=8;break;}
        CHECK(windows_group_commit(&q,&c,size,NULL,NULL,NULL,&cancel)==ERROR_INVALID_DATA);CHECK(!exists_test(targets[0])&&!exists_test(targets[1]));
    }
    q.count=2;q.active_token=0;group_testing=1;windows_prepare_abort(&q);group_testing=0;memset(&T,0,sizeof T);no_workspace();
    memset(&G,0,sizeof G);group_trial(1);CHECK(!G.pre&&!G.post&&!G.parent&&G.ordinary==3&&G.checks==1);
}

typedef void (*test_fn)(void);
int wmain(int argc,wchar_t **argv)
{
    if(argc!=3){fputs("Pass private Windows and WSL UNC fixture directories.\n",stderr);return 2;}
    windows_root=argv[1];linux_root=argv[2];
    if(!exists_test(windows_root)||!exists_test(linux_root)||!fpe_wsl_path_kind(linux_root))return 2;
    fpe_wsl_configure(&(fpe_wsl_config){1,0,60000});test_engine=fpe_engine_create(NULL);CHECK(test_engine!=NULL);if(!test_engine)return 1;
    test_fn tests[]={test_group_commit_order_and_metadata,test_group_commit_barrier_failures_and_cancel,test_group_commit_fresh_source_refusal_and_bad_replies,test_group_commit_malformed_commands_and_fallback,test_prepared_windows_credits_and_serial_durable_commits,test_prepared_windows_bounds_tokens_and_cleanup,test_prepared_windows_failed_barriers_cancel_and_destination_race,test_metadata_reader_fragmented_and_repeated_replies,test_metadata_reader_rejects_headers_before_body,test_metadata_reader_truncated_replies,test_metadata_reader_timeout_and_shutdown,test_wsl_single_unit_destinations_copy_move_and_selected,test_flush_order_and_final_metadata,test_prepublication_flush_failure,test_postpublication_flush_failure,test_existing_errors_abort_and_cleanup,test_cancellation_during_flush,test_guarded_parent_publication,test_destination_version_changes_are_preserved,test_destination_commit_blocks_new_writers,test_wsl_move_requires_successful_flush_ack,test_wsl_success_ack_and_source_removal,test_wsl_cancellation_during_flush_retains_source,test_wsl_selected_copy_flush_ack,test_wsl_destination_races_reject_ack_and_preserve_source,test_overwrite_dacl_is_flushed_before_publication,test_dacl_errors_and_cancellation_never_publish,test_wsl_overwrite_dacl_and_failed_security_ack,test_windows_source_inventory_is_fail_closed,test_windows_source_request_rejects_foreign_paths,test_windows_named_streams_never_disappear_in_linux_moves,test_windows_tree_stream_refusal_retains_unsupported_nodes};
    const char *names[]={"test_group_commit_order_and_metadata","test_group_commit_barrier_failures_and_cancel","test_group_commit_fresh_source_refusal_and_bad_replies","test_group_commit_malformed_commands_and_fallback","test_prepared_windows_credits_and_serial_durable_commits","test_prepared_windows_bounds_tokens_and_cleanup","test_prepared_windows_failed_barriers_cancel_and_destination_race","test_metadata_reader_fragmented_and_repeated_replies","test_metadata_reader_rejects_headers_before_body","test_metadata_reader_truncated_replies","test_metadata_reader_timeout_and_shutdown","test_wsl_single_unit_destinations_copy_move_and_selected","test_flush_order_and_final_metadata","test_prepublication_flush_failure","test_postpublication_flush_failure","test_existing_errors_abort_and_cleanup","test_cancellation_during_flush","test_guarded_parent_publication","test_destination_version_changes_are_preserved","test_destination_commit_blocks_new_writers","test_wsl_move_requires_successful_flush_ack","test_wsl_success_ack_and_source_removal","test_wsl_cancellation_during_flush_retains_source","test_wsl_selected_copy_flush_ack","test_wsl_destination_races_reject_ack_and_preserve_source","test_overwrite_dacl_is_flushed_before_publication","test_dacl_errors_and_cancellation_never_publish","test_wsl_overwrite_dacl_and_failed_security_ack","test_windows_source_inventory_is_fail_closed","test_windows_source_request_rejects_foreign_paths","test_windows_named_streams_never_disappear_in_linux_moves","test_windows_tree_stream_refusal_retains_unsupported_nodes"};
    for(unsigned i=0;i<sizeof tests/sizeof *tests;i++){int before=failed;uint64_t start=GetTickCount64();tests[i]();printf("CASE %s %s %llu\n",names[i],failed==before?"passed":"failed",(unsigned long long)(GetTickCount64()-start));fflush(stdout);}
    memset(&T,0,sizeof T);fpe_engine_destroy(test_engine);fpe_wsl_shutdown();
    printf("%d passed, %d failed; 32 selectors, no skips\n",passed,failed);return failed?1:0;
}
