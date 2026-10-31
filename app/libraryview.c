/* Folderwing owns navigation; the catalogue helper owns read-only DB queries. */
#include "libraryview.h"
#include "app.h"
#include "theme.h"
#include "docmarks.h"
#include <windowsx.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#include <wctype.h>
#define LIB_LIMIT 64
#define LIB_ROOT ((ULONG_PTR)0x46574C01)
#define LIB_SET ((ULONG_PTR)0x46574C04)
#define LIB_ACTION ((ULONG_PTR)0x46574C02)
#define LIB_READY (WM_APP+110)
#define LIB_READER (WM_APP+111)
#define LIB_FOCUS (WM_APP+114)
#define LIB_READER_DATA ((ULONG_PTR)0x46574C03)
typedef struct libroot {wchar_t name[128],*path;int kind,section;} libroot;
static struct {HWND parent,host,tree,child;HANDLE process,job;DWORD pid;wchar_t *config,*state;libroot roots[LIB_LIMIT];int count,index,focus,active,height,scroll,drag,drag_y,drag_height,pending,retries;uint64_t checked;uint32_t session,launched;library_callbacks callbacks;} L;
static int valid_local(const wchar_t *path){return path&&iswalpha(path[0])&&path[1]==L':'&&path[2]==L'\\'&&!wcspbrk(path,L"\"\r\n");}
const wchar_t *libraryview_config(void){return L.config;}
static void library_reap(void){if(L.job)CloseHandle(L.job);else if(L.process)TerminateProcess(L.process,0);if(L.process)CloseHandle(L.process);L.process=L.job=NULL;L.pid=0;L.child=NULL;}
int libraryview_checked(int index){return index>=0&&index<L.count&&!!(L.checked&(UINT64_C(1)<<index));}
int libraryview_checked_count(void){int n=0;for(int i=0;i<L.count;i++)n+=libraryview_checked(i);return n;}
static void send_root(void){
 if(!L.child||L.index<0||!L.checked)return;int count=libraryview_checked_count();size_t n=8;
 for(int i=0;i<L.count;i++)if(libraryview_checked(i)){libroot *r=L.roots+i;n+=(wcslen(r->path)+wcslen(r->name)+2)*2+(count>1?4:0);}
 uint8_t *bytes=calloc(1,n);if(!bytes)return;memcpy(bytes,&L.session,4);uint8_t *to=bytes+8;
 if(count==1){libroot *r=L.roots+L.index;memcpy(bytes+4,&r->kind,4);wcscpy((wchar_t*)to,r->path);to+=(wcslen(r->path)+1)*2;wcscpy((wchar_t*)to,r->name);}
 else {uint32_t total=count;memcpy(bytes+4,&total,4);for(int i=0;i<L.count;i++)if(libraryview_checked(i)){libroot *r=L.roots+i;memcpy(to,&r->kind,4);to+=4;wcscpy((wchar_t*)to,r->path);to+=(wcslen(r->path)+1)*2;wcscpy((wchar_t*)to,r->name);to+=(wcslen(r->name)+1)*2;}}
 COPYDATASTRUCT data={count>1?LIB_SET:LIB_ROOT,(DWORD)n,bytes};DWORD_PTR result=0;
 L.pending=!(SendMessageTimeoutW(L.child,WM_COPYDATA,(WPARAM)L.host,(LPARAM)&data,SMTO_ABORTIFHUNG|SMTO_ERRORONEXIT,250,&result)&&result==1);L.retries++;free(bytes);
}

