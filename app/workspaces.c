/* Cold modeless workspace manager. The UI owns snapshots; a single worker
 * owns all catalog I/O. Completion adopts a whole new catalog or retains the
 * old one with a visible error. No automatic updates of named workspaces. */
#include "workspaces.h"
#include "workspace.h"
#include "app.h"
#include "theme.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <uxtheme.h>
enum { ID_WS_LIST=101,ID_WS_NAME,ID_WS_SAVE,ID_WS_SWITCH,ID_WS_UPDATE,ID_WS_RENAME,ID_WS_DELETE,ID_WS_RELOAD,ID_WS_STATUS,ID_WS_DETAIL };
#define WM_WS_DONE (WM_APP+1)
#define WM_WS_SHOW (WM_APP+2)
typedef struct workspace_window {
 HWND hwnd;HANDLE thread,cancel;CRITICAL_SECTION lock;LONG refs;const fn_def *owner;HBRUSH bg,edit;
 wchar_t *file;workspace_catalog catalog,result;session draft;
 wchar_t name[WS_NAME],new_name[WS_NAME],selected[WS_NAME],active[WS_NAME];
 uint32_t action;uint64_t expected;DWORD error;int loaded,busy,deleting;
} workspace_window;
static HWND window;
static void release(workspace_window *s){if(InterlockedDecrement(&s->refs))return;workspace_free(&s->catalog);workspace_free(&s->result);session_free(&s->draft);if(s->cancel)CloseHandle(s->cancel);free(s->file);DeleteObject(s->bg);DeleteObject(s->edit);DeleteCriticalSection(&s->lock);free(s);}
static void status(workspace_window *s,const wchar_t *message){SetDlgItemTextW(s->hwnd,ID_WS_STATUS,message);}
static int selection(workspace_window *s){LRESULT at=SendDlgItemMessageW(s->hwnd,ID_WS_LIST,LB_GETCURSEL,0,0);return at>=0&&(uint32_t)at<s->catalog.count?(int)at:-1;}
static void enable(workspace_window *s){int selected=selection(s)>=0,ready=s->loaded&&!s->busy;int ids[]={ID_WS_SAVE,ID_WS_SWITCH,ID_WS_UPDATE,ID_WS_RENAME,ID_WS_DELETE};for(int i=0;i<5;i++)EnableWindow(GetDlgItem(s->hwnd,ids[i]),ready&&(i==0||selected));EnableWindow(GetDlgItem(s->hwnd,ID_WS_LIST),!s->busy);EnableWindow(GetDlgItem(s->hwnd,ID_WS_NAME),!s->busy);EnableWindow(GetDlgItem(s->hwnd,ID_WS_RELOAD),!s->busy);}
static void detail(workspace_window *s){int at=selection(s);s->deleting=0;SetDlgItemTextW(s->hwnd,ID_WS_DELETE,L"Delete…");if(at<0){s->selected[0]=0;SetDlgItemTextW(s->hwnd,ID_WS_DETAIL,L"Save as creates a named snapshot. Update selected explicitly replaces it with your current tabs and layout. Switch replaces open tabs; it does not save your temporary navigation.");enable(s);return;}
 workspace *w=&s->catalog.items[at];wcscpy(s->selected,w->name);SetDlgItemTextW(s->hwnd,ID_WS_NAME,w->name);wchar_t text[2200];session *v=&w->snapshot;swprintf(text,2200,L"%ls%ls\r\n%d left tabs · %d right tabs · %ls · %ls tree · split %.0f%%\r\nLeft: %.800ls\r\nRight: %.800ls\r\nFolders are checked when opened; inaccessible tabs retain their address and show an error.",w->name,!_wcsicmp(s->active,w->name)?L" (last switched here)":L"",v->ntabs[0],v->ntabs[1],v->dual?L"two panes":L"one pane",v->tree?L"visible":L"hidden",v->ratio*100,v->tabs[0][v->cur[0]].path,v->tabs[1][v->cur[1]].path);SetDlgItemTextW(s->hwnd,ID_WS_DETAIL,text);enable(s);}
