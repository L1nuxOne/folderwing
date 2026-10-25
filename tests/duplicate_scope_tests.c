/* Production duplicate engine with deterministic native traversal records.
 * Data hashing/comparison uses actual owned Windows files; no WSL or mounts. */
#include "../app/duplicates.h"
#include "../app/space.h"
#include "../app/termutil.h"
#include <stdio.h>
#include <string.h>
static DWORD scope_scan(const wchar_t *,int,int,HANDLE,space_emit,void *);
static DWORD scope_reader_init(du_reader *,const wchar_t *,int,int,HANDLE);
#define space_scan scope_scan
#define duplicate_reader_init scope_reader_init
#include "../app/duplicates.c"
#undef space_scan
#undef duplicate_reader_init
static int passed,failed,mode,calls[6],sequence[32],sequence_count;
static wchar_t backing[32768],mounted[32768];static du_stamp first_stamp,second_stamp;
enum {EXCLUDED,ORDINARY,UNFINISHED,PARTIAL};
#define CHECK(label,c) do{if(c){passed++;printf("ok %s\n",label);}else{failed++;printf("FAIL %d %s (error %lu)\n",__LINE__,label,(unsigned long)GetLastError());}}while(0)
static const wchar_t *parent=L"\\\\wsl.localhost\\AuditFixture\\scope",*child=L"\\\\wsl.localhost\\AuditFixture\\scope\\mount",*deep=L"\\\\wsl.localhost\\AuditFixture\\scope\\mount\\deep",*alias=L"\\\\wsl$\\AuditFixture\\scope\\mount\\";
static int kind(const wchar_t *root){wchar_t distro[128],path[32768];if(!term_wsl_split(root,distro,128,path,32768))return -1;size_t n=wcslen(path);while(n>1&&path[n-1]=='/')path[--n]=0;if(!wcscmp(path,L"/scope"))return 0;if(!wcscmp(path,L"/scope/mount"))return 1;if(!wcscmp(path,L"/scope/mount/deep"))return 2;if(!wcscmp(path,L"/scope/MOUNT"))return 3;if(!wcscmp(path,L"/scope/mountain"))return 4;return -1;}
static DWORD scope_reader_init(du_reader *r,const wchar_t *root,int wsl,int as_root,HANDLE cancel){(void)as_root;int k=kind(root);if(!wsl||k<0)return ERROR_NOT_SUPPORTED;return duplicate_reader_init(r,k?mounted:backing,0,0,cancel);}
static int emit_file(space_emit emit,void *user,uint32_t id,uint32_t parent,const wchar_t *name,du_stamp stamp){sp_record r={.magic=SP_MAGIC,.type=SP_ENTRY,.id=id,.parent=parent,.logical=stamp.size,.device=stamp.device,.identity=stamp.identity};return emit(user,&r,name);}
static DWORD scope_scan(const wchar_t *root,int wsl,int as_root,HANDLE cancel,space_emit emit,void *user){(void)as_root;int k=kind(root);if(!wsl||k<0)return ERROR_NOT_SUPPORTED;calls[k]++;if(sequence_count<32)sequence[sequence_count++]=k;if(cancel&&WaitForSingleObject(cancel,0)==WAIT_OBJECT_0)return ERROR_CANCELLED;
 sp_record r={.magic=SP_MAGIC,.type=SP_ENTRY,.parent=SP_NONE,.flags=SP_DIR,.device=first_stamp.device,.identity=(uint64_t)(100+k)};if(!emit(user,&r,L""))return ERROR_CANCELLED;
 if(k){if(!emit_file(emit,user,1,0,L"b.txt",second_stamp))return ERROR_CANCELLED;}
 else{if(!emit_file(emit,user,1,0,L"a.txt",first_stamp)||!emit_file(emit,user,2,0,L"alias.txt",first_stamp))return ERROR_CANCELLED;
  r=(sp_record){.magic=SP_MAGIC,.type=SP_ENTRY,.id=3,.parent=0,.flags=mode==EXCLUDED?SP_SKIP:SP_DIR,.error=mode==EXCLUDED?18:0,.device=first_stamp.device,.identity=101};if(!emit(user,&r,L"mount"))return ERROR_CANCELLED;
  if(mode!=EXCLUDED){if(!emit_file(emit,user,4,3,L"b.txt",second_stamp))return ERROR_CANCELLED;if(mode!=UNFINISHED){r=(sp_record){.magic=SP_MAGIC,.type=SP_LEAVE,.id=3};if(!emit(user,&r,NULL))return ERROR_CANCELLED;}}
 }
 r=(sp_record){.magic=SP_MAGIC,.type=SP_LEAVE};if(!emit(user,&r,NULL))return ERROR_CANCELLED;r=(sp_record){.magic=SP_MAGIC,.type=SP_END,.flags=!k&&mode==PARTIAL?SP_PARTIAL:SP_COMPLETE};return emit(user,&r,NULL)?0:ERROR_CANCELLED;
}
static duplicate_result run(const wchar_t *const *roots,uint32_t count,int scan_mode,HANDLE cancel){mode=scan_mode;memset(calls,0,sizeof calls);sequence_count=0;duplicate_result r;duplicates_scan(roots,count,1,0,cancel,NULL,NULL,&r);return r;}
static void test_duplicate_scope_keeps_explicit_mount_roots(void){
 const wchar_t *one[]={parent};duplicate_result r=run(one,1,EXCLUDED,NULL);CHECK("parent alone respects a non-failing mount exclusion",r.state==DUP_COMPLETE&&!r.errors&&!r.groups&&r.count==1&&r.skipped==1&&r.aliases==1&&!calls[1]);duplicates_free(&r);
 const wchar_t *both[]={parent,child};r=run(both,2,EXCLUDED,NULL);CHECK("explicit mount root reaches scanner despite excluded parent entry",calls[0]==1&&calls[1]==1&&sequence_count==2);CHECK("same-device mount case finds a real verified cross-root group",r.state==DUP_COMPLETE&&!r.errors&&r.groups==1&&r.members==2&&r.count==2&&r.duplicate_bytes==4&&r.aliases==1&&r.skipped==1);int roots_ok=0;for(uint32_t i=0;i<r.members;i++)roots_ok|=1<<r.files[i].root;CHECK("results retain both caller root indices",roots_ok==3);duplicates_free(&r);
 const wchar_t *reverse[]={child,parent,alias};r=run(reverse,3,EXCLUDED,NULL);CHECK("parent-first scheduling handles child-first requests and repeated UNC aliases",sequence_count==2&&sequence[0]==0&&calls[0]==1&&calls[1]==1);CHECK("alias root does not inflate groups, files or hardlink counters",r.state==DUP_COMPLETE&&r.groups==1&&r.members==2&&r.count==2&&r.aliases==1&&r.duplicate_bytes==4);roots_ok=0;for(uint32_t i=0;i<r.members;i++)roots_ok|=1<<r.files[i].root;CHECK("scheduling keeps original root authority after reordering",roots_ok==3);duplicates_free(&r);
 const wchar_t *below[]={parent,deep};r=run(below,2,EXCLUDED,NULL);CHECK("explicit scope below an excluded mount is also scanned",calls[2]==1&&r.state==DUP_COMPLETE&&r.groups==1&&r.members==2);duplicates_free(&r);
}
static void test_duplicate_scope_requires_completed_coverage(void){
 const wchar_t *roots[]={child,parent,alias};duplicate_result r=run(roots,3,ORDINARY,NULL);CHECK("ordinary native overlap is suppressed only after parent traversal",calls[0]==1&&!calls[1]&&sequence_count==1);CHECK("ordinary overlap retains exact group and hardlink counts",r.state==DUP_COMPLETE&&!r.errors&&r.groups==1&&r.members==2&&r.count==2&&r.aliases==1&&!r.skipped&&r.entries==5);duplicates_free(&r);
 r=run(roots,3,UNFINISHED,NULL);CHECK("a listed but unfinished directory does not prove child coverage",calls[1]==1&&r.groups==1&&r.count==2&&r.members==2);duplicates_free(&r);
 r=run(roots,3,PARTIAL,NULL);CHECK("an incomplete parent cannot suppress an explicit child request",calls[0]==1&&calls[1]==1&&r.state==DUP_PARTIAL&&r.errors&&r.groups==1&&r.members==2);duplicates_free(&r);
 HANDLE cancel=CreateEventW(NULL,TRUE,TRUE,NULL);r=run(roots,3,EXCLUDED,cancel);CHECK("cancelled request neither scans nor reports complete",!sequence_count&&r.state==DUP_STOPPED&&!r.groups);duplicates_free(&r);CloseHandle(cancel);
}
static void test_duplicate_scope_namespace_and_name_boundaries(void){
 const wchar_t *other=L"\\\\wsl.localhost\\OtherFixture\\scope\\mount";const wchar_t *roots[]={parent,other};duplicate_result r=run(roots,2,ORDINARY,NULL);CHECK("coverage and physical identities never cross distro namespaces",calls[0]==1&&calls[1]==1&&r.state==DUP_COMPLETE&&r.members==3&&r.count==3&&r.duplicate_bytes==8);duplicates_free(&r);
 const wchar_t *upper[]={parent,L"\\\\wsl.localhost\\AuditFixture\\scope\\MOUNT"};r=run(upper,2,ORDINARY,NULL);CHECK("native scope matching remains case sensitive",calls[3]==1&&r.state==DUP_COMPLETE&&r.count==2&&r.members==2);duplicates_free(&r);
 const wchar_t *near_roots[]={child,L"\\\\wsl.localhost\\AuditFixture\\scope\\mountain"};r=run(near_roots,2,EXCLUDED,NULL);CHECK("lexical prefix lookalike is a separate explicit scope",calls[1]==1&&calls[4]==1&&sequence_count==2);duplicates_free(&r);
 sp_model m={0};space_model_record(&m,&(sp_record){.magic=SP_MAGIC,.type=SP_ENTRY,.parent=SP_NONE,.flags=SP_DIR},L"");space_model_record(&m,&(sp_record){.magic=SP_MAGIC,.type=SP_ENTRY,.id=1,.parent=0,.flags=SP_DIR},L"mo\uf03aunt");space_model_record(&m,&(sp_record){.magic=SP_MAGIC,.type=SP_LEAVE,.id=1},NULL);space_model_record(&m,&(sp_record){.magic=SP_MAGIC,.type=SP_LEAVE,.id=0},NULL);space_model_record(&m,&(sp_record){.magic=SP_MAGIC,.type=SP_END,.flags=SP_COMPLETE},NULL);
 CHECK("coverage resolves exact encoded Linux components and UNC aliases",native_scope_covered(&m,parent,L"\\\\wsl$\\AuditFixture\\scope\\mo\uf03aunt\\"));CHECK("encoded name proof does not admit a different component",!native_scope_covered(&m,parent,child));space_model_free(&m);
}
static int make_file(const wchar_t *path,du_stamp *stamp){HANDLE h=CreateFileW(path,GENERIC_WRITE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,CREATE_NEW,0,NULL);if(h==INVALID_HANDLE_VALUE)return 0;DWORD n=0;BY_HANDLE_FILE_INFORMATION i;int ok=WriteFile(h,"same",4,&n,NULL)&&n==4&&GetFileInformationByHandle(h,&i);CloseHandle(h);if(ok)*stamp=(du_stamp){4,i.dwVolumeSerialNumber,((uint64_t)i.nFileIndexHigh<<32)|i.nFileIndexLow,0,0};return ok;}
int wmain(void){setvbuf(stdout,NULL,_IONBF,0);wchar_t temp[32768],file[32768],link[32768];GetTempPathW(32768,temp);swprintf(backing,32768,L"%lsopenpilot-duplicate-scope-%lu-%llu",temp,GetCurrentProcessId(),(unsigned long long)GetTickCount64());swprintf(mounted,32768,L"%ls\\mount",backing);CHECK("owned backing directory",CreateDirectoryW(backing,NULL));CHECK("owned child backing directory",CreateDirectoryW(mounted,NULL));swprintf(file,32768,L"%ls\\a.txt",backing);CHECK("first real content and identity",make_file(file,&first_stamp));swprintf(link,32768,L"%ls\\alias.txt",backing);CHECK("real hardlink control",CreateHardLinkW(link,file,NULL));swprintf(file,32768,L"%ls\\b.txt",mounted);CHECK("second independent real content and identity",make_file(file,&second_stamp));if(first_stamp.identity&&second_stamp.identity){test_duplicate_scope_keeps_explicit_mount_roots();test_duplicate_scope_requires_completed_coverage();test_duplicate_scope_namespace_and_name_boundaries();}else failed++;
 DeleteFileW(file);swprintf(file,32768,L"%ls\\a.txt",backing);DeleteFileW(file);DeleteFileW(link);RemoveDirectoryW(mounted);RemoveDirectoryW(backing);printf("%d passed, %d failed; 3 duplicate scope selectors\n",passed,failed);return failed?1:0;}