static void spawn(void){wchar_t exe[32768];GetModuleFileNameW(NULL,exe,32768);wchar_t *slash=wcsrchr(exe,L'\\');if(!slash)return;wcscpy(slash+1,L"folderwing-library.exe");libroot *r=L.roots+L.index;size_t cap=wcslen(exe)+wcslen(r->path)+wcslen(r->name)+wcslen(L.state)+180;wchar_t *cmd=malloc(cap*2);if(!cmd)return;swprintf(cmd,cap,L"\"%ls\" %llu %u %ls \"%ls\" \"%ls\" \"%ls\"",exe,(unsigned long long)(uintptr_t)L.host,L.session,r->kind==1?L"calibre":L"zotero",r->path,r->name,L.state);wcscat(cmd,L" wait");STARTUPINFOW si={sizeof si};PROCESS_INFORMATION p={0};HANDLE job=CreateJobObjectW(NULL,NULL);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_PROCESS_MEMORY;limits.ProcessMemoryLimit=768ull*1024*1024;if(!job||!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof limits)){if(job)CloseHandle(job);free(cmd);return;}if(CreateProcessW(exe,cmd,NULL,NULL,FALSE,CREATE_SUSPENDED|CREATE_NO_WINDOW,NULL,NULL,&si,&p)){if(AssignProcessToJobObject(job,p.hProcess)&&ResumeThread(p.hThread)!=(DWORD)-1){L.launched=L.session;L.process=p.hProcess;L.job=job;L.pid=p.dwProcessId;SetTimer(L.host,1,500,NULL);}else{TerminateProcess(p.hProcess,0);CloseHandle(p.hProcess);CloseHandle(job);}CloseHandle(p.hThread);}else CloseHandle(job);free(cmd);InvalidateRect(L.host,NULL,FALSE);}
static void reveal_focus(void){RECT area;GetClientRect(L.tree,&area);int page=(area.bottom-app_px(34))/app_px(42);if(page<1)page=1;if(L.focus<L.scroll)L.scroll=L.focus;else if(L.focus>=L.scroll+page)L.scroll=L.focus-page+1;InvalidateRect(L.tree,NULL,FALSE);}
static void apply_selection(uint64_t mask){
 if(L.process&&WaitForSingleObject(L.process,0)==WAIT_OBJECT_0)library_reap();
 int same=L.checked==mask&&L.process;
 if(L.checked!=mask){L.checked=mask;for(int i=0;i<L.count;i++){wchar_t section[32];swprintf(section,32,L"Library%d",L.roots[i].section);WritePrivateProfileStringW(section,L"Checked",libraryview_checked(i)?L"1":L"0",L.config);}}
 if(!mask){if(!++L.session)++L.session;L.index=-1;L.pending=0;if(L.callbacks.action)L.callbacks.action(0,L"");reveal_focus();return;}
 L.index=0;while(!libraryview_checked(L.index))L.index++;
 if(same){int resume=!L.active;L.active=1;if(resume&&L.callbacks.selected)L.callbacks.selected();if(L.child){if(L.pending){L.retries=0;send_root();}PostMessageW(L.child,LIB_FOCUS,(WPARAM)L.host,0);}else SetFocus(L.host);reveal_focus();if(resume&&L.callbacks.changed)L.callbacks.changed();return;}
 L.active=1;if(!++L.session)++L.session;L.pending=1;L.retries=0;if(L.callbacks.selected)L.callbacks.selected();
 if(L.child)send_root();else if(!L.process)spawn();reveal_focus();if(L.callbacks.changed)L.callbacks.changed();
}
void libraryview_select(int index){if(index<0||index>=L.count)return;L.focus=index;apply_selection(UINT64_C(1)<<index);}
void libraryview_toggle(int index){if(index<0||index>=L.count)return;L.focus=index;apply_selection(L.checked^(UINT64_C(1)<<index));}

void libraryview_reader(int opened,const wchar_t *path){
 if(!L.host)return;if(!path)path=L"";size_t chars=wcslen(path)+1;if(chars>32768)return;
 uint8_t *data=calloc(1,8+chars*2);if(!data)return;memcpy(data,&L.session,4);uint32_t state=!!opened;memcpy(data+4,&state,4);memcpy(data+8,path,chars*2);
 if(!PostMessageW(L.host,LIB_READER,(WPARAM)(8+chars*2),(LPARAM)data))free(data);
}
void libraryview_activate(int active){L.active=active;ShowWindow(L.host,active?SW_SHOWNA:SW_HIDE);InvalidateRect(L.tree,NULL,FALSE);}
int libraryview_count(void){return L.count;}
int libraryview_height(void){return app_px(L.height);}
double libraryview_reader_ratio(void){if(L.index<0)return .6;wchar_t section[32];int combined=libraryview_checked_count()>1;if(combined)wcscpy(section,L"Libraries");else swprintf(section,32,L"Library%d",L.roots[L.index].section);int value=GetPrivateProfileIntW(section,combined?L"CombinedReaderPercent":L"ReaderPercent",60,L.config);return value>=25&&value<=85?value/100.0:.6;}
void libraryview_save_ratio(double ratio){if(L.index<0||ratio<.25||ratio>.85)return;wchar_t section[32],text[16];int combined=libraryview_checked_count()>1;if(combined)wcscpy(section,L"Libraries");else swprintf(section,32,L"Library%d",L.roots[L.index].section);swprintf(text,16,L"%d",(int)(ratio*100+.5));WritePrivateProfileStringW(section,combined?L"CombinedReaderPercent":L"ReaderPercent",text,L.config);}

