/* Actual restricted player client; owned hidden windows and synthetic fixtures.
 * Host stubs supply only fonts/context plumbing, never player behavior. */
#include "../../../app/mediaview.h"
#include "../../../app/mediautil.h"
#include "../../../app/app.h"
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <math.h>
static int passed,failed;static HWND owner;static wchar_t fixtures[32768];
#define CHECK(label,c) do{if(c){passed++;printf("ok %s\n",label);}else{failed++;printf("FAIL line %d: %s\n",__LINE__,label);}}while(0)
HFONT app_font(void){return GetStockObject(DEFAULT_GUI_FONT);}int app_px(int n){return n;}
void app_preview_close(void){mediaview_clear();}void app_focus_cycle(int n){(void)n;}
void app_preview_ctx(fn_ctx *c,HWND h,int from){memset(c,0,sizeof *c);c->from=from;}
void app_ctx_free(fn_ctx *c){(void)c;}
int fn_key(uint8_t key,uint8_t mods,fn_ctx *c){return 0;}
static void pump_media(int ms){uint64_t until=GetTickCount64()+ms;do{MSG m;while(PeekMessageW(&m,NULL,0,0,PM_REMOVE))if(m.message!=WM_QUIT){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(2);}while(GetTickCount64()<until);}
static mediap_state state(void){mediap_state s;mediaview_state(&s);return s;}
static int wait_ready(void){uint64_t until=GetTickCount64()+22000;while(state().state==MEDIAP_LOADING&&GetTickCount64()<until)pump_media(10);mediap_state s=state();printf("state=%u backend=%u error=0x%08x fallback=0x%08x audio=%u video=%u paused=%u muted=%u pid=%lu\n",s.state,s.backend,s.error,s.fallback_error,s.has_audio,s.has_video,s.paused,s.muted,(unsigned long)mediaview_helper_pid());return s.state==MEDIAP_READY;}
static void open_fixture(const wchar_t *name){wchar_t path[32768];swprintf(path,32768,L"%ls\\%ls",fixtures,name);mediaview_open(path,1,1);}
static int has_mpv(void){DWORD pid=mediaview_helper_pid();HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);if(snapshot==INVALID_HANDLE_VALUE)return -1;MODULEENTRY32W m={.dwSize=sizeof m};int yes=0;if(Module32FirstW(snapshot,&m))do{if(!_wcsicmp(m.szModule,L"libmpv-2.dll"))yes=1;}while(Module32NextW(snapshot,&m));CloseHandle(snapshot);return yes;}
static int low_token(void){HANDLE p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,mediaview_helper_pid()),token=NULL;DWORD n=0;int low=0;if(p&&OpenProcessToken(p,TOKEN_QUERY,&token)){GetTokenInformation(token,TokenIntegrityLevel,NULL,0,&n);TOKEN_MANDATORY_LABEL *label=malloc(n);if(label&&GetTokenInformation(token,TokenIntegrityLevel,label,n,&n)){DWORD count=*GetSidSubAuthorityCount(label->Label.Sid);low=*GetSidSubAuthority(label->Label.Sid,count-1)==SECURITY_MANDATORY_LOW_RID;}free(label);CloseHandle(token);}if(p)CloseHandle(p);return low;}
static int alive(DWORD pid){HANDLE p=OpenProcess(SYNCHRONIZE,FALSE,pid);if(!p)return 0;int active=WaitForSingleObject(p,0)==WAIT_TIMEOUT;CloseHandle(p);return active;}