static void populate(workspace_window *s,const wchar_t *wanted){SendDlgItemMessageW(s->hwnd,ID_WS_LIST,LB_RESETCONTENT,0,0);for(uint32_t i=0;i<s->catalog.count;i++)SendDlgItemMessageW(s->hwnd,ID_WS_LIST,LB_ADDSTRING,0,(LPARAM)s->catalog.items[i].name);int at=wanted&&*wanted?workspace_find(&s->catalog,wanted):-1;if(at<0&&s->catalog.count)at=0;if(at>=0)SendDlgItemMessageW(s->hwnd,ID_WS_LIST,LB_SETCURSEL,at,0);detail(s);}
static DWORD WINAPI worker(void *arg){workspace_window *s=arg;s->error=s->action==WS_LOAD?workspace_load(s->file,&s->result,s->cancel):workspace_change(s->file,s->expected,s->action,s->name,s->new_name,&s->draft,&s->result,s->cancel);EnterCriticalSection(&s->lock);if(s->hwnd)PostMessageW(s->hwnd,WM_WS_DONE,0,0);LeaveCriticalSection(&s->lock);release(s);return 0;}
static void begin(workspace_window *s,uint32_t action){if(s->busy)return;s->deleting=0;SetDlgItemTextW(s->hwnd,ID_WS_DELETE,L"Delete…");s->action=action;s->expected=s->catalog.revision;workspace_free(&s->result);ResetEvent(s->cancel);s->busy=1;s->error=0;InterlockedIncrement(&s->refs);s->thread=CreateThread(NULL,0,worker,s,0,NULL);if(!s->thread){s->busy=0;s->error=GetLastError();release(s);status(s,L"Could not start workspace I/O");}else status(s,action==WS_LOAD?L"Loading workspaces…":L"Saving workspace catalog…");enable(s);}
static void completed(workspace_window *s){if(!s->busy)return;/* Worker posts after publishing; no UI work waits for slow file I/O. */
 if(s->thread){CloseHandle(s->thread);s->thread=NULL;}s->busy=0;session_free(&s->draft);
 if(s->error){wchar_t text[350];if(s->error==ERROR_REVISION_MISMATCH)wcscpy(text,L"Another window changed the workspace catalog. Reload before saving again; your saved data was retained.");else if(s->error==ERROR_ALREADY_EXISTS)wcscpy(text,L"That workspace name already exists. Choose a new name, or select it and use Update selected.");else if(s->error==ERROR_CANCELLED)wcscpy(text,L"Workspace operation cancelled. Reload to see the current catalog.");else swprintf(text,350,L"Workspace operation failed (Windows error %lu). Existing saved data was retained; Reload to retry.",(unsigned long)s->error);if(s->action==WS_LOAD||s->error==ERROR_REVISION_MISMATCH)s->loaded=0;status(s,text);enable(s);return;}
 wchar_t wanted[WS_NAME];wcscpy(wanted,s->action==WS_RENAME?s->new_name:s->action==WS_LOAD?s->selected:s->name);if(s->action==WS_RENAME&&!_wcsicmp(s->active,s->name))wcscpy(s->active,s->new_name);if(s->action==WS_DELETE&&!_wcsicmp(s->active,s->name))s->active[0]=0;
 workspace_free(&s->catalog);s->catalog=s->result;memset(&s->result,0,sizeof s->result);s->loaded=1;populate(s,wanted);status(s,s->action==WS_LOAD?L"Ready. Named workspaces change only through Save as or Update selected.":s->action==WS_DELETE?L"Workspace deleted. Current tabs were kept.":s->action==WS_RENAME?L"Workspace renamed.":L"Workspace saved. Further navigation will not update it automatically.");}