HWND libraryview_window(void){return L.host;}
HWND libraryview_tree(void){return L.tree;}
void libraryview_place(int x,int y,int width,int height){SetWindowPos(L.host,NULL,x,y,width,height,SWP_NOZORDER|SWP_NOACTIVATE);if(L.child)SetWindowPos(L.child,NULL,0,0,width,height,SWP_NOZORDER|SWP_NOACTIVATE|SWP_ASYNCWINDOWPOS);}
static LRESULT CALLBACK host_proc(HWND hwnd,UINT message,WPARAM w,LPARAM l){switch(message){case WM_ERASEBKGND:return 1;case WM_SETFOCUS:if(L.child)SetFocus(L.child);return 0;case WM_SIZE:if(L.child)SetWindowPos(L.child,NULL,0,0,LOWORD(l),HIWORD(l),SWP_NOZORDER|SWP_NOACTIVATE|SWP_ASYNCWINDOWPOS);return 0;case LIB_READY:{DWORD pid=0;GetWindowThreadProcessId((HWND)w,&pid);if(pid!=L.pid||!L.pid||GetParent((HWND)w)!=hwnd)return 0;L.child=(HWND)w;RECT rc;GetClientRect(hwnd,&rc);SetWindowPos(L.child,NULL,0,0,rc.right,rc.bottom,SWP_NOZORDER|SWP_NOACTIVATE|SWP_ASYNCWINDOWPOS);send_root();return 0;}case LIB_READER:{uint8_t *bytes=(void*)l;uint32_t seq,state;memcpy(&seq,bytes,4);memcpy(&state,bytes+4,4);if(L.child&&seq==L.session&&(!state||L.active)){COPYDATASTRUCT data={LIB_READER_DATA,(DWORD)w,bytes};DWORD_PTR result;SendMessageTimeoutW(L.child,WM_COPYDATA,(WPARAM)hwnd,(LPARAM)&data,SMTO_ABORTIFHUNG|SMTO_ERRORONEXIT,250,&result);}free(bytes);return 0;}
 case WM_TIMER:if(L.child&&L.pending&&L.retries<3)send_root();if(L.process&&WaitForSingleObject(L.process,0)==WAIT_OBJECT_0){library_reap();KillTimer(hwnd,1);InvalidateRect(hwnd,NULL,FALSE);}return 0;
 case WM_COPYDATA:{DWORD pid=0;GetWindowThreadProcessId((HWND)w,&pid);COPYDATASTRUCT *data=(void*)l;if(pid!=L.pid||!L.active||!L.child||(HWND)w!=L.child||!data||data->dwData!=LIB_ACTION||data->cbData<10||data->cbData>65544||data->cbData%2)return 0;uint32_t seq,action;memcpy(&seq,data->lpData,4);memcpy(&action,(uint8_t*)data->lpData+4,4);const wchar_t *path=(const wchar_t*)((uint8_t*)data->lpData+8);size_t chars=(data->cbData-8)/2;if(seq!=L.session||action>5||path[chars-1]||wcsnlen(path,chars)!=chars-1||((action==1||action==2||action==3||action==5)&&!valid_local(path)))return 0;wchar_t *copy=_wcsdup(path);if(copy&&L.callbacks.action)L.callbacks.action(action,copy);free(copy);return 1;}
 case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);RECT r;GetClientRect(hwnd,&r);HBRUSH b=CreateSolidBrush(C_BG);FillRect(dc,&r,b);DeleteObject(b);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,C_TEXT);SelectObject(dc,app_font());DrawTextW(dc,L.process?L"Opening library…":L"Library helper unavailable. Select the library again to retry.",-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);EndPaint(hwnd,&ps);return 0;}}
 return DefWindowProcW(hwnd,message,w,l);}
