/* Exact production Archive worker and ordinary Diff reader, with observed
 * provider-open/decoder-launch boundaries. No native/provider alias I/O. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winnetwk.h>
#include <stdio.h>
#include <string.h>
#include "../app/previewlaunch.h"
static volatile LONG opens,launches;
static HANDLE WINAPI observed_open(LPCWSTR p,DWORD a,DWORD s,LPSECURITY_ATTRIBUTES x,DWORD c,DWORD f,HANDLE t){InterlockedIncrement(&opens);return CreateFileW(p,a,s,x,c,f,t);}
static preview_launch *observed_launch(const wchar_t *exe,HANDLE in,HANDLE out,HANDLE section,SIZE_T cap,HWND hwnd,UINT msg,uint32_t generation){(void)exe;(void)in;(void)out;(void)section;(void)cap;(void)hwnd;(void)msg;(void)generation;InterlockedIncrement(&launches);return NULL;}
static UINT WINAPI mapped_drive(LPCWSTR drive){if(drive[0]==L'Q'||drive[0]==L'R')return DRIVE_REMOTE;return GetDriveTypeW(drive);}
static DWORD WINAPI mapped_share(LPCWSTR local,LPWSTR share,LPDWORD size){const wchar_t *value=local[0]==L'Q'?L"\\\\wsl.localhost\\Ubuntu\\quoted\uf080\uf080parent":L"\\\\ordinary-server\\share";if(local[0]!=L'Q'&&local[0]!=L'R')return WNetGetConnectionW(local,share,size);size_t n=wcslen(value)+1;if(*size<n){*size=(DWORD)n;return ERROR_MORE_DATA;}wcscpy(share,value);return NO_ERROR;}
#define GetDriveTypeW mapped_drive
#define WNetGetConnectionW mapped_share
#include "../app/providerpath.h"
#undef GetDriveTypeW
#undef WNetGetConnectionW
#define CreateFileW observed_open
#define preview_launch_begin observed_launch
#define cancelled archive_cancelled
#include "../app/archiveclient.c"
#undef cancelled
#include "../app/diffread.c"
#undef CreateFileW
#undef preview_launch_begin
static int passed,failed;
#define CHECK(label,c) do{if(c){passed++;printf("ok %s\n",label);}else{failed++;printf("FAIL %d %s\n",__LINE__,label);}}while(0)
static const wchar_t *bad[]={L"\\\\wsl.localhost\\Ubuntu\\quoted\uf080\uf080file.zip",L"\\\\wsl$\\Ubuntu\\parent\uf080\uf080x\\file.dat",L"\\\\?\\UNC\\wsl.localhost\\Ubuntu\\parent\uf080\uf080x\\file.zip",L"\\\\?\\UNC\\wsl$\\Ubuntu\\quoted\uf080\uf03afile.txt",L"Q:\\file.zip"};
static void test_provider_namespace_admission(void){int rejected=1;for(unsigned i=0;i<sizeof bad/sizeof *bad;i++)rejected&=provider_path_check(bad[i],1)==ERROR_NOT_SUPPORTED&&provider_path_check(bad[i],0)==ERROR_NOT_SUPPORTED;CHECK("native-only leaf, ancestor, extended UNC and mapped-share identities refused",rejected);
 CHECK("ordinary WSL provider path obeys enablement",!provider_path_check(L"\\\\wsl.localhost\\Ubuntu\\ordinary.txt",1)&&provider_path_check(L"\\\\wsl.localhost\\Ubuntu\\ordinary.txt",0)==ERROR_NOT_SUPPORTED);
 CHECK("extended WSL namespace cannot bypass disabled policy",provider_path_check(L"\\\\?\\UNC\\wsl$\\Ubuntu\\ordinary.txt",0)==ERROR_NOT_SUPPORTED);
 CHECK("legacy punctuation aliases remain provider-eligible",!provider_path_check(L"\\\\wsl$\\Ubuntu\\a\uf03ab.txt",1));
 CHECK("literal quote code points in Windows namespaces remain legal",!provider_path_check(L"C:\\literal\uf080name.zip",0)&&!provider_path_check(L"\\\\ordinary-server\\share\\literal\uf080name.txt",0)&&!provider_path_check(L"R:\\literal\uf080name.txt",0));
 CHECK("lookalike server is not the WSL namespace",!provider_path_check(L"\\\\wsl.localhost.example\\share\\literal\uf080name",0));}
static void test_provider_refusal_precedes_archive_and_diff_io(void){arc_client *client=arc_client_start(NULL,0);CHECK("actual archive worker created",client!=NULL);if(!client)return;int refused=1,zero=1;
 for(unsigned i=0;i<sizeof bad/sizeof *bad;i++){opens=launches=0;arc_client_request(client,ARC_OPEN,bad[i],L"",0);arc_result *result=NULL;uint64_t until=GetTickCount64()+3000;while(!(result=arc_client_take(client))&&GetTickCount64()<until)Sleep(2);refused&=result&&result->error==ERROR_NOT_SUPPORTED;zero&=!opens&&!launches;arc_result_free(result);diff_result diff={0};DWORD error=diff_files(L"C:\\synthetic-admitted-left.txt",bad[i],1,NULL,&diff);refused&=error==ERROR_NOT_SUPPORTED&&diff.error_side==1;zero&=!opens;diff_free(&diff);}
 CHECK("real Archive worker and Diff reader return explicit unsupported errors",refused);CHECK("unsupported pair/path performs zero provider opens and zero decoder launches",zero);
 opens=launches=0;arc_client_request(client,ARC_OPEN,L"\\\\wsl.localhost\\Ubuntu\\ordinary.zip",L"",0);arc_result *result=NULL;uint64_t until=GetTickCount64()+3000;while(!(result=arc_client_take(client))&&GetTickCount64()<until)Sleep(2);CHECK("supported archive reaches the observed decoder launch boundary",result&&result->error!=ERROR_NOT_SUPPORTED&&launches>0);arc_result_free(result);arc_client_stop(&client);}
static void test_provider_windows_literal_name_remains_readable(void){wchar_t temp[32768],root[32768],file[32768];GetTempPathW(32768,temp);swprintf(root,32768,L"%lsopenpilot-provider-%lu-%llu",temp,(unsigned long)GetCurrentProcessId(),(unsigned long long)GetTickCount64());CHECK("owned literal-name fixture directory",CreateDirectoryW(root,NULL));swprintf(file,32768,L"%ls\\literal\uf080name.txt",root);HANDLE f=CreateFileW(file,GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);DWORD n=0;CHECK("literal Windows quote-codepoint file created",f!=INVALID_HANDLE_VALUE&&WriteFile(f,"literal\n",8,&n,NULL)&&n==8);if(f!=INVALID_HANDLE_VALUE)CloseHandle(f);opens=0;diff_result diff={0};CHECK("ordinary Diff reads the exact literal Windows filename",!diff_files(file,file,0,NULL,&diff)&&diff.bytes_equal&&opens==2);diff_free(&diff);DeleteFileW(file);RemoveDirectoryW(root);}
int wmain(void){setvbuf(stdout,NULL,_IONBF,0);test_provider_namespace_admission();test_provider_refusal_precedes_archive_and_diff_io();test_provider_windows_literal_name_remains_readable();printf("%d passed, %d failed; 3 provider-admission selectors\n",passed,failed);return failed?1:0;}