static void command(workspace_window *s,int id){if(s->busy)return;if(id==ID_WS_RELOAD){begin(s,WS_LOAD);return;}int at=selection(s);if(!s->loaded)return;
 if(id==ID_WS_SWITCH){if(at<0)return;DWORD e=app_workspace_apply(&s->catalog.items[at].snapshot);if(e){status(s,L"Could not restore workspace; current tabs were retained.");return;}wcscpy(s->active,s->catalog.items[at].name);detail(s);status(s,L"Workspace switched. Folders load in the background; unavailable folders show an error. Saved data was not updated.");return;}
 wchar_t field[WS_NAME];GetDlgItemTextW(s->hwnd,ID_WS_NAME,field,WS_NAME);if(id==ID_WS_SAVE||id==ID_WS_RENAME){if(!workspace_name(field,s->new_name)){status(s,L"Use a name of 1–80 characters, without control characters or spaces at either end.");return;}}
 if(id!=ID_WS_SAVE&&at<0)return;
 wcscpy(s->name,id==ID_WS_SAVE?s->new_name:s->catalog.items[at].name);
 if(id==ID_WS_DELETE&&!s->deleting){s->deleting=1;wchar_t text[240];swprintf(text,240,L"Delete “%ls”? Click Delete again to confirm. Current tabs will be kept.",s->name);status(s,text);SetDlgItemTextW(s->hwnd,ID_WS_DELETE,L"Delete again");return;}
 if(id==ID_WS_SAVE||id==ID_WS_UPDATE){session_free(&s->draft);DWORD e=app_workspace_capture(&s->draft);if(e){status(s,e==ERROR_BUSY?L"Wait for folder navigation to finish, then save again.":L"Workspaces require a real folder in every tab (up to 32 per pane).");return;}}
 begin(s,id==ID_WS_SAVE?WS_CREATE:id==ID_WS_UPDATE?WS_UPDATE:id==ID_WS_RENAME?WS_RENAME:WS_DELETE);
}
static void layout(workspace_window *s){RECT r;GetClientRect(s->hwnd,&r);int dpi=(int)GetDpiForWindow(s->hwnd),pad=MulDiv(14,dpi,96),gap=MulDiv(8,dpi,96),line=MulDiv(30,dpi,96),lw=MulDiv(220,dpi,96),right=lw+pad*2,width=r.right-right-pad;
 MoveWindow(GetDlgItem(s->hwnd,ID_WS_LIST),pad,pad,lw,r.bottom-pad*2,TRUE);MoveWindow(GetDlgItem(s->hwnd,ID_WS_NAME),right,pad,width,line,TRUE);
 int ids[]={ID_WS_SAVE,ID_WS_SWITCH,ID_WS_UPDATE,ID_WS_RENAME,ID_WS_DELETE,ID_WS_RELOAD};for(int i=0;i<6;i++){int col=i%2,row=i/2,w=(width-gap)/2;MoveWindow(GetDlgItem(s->hwnd,ids[i]),right+col*(w+gap),pad+line+gap+row*(line+gap),w,line,TRUE);}
 int y=pad+4*(line+gap);MoveWindow(GetDlgItem(s->hwnd,ID_WS_STATUS),right,y,width,MulDiv(75,dpi,96),TRUE);y+=MulDiv(85,dpi,96);MoveWindow(GetDlgItem(s->hwnd,ID_WS_DETAIL),right,y,width,r.bottom-y-pad>1?r.bottom-y-pad:1,TRUE);}