static LRESULT CALLBACK tree_proc(HWND hwnd,UINT message,WPARAM w,LPARAM l){switch(message){case WM_ERASEBKGND:return 1;case WM_SETCURSOR:{POINT p;GetCursorPos(&p);ScreenToClient(hwnd,&p);if(L.drag||p.y<app_px(6)){SetCursor(LoadCursorW(NULL,IDC_SIZENS));return 1;}break;}case WM_LBUTTONDOWN:{int y=GET_Y_LPARAM(l);if(y<app_px(6)){L.drag=1;L.drag_y=GET_Y_LPARAM(GetMessagePos());L.drag_height=L.height;SetCapture(hwnd);return 0;}int row=(y-app_px(34))/app_px(42)+L.scroll;if(y>=app_px(34)&&row>=0&&row<L.count){SetFocus(hwnd);if(GET_X_LPARAM(l)<app_px(34))libraryview_toggle(row);else libraryview_select(row);}return 0;}
 case WM_MOUSEMOVE:if(L.drag){POINT p;GetCursorPos(&p);L.height=L.drag_height+MulDiv(L.drag_y-p.y,96,GetDpiForWindow(hwnd));if(L.height<84)L.height=84;if(L.height>600)L.height=600;if(L.callbacks.changed)L.callbacks.changed();}return 0;
 case WM_LBUTTONUP:if(L.drag){L.drag=0;ReleaseCapture();wchar_t text[20];swprintf(text,20,L"%d",L.height);WritePrivateProfileStringW(L"Libraries",L"Height",text,L.config);}return 0;case WM_CAPTURECHANGED:L.drag=0;return 0;
 case WM_MOUSEWHEEL:L.scroll-=GET_WHEEL_DELTA_WPARAM(w)/WHEEL_DELTA;if(L.scroll<0)L.scroll=0;RECT rc;GetClientRect(hwnd,&rc);int page=(rc.bottom-app_px(34))/app_px(42);if(page<1)page=1;if(L.scroll>L.count-page)L.scroll=L.count>page?L.count-page:0;InvalidateRect(hwnd,NULL,FALSE);return 0;
 case WM_SETFOCUS:case WM_KILLFOCUS:InvalidateRect(hwnd,NULL,FALSE);return 0;
 case WM_KEYDOWN:if(w==VK_UP||w==VK_DOWN){int next=L.focus+(w==VK_UP?-1:1);if(next>=0&&next<L.count){L.focus=next;reveal_focus();}return 0;}if(w==VK_SPACE){libraryview_toggle(L.focus);return 0;}if(w==VK_RETURN){libraryview_select(L.focus);return 0;}break;
 case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);RECT rc;GetClientRect(hwnd,&rc);HBRUSH bg=CreateSolidBrush(C_TREE_BG);FillRect(dc,&rc,bg);DeleteObject(bg);SetBkMode(dc,TRANSPARENT);SelectObject(dc,app_font());SetTextColor(dc,C_DIM);RECT header={app_px(12),app_px(7),rc.right-app_px(10),app_px(32)};DrawTextW(dc,L"LIBRARIES",-1,&header,DT_SINGLELINE|DT_VCENTER);for(int i=L.scroll;i<L.count;i++){int y=app_px(34)+(i-L.scroll)*app_px(42);if(y>=rc.bottom)break;RECT row={0,y,rc.right,y+app_px(42)};if(L.active&&libraryview_checked(i)){HBRUSH sel=CreateSolidBrush(C_SEL);FillRect(dc,&row,sel);DeleteObject(sel);}RECT check={app_px(12),y+app_px(13),app_px(27),y+app_px(28)};DrawFrameControl(dc,&check,DFC_BUTTON,DFCS_BUTTONCHECK|(libraryview_checked(i)?DFCS_CHECKED:0));if(GetFocus()==hwnd&&L.focus==i)DrawFocusRect(dc,&row);RECT name={app_px(36),y+app_px(2),rc.right-app_px(8),y+app_px(22)};SetTextColor(dc,C_TEXT);DrawTextW(dc,L.roots[i].name,-1,&name,DT_SINGLELINE|DT_END_ELLIPSIS);name.top=y+app_px(22);name.bottom=y+app_px(40);SetTextColor(dc,C_DIM);DrawTextW(dc,L.roots[i].kind==1?L"Calibre":L"Zotero",-1,&name,DT_SINGLELINE);}EndPaint(hwnd,&ps);return 0;}}
 return DefWindowProcW(hwnd,message,w,l);}
int libraryview_start(HWND parent,const wchar_t *reference,const library_callbacks *callbacks){if(L.host)return 0;L.parent=parent;L.index=-1;L.height=220;L.callbacks=*callbacks;wchar_t override[32768];DWORD env=GetEnvironmentVariableW(L"FOLDERWING_LIBRARY_CONFIG",override,32768);L.config=env&&env<32768?_wcsdup(override):docmarks_location(reference,L"libraries.ini");L.state=docmarks_location(reference,L"library-views.json");if(!L.config||!L.state)return -1;L.height=GetPrivateProfileIntW(L"Libraries",L"Height",220,L.config);if(L.height<84||L.height>600)L.height=220;int count=GetPrivateProfileIntW(L"Libraries",L"Count",0,L.config);if(count>LIB_LIMIT)count=LIB_LIMIT;for(int i=0;i<count;i++){wchar_t section[32],path[32768],name[128],kind[20];swprintf(section,32,L"Library%d",i+1);GetPrivateProfileStringW(section,L"Path",L"",path,32768,L.config);GetPrivateProfileStringW(section,L"Name",L"Library",name,128,L.config);GetPrivateProfileStringW(section,L"Type",L"",kind,20,L.config);if(!valid_local(path)||wcspbrk(name,L"\"\r\n")||(_wcsicmp(kind,L"calibre")&&_wcsicmp(kind,L"zotero")))continue;libroot *r=L.roots+L.count;r->path=_wcsdup(path);if(!r->path)continue;size_t length=wcslen(r->path);while(length>3&&r->path[length-1]==L'\\')r->path[--length]=0;wcscpy(r->name,name);r->section=i+1;r->kind=!_wcsicmp(kind,L"calibre")?1:2;if(GetPrivateProfileIntW(section,L"Checked",0,L.config))L.checked|=UINT64_C(1)<<L.count;L.count++;}
 WNDCLASSW cls={0};cls.hInstance=GetModuleHandleW(NULL);cls.hCursor=LoadCursorW(NULL,IDC_ARROW);cls.lpszClassName=L"FolderwingLibraryHost";cls.lpfnWndProc=host_proc;RegisterClassW(&cls);L.host=CreateWindowExW(0,cls.lpszClassName,L"Library",WS_CHILD|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,0,0,1,1,parent,NULL,cls.hInstance,NULL);cls.lpszClassName=L"FolderwingLibraries";cls.lpfnWndProc=tree_proc;RegisterClassW(&cls);L.tree=CreateWindowExW(0,cls.lpszClassName,L"Libraries",WS_CHILD|WS_CLIPSIBLINGS,0,0,1,1,parent,NULL,cls.hInstance,NULL);return L.host&&L.tree?0:-1;}
void libraryview_stop(void){MSG pending;while(PeekMessageW(&pending,L.host,LIB_READER,LIB_READER,PM_REMOVE))free((void*)pending.lParam);if(L.child){DWORD_PTR result;SendMessageTimeoutW(L.child,WM_CLOSE,0,0,SMTO_ABORTIFHUNG|SMTO_ERRORONEXIT,500,&result);}library_reap();if(L.host)DestroyWindow(L.host);if(L.tree)DestroyWindow(L.tree);for(int i=0;i<L.count;i++)free(L.roots[i].path);free(L.config);free(L.state);memset(&L,0,sizeof L);}