static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l){workspace_window *s=(void*)GetWindowLongPtrW(h,GWLP_USERDATA);if(m==WM_NCCREATE){s=((CREATESTRUCTW*)l)->lpCreateParams;s->hwnd=h;SetWindowLongPtrW(h,GWLP_USERDATA,(LONG_PTR)s);}if(!s)return DefWindowProcW(h,m,w,l);
 switch(m){case WM_CREATE:{struct control{const wchar_t *cls,*text;DWORD style;int id;} controls[]={
  {L"LISTBOX",L"",WS_TABSTOP|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,ID_WS_LIST},{L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,ID_WS_NAME},
  {L"BUTTON",L"Save as new",WS_TABSTOP,ID_WS_SAVE},{L"BUTTON",L"Switch",WS_TABSTOP,ID_WS_SWITCH},{L"BUTTON",L"Update selected",WS_TABSTOP,ID_WS_UPDATE},{L"BUTTON",L"Rename selected",WS_TABSTOP,ID_WS_RENAME},{L"BUTTON",L"Delete…",WS_TABSTOP,ID_WS_DELETE},{L"BUTTON",L"Reload list",WS_TABSTOP,ID_WS_RELOAD},
  {L"STATIC",L"",0,ID_WS_STATUS},{L"STATIC",L"",0,ID_WS_DETAIL}};
  for(size_t i=0;i<sizeof controls/sizeof *controls;i++){struct control *c=&controls[i];HWND child=CreateWindowExW(!wcscmp(c->cls,L"EDIT")||!wcscmp(c->cls,L"LISTBOX")?WS_EX_CLIENTEDGE:0,c->cls,c->text,WS_CHILD|WS_VISIBLE|c->style,0,0,1,1,h,(HMENU)(INT_PTR)c->id,GetModuleHandleW(NULL),NULL);SendMessageW(child,WM_SETFONT,(WPARAM)app_font(),TRUE);SetWindowTheme(child,L"DarkMode_Explorer",NULL);}
  SendDlgItemMessageW(h,ID_WS_NAME,EM_SETLIMITTEXT,WS_NAME-1,0);layout(s);detail(s);begin(s,WS_LOAD);return 0;}
 case WM_SIZE:layout(s);return 0;
 case WM_GETMINMAXINFO:{MINMAXINFO *mm=(void*)l;mm->ptMinTrackSize.x=app_px(720);mm->ptMinTrackSize.y=app_px(460);return 0;}
 case WM_WS_SHOW:ShowWindow(h,SW_SHOWNOACTIVATE);app_focus_window(GetDlgItem(h,ID_WS_LIST));return 0;
 case WM_WS_DONE:completed(s);return 0;
 case WM_COMMAND:if(LOWORD(w)==IDCANCEL){DestroyWindow(h);return 0;}if(LOWORD(w)==ID_WS_LIST&&HIWORD(w)==LBN_SELCHANGE){detail(s);return 0;}if(LOWORD(w)==ID_WS_LIST&&HIWORD(w)==LBN_DBLCLK){command(s,ID_WS_SWITCH);return 0;}if(LOWORD(w)>=ID_WS_SAVE&&LOWORD(w)<=ID_WS_RELOAD&&HIWORD(w)==BN_CLICKED){command(s,LOWORD(w));return 0;}if(LOWORD(w)==ID_WS_NAME&&HIWORD(w)==EN_CHANGE){s->deleting=0;SetDlgItemTextW(h,ID_WS_DELETE,L"Delete…");}break;
 case WM_CLOSE:DestroyWindow(h);return 0;
 case WM_DESTROY:SetEvent(s->cancel);if(s->thread){CancelSynchronousIo(s->thread);CloseHandle(s->thread);s->thread=NULL;}EnterCriticalSection(&s->lock);s->hwnd=NULL;LeaveCriticalSection(&s->lock);if(window==h)window=NULL;SetWindowLongPtrW(h,GWLP_USERDATA,0);fn_release(s->owner);release(s);return 0;
 case WM_CTLCOLORSTATIC:case WM_CTLCOLORBTN:case WM_CTLCOLOREDIT:case WM_CTLCOLORLISTBOX:SetTextColor((HDC)w,C_TEXT);SetBkColor((HDC)w,m==WM_CTLCOLOREDIT||m==WM_CTLCOLORLISTBOX?C_EDIT:C_BG);return (LRESULT)(m==WM_CTLCOLOREDIT||m==WM_CTLCOLORLISTBOX?s->edit:s->bg);
 case WM_ERASEBKGND:{RECT r;GetClientRect(h,&r);FillRect((HDC)w,&r,s->bg);return 1;}}
 return DefWindowProcW(h,m,w,l);
}
void workspaces_run(fn_ctx *context){(void)context;if(window){ShowWindow(window,SW_SHOWNOACTIVATE);app_focus_window(GetDlgItem(window,ID_WS_LIST));return;}const wchar_t *base=app_workspace_session();if(!base||!*base){app_status(NULL,1,L"Saved workspaces are disabled by --session=");return;}
 wchar_t absolute[32768];DWORD n=GetFullPathNameW(base,32768,absolute,NULL);if(!n||n>=32750){app_status(NULL,1,L"Workspace catalog path is too long");return;}workspace_window *s=calloc(1,sizeof *s);if(!s)return;s->refs=1;s->owner=fn_self();InitializeCriticalSection(&s->lock);s->cancel=CreateEventW(NULL,TRUE,FALSE,NULL);s->bg=CreateSolidBrush(C_BG);s->edit=CreateSolidBrush(C_EDIT);s->file=malloc(((size_t)n+16)*2);if(s->file)swprintf(s->file,(size_t)n+16,L"%ls.workspaces",absolute);if(!s->file||!s->cancel||fn_hold(s->owner)){release(s);return;}
 WNDCLASSW cls={.lpfnWndProc=proc,.hInstance=GetModuleHandleW(NULL),.hCursor=LoadCursorW(NULL,IDC_ARROW),.lpszClassName=L"OpenPilotWorkspaces"};RegisterClassW(&cls);RECT r={0,0,app_px(850),app_px(500)},parent;AdjustWindowRectEx(&r,WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,0,WS_EX_CONTROLPARENT);GetWindowRect(app_window(),&parent);HWND h=CreateWindowExW(WS_EX_CONTROLPARENT,cls.lpszClassName,L"Saved workspaces",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,parent.left+32,parent.top+32,r.right-r.left,r.bottom-r.top,app_window(),NULL,cls.hInstance,s);if(!h){fn_release(s->owner);release(s);return;}window=h;PostMessageW(h,WM_WS_SHOW,0,0);
}
int workspaces_message(MSG *m){return window&&(m->hwnd==window||IsChild(window,m->hwnd))&&IsDialogMessageW(window,m);}
void workspaces_shutdown(void){if(window)DestroyWindow(window);}
void workspaces_configure(void){if(!fn_active(fn_find("workspace.manage")))workspaces_shutdown();}
