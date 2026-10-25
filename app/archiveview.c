/* Archive listing is a read-only navigation surface, independent of normal pane commands. */
#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "archiveview.h"
#include "app.h"
#include "theme.h"
#include "providerpath.h"
#define ARC_RESULT (WM_APP+180)
static struct {
 HWND hwnd;arc_client *client;LONG generation;uint64_t revision,size;int64_t mtime;
 wchar_t *path,*folder,*destination;arc_row *rows;uint32_t count,backend;int cursor,top,w,h,busy;
 wchar_t status[200];uint64_t due;arc_file *preview;
} A;
static int header(void){return app_px(34);}
static int row_h(void){return app_row_h();}
static int page(void){int n=(A.h-header()-app_px(26))/row_h();return n>0?n:1;}
static arc_row *selected(void){return A.cursor>=0&&(uint32_t)A.cursor<A.count?&A.rows[A.cursor]:NULL;}
static void set_status(const wchar_t *s){wcsncpy(A.status,s,199);A.status[199]=0;InvalidateRect(A.hwnd,NULL,FALSE);}
static const wchar_t *error_text(uint32_t e){switch(e){case ERROR_FILE_TOO_LARGE:return L"Entry exceeds the size limit";case ERROR_FILE_INVALID:return L"Archive changed — select it again";case ERROR_PASSWORD_RESTRICTION:return L"Encrypted entry — password entry is not available yet";case ERROR_NOT_SUPPORTED:return L"This entry type or compression method is unsupported";case ERROR_BUFFER_OVERFLOW:return L"Archive listing exceeds the entry/name limit";case ERROR_TIMEOUT:return L"Archive helper stopped responding";case ERROR_ACCESS_DENIED:return L"Archive could not be opened or its helper could not start";case ERROR_BAD_FORMAT:return L"Unsupported, damaged or encrypted archive";case ERROR_CRC:return L"Archive entry failed its integrity check";default:return L"Archive could not be read";}}
static void scroll(void){if(!A.hwnd)return;int max=(int)A.count-page();if(max<0)max=0;if(A.top>max)A.top=max;if(A.top<0)A.top=0;SCROLLINFO s={sizeof s,SIF_RANGE|SIF_PAGE|SIF_POS,0,A.count?(int)A.count-1:0,(UINT)page(),A.top,0};SetScrollInfo(A.hwnd,SB_VERT,&s,TRUE);InvalidateRect(A.hwnd,NULL,FALSE);}
static void clear_content(void){A.due=0;fn_timer(fn_find("preview.archive"),0);app_archive_preview(NULL,NULL,0);arc_file_release(A.preview);A.preview=NULL;}
static void select_row(int at){
 if(at<0||at>=(int)A.count)return;if(A.cursor==at)return;A.cursor=at;A.revision++;A.generation=arc_client_cancel(A.client);A.busy=0;clear_content();free(A.destination);A.destination=NULL;
 arc_row *row=selected();if(row->flags&ARCP_DIR)set_status(L"Folder — Enter or double-click to open");
 else if(row->flags&(ARCP_SPECIAL|ARCP_UNSAFE|ARCP_UNSUPPORTED))set_status(L"Link, special or unsupported entry — excluded");
 else if(row->flags&ARCP_ENCRYPTED)set_status(error_text(ERROR_PASSWORD_RESTRICTION));
 else if(app_archive_preview_kind(row->name)){A.due=GetTickCount64()+100;fn_timer(fn_find("preview.archive"),100);set_status(L"Preparing preview…");}
 else {swprintf(A.status,200,L"No preview for this type · %llu bytes · Extract file…",(unsigned long long)row->size);InvalidateRect(A.hwnd,NULL,FALSE);}
 if(A.cursor<A.top)A.top=A.cursor;if(A.cursor>=A.top+page())A.top=A.cursor-page()+1;scroll();
}
static void request_folder(const wchar_t *folder){wchar_t *copy=_wcsdup(folder);if(!copy)return;A.revision++;clear_content();free(A.folder);A.folder=copy;arc_rows_free(A.rows,A.count);A.rows=NULL;A.count=0;A.cursor=-1;A.top=0;A.busy=1;A.generation=arc_client_request(A.client,ARC_FOLDER,A.path,A.folder,0);set_status(L"Opening folder…");scroll();}
static int run_id(const char *id,int from){fn_ctx c;app_preview_ctx(&c,A.hwnd,from);int ran=fn_run_id(id,&c);app_ctx_free(&c);return ran;}
int archiveview_scope(const fn_ctx *c){return A.hwnd&&A.path&&IsWindowVisible(A.hwnd)&&selected()&&(!c||c->from!=FN_FROM_KEY||GetFocus()==A.hwnd);}
int archiveview_can_open(const fn_ctx *c){arc_row *r=selected();return archiveview_scope(c)&&!A.busy&&!(r->flags&(ARCP_SPECIAL|ARCP_UNSAFE|ARCP_UNSUPPORTED|ARCP_ENCRYPTED))&&((r->flags&ARCP_DIR)||app_archive_preview_kind(r->name));}
int archiveview_can_extract(const fn_ctx *c){arc_row *r=selected();return fn_active(fn_find("operations.show"))&&archiveview_scope(c)&&!A.busy&&arc_leaf_safe(r->name)&&!(r->flags&(ARCP_DIR|ARCP_SPECIAL|ARCP_UNSAFE|ARCP_UNSUPPORTED|ARCP_ENCRYPTED));}
int archiveview_can_up(const fn_ctx *c){return A.hwnd&&A.path&&A.folder&&*A.folder&&(!c||c->from!=FN_FROM_KEY||GetFocus()==A.hwnd);}
void archiveview_preview(fn_ctx *c){(void)c;arc_row *r=selected();if(!r)return;if(r->flags&ARCP_DIR){if(!(r->flags&ARCP_UNSAFE))request_folder(r->path);return;}
 A.due=0;fn_timer(fn_find("preview.archive"),0);clear_content();A.busy=1;A.generation=arc_client_request(A.client,ARC_PREVIEW,A.path,A.folder,r->index);if(!A.generation){A.busy=0;set_status(L"Could not prepare preview");}else set_status(L"Decoding selected entry…");}
void archiveview_tick(void){if(A.due&&GetTickCount64()>=A.due){A.due=0;fn_timer(fn_find("preview.archive"),0);if(run_id("archive.preview",FN_FROM_CODE)<=0)set_status(L"Preview is unavailable");}else if(!A.due)fn_timer(fn_find("preview.archive"),0);}
void archiveview_up(fn_ctx *c){(void)c;if(!A.folder||!*A.folder)return;wchar_t *up=_wcsdup(A.folder);if(!up)return;size_t n=wcslen(up);if(n&&up[n-1]==L'/')up[--n]=0;wchar_t *slash=wcsrchr(up,L'/');if(slash)slash[1]=0;else up[0]=0;request_folder(up);free(up);}
void archiveview_close(fn_ctx *c){(void)c;app_archive_close();}
static void copy_text(const wchar_t *s){uint64_t ticket=app_clipboard_begin(A.hwnd);size_t n=(wcslen(s)+1)*sizeof *s;HGLOBAL data=GlobalAlloc(GMEM_MOVEABLE,n);void *to=data?GlobalLock(data):NULL;if(to){memcpy(to,s,n);GlobalUnlock(data);}else {if(data)GlobalFree(data);data=NULL;}app_clipboard_submit(ticket,CF_UNICODETEXT,data,0,NULL,L"Archive entry copied");}
void archiveview_copy_name(fn_ctx *c){(void)c;arc_row *r=selected();if(r)copy_text(r->name);}
void archiveview_copy_path(fn_ctx *c){(void)c;arc_row *r=selected();if(r)copy_text(r->path);}
void archiveview_extract(fn_ctx *c){(void)c;if(!archiveview_can_extract(NULL))return;
 HRESULT com=CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);if(FAILED(com)&&com!=RPC_E_CHANGED_MODE){set_status(L"Folder picker could not open");return;}
 uint64_t revision=A.revision;IFileDialog *dialog=NULL;IShellItem *item=NULL;PWSTR path=NULL;
 HRESULT hr=CoCreateInstance(&CLSID_FileOpenDialog,NULL,CLSCTX_INPROC_SERVER,&IID_IFileDialog,(void**)&dialog);
 if(FAILED(hr)){if(SUCCEEDED(com))CoUninitialize();set_status(L"Folder picker could not open");return;}
 IFileDialog_SetOptions(dialog,FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR);
 IFileDialog_SetTitle(dialog,L"Extract file — choose destination folder");
 hr=IFileDialog_Show(dialog,A.hwnd);
 if(SUCCEEDED(hr)&&revision==A.revision&&A.path&&SUCCEEDED(IFileDialog_GetResult(dialog,&item))&&SUCCEEDED(IShellItem_GetDisplayName(item,SIGDN_FILESYSPATH,&path))){
  free(A.destination);A.destination=_wcsdup(path);arc_row *r=selected();if(A.destination&&r){A.due=0;fn_timer(fn_find("preview.archive"),0);A.busy=1;A.generation=arc_client_request(A.client,ARC_EXTRACT,A.path,A.folder,r->index);set_status(L"Preparing extraction for Operations…");}
 }
 if(path)CoTaskMemFree(path);if(item)IShellItem_Release(item);IFileDialog_Release(dialog);if(SUCCEEDED(com))CoUninitialize();
}
static void paint(HDC dc){RECT all={0,0,A.w,A.h};HBRUSH bg=CreateSolidBrush(C_BG);FillRect(dc,&all,bg);DeleteObject(bg);SelectObject(dc,app_font());SetBkMode(dc,TRANSPARENT);SetTextColor(dc,C_TEXT);
 RECT title={app_px(A.folder&&*A.folder?30:8),0,A.w-app_px(30),header()};wchar_t label[ARCP_NAME+32];const wchar_t *leaf=A.path?wcsrchr(A.path,L'\\'):NULL;leaf=leaf?leaf+1:A.path?A.path:L"Archive";
 swprintf(label,ARCP_NAME+32,L"%ls  /%ls",leaf,A.folder?A.folder:L"");DrawTextW(dc,label,-1,&title,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
 if(A.folder&&*A.folder){RECT up={0,0,app_px(26),header()};DrawTextW(dc,L"↑",1,&up,DT_SINGLELINE|DT_VCENTER|DT_CENTER);}
 RECT close={A.w-app_px(28),0,A.w,header()};DrawTextW(dc,L"×",1,&close,DT_SINGLELINE|DT_VCENTER|DT_CENTER);
 int y=header();for(uint32_t i=(uint32_t)A.top;i<A.count&&y<A.h-app_px(26);i++,y+=row_h()){
  arc_row *r=&A.rows[i];RECT area={0,y,A.w,y+row_h()};if((int)i==A.cursor){HBRUSH b=CreateSolidBrush(GetFocus()==A.hwnd?C_SEL:C_SEL_IDLE);FillRect(dc,&area,b);DeleteObject(b);}
  app_draw_icon(dc,r->name,!!(r->flags&ARCP_DIR),-1,app_px(7),y,row_h());RECT name={app_px(30),y,A.w-app_px(86),y+row_h()};SetTextColor(dc,r->flags&(ARCP_UNSAFE|ARCP_SPECIAL)?C_DIM:C_TEXT);DrawTextW(dc,r->name,-1,&name,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
  if(!(r->flags&ARCP_DIR)){wchar_t size[40];if(r->flags&ARCP_UNKNOWN_SIZE)wcscpy(size,L"—");else if(r->size<1024)swprintf(size,40,L"%llu B",(unsigned long long)r->size);else if(r->size<1048576)swprintf(size,40,L"%.1f KiB",r->size/1024.0);else swprintf(size,40,L"%.1f MiB",r->size/1048576.0);RECT bytes={A.w-app_px(84),y,A.w-app_px(6),y+row_h()};SetTextColor(dc,C_DIM);DrawTextW(dc,size,-1,&bytes,DT_SINGLELINE|DT_VCENTER|DT_RIGHT);}
 }
 RECT status={app_px(7),A.h-app_px(26),A.w-app_px(7),A.h};SetTextColor(dc,C_DIM);DrawTextW(dc,A.status,-1,&status,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
}
static void result_ready(void){arc_result *r=arc_client_take(A.client);if(!r)return;if(r->generation!=A.generation||!A.path){arc_result_free(r);return;}A.busy=0;A.backend=r->backend;
 if(r->error)set_status(error_text(r->error));
 else if(r->action==ARC_OPEN||r->action==ARC_FOLDER){arc_rows_free(A.rows,A.count);A.rows=r->rows;r->rows=NULL;A.count=r->count;r->count=0;A.cursor=-1;A.top=0;swprintf(A.status,200,L"%u items · %ls · Read-only",A.count,A.backend==1?L"ZIP / miniz":L"libarchive");scroll();}
 else if(r->action==ARC_PREVIEW){arc_row *row=selected();if(row&&r->file){arc_file_release(A.preview);A.preview=r->file;r->file=NULL;app_archive_preview(row->name,A.preview->path,row->size);set_status(L"Preview open · Archive unchanged");}}
 else if(r->action==ARC_EXTRACT&&A.destination&&r->file){if(app_stage_archive_copy(r->file->path,A.destination,arc_file_release,r->file)){r->file=NULL;set_status(L"Extraction ready — choose conflicts and Start in Operations");}else set_status(L"Extraction not queued — enable Operations and try again");}
 arc_result_free(r);InvalidateRect(A.hwnd,NULL,FALSE);
}
static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l){if(m==WM_NCCREATE)A.hwnd=h;switch(m){
 case ARC_RESULT:result_ready();return 0;case ARC_RESULT+1:return 0;
 case WM_SIZE:A.w=LOWORD(l);A.h=HIWORD(l);scroll();return 0;
 case WM_ERASEBKGND:return 1;case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);paint(dc);EndPaint(h,&ps);return 0;}
 case WM_SETFOCUS:case WM_KILLFOCUS:InvalidateRect(h,NULL,FALSE);return 0;
 case WM_GETDLGCODE:return DLGC_WANTALLKEYS;
 case WM_LBUTTONDOWN:app_focus_window(h);if(GET_Y_LPARAM(l)<header()){if(GET_X_LPARAM(l)>A.w-app_px(30))app_archive_close();else if(GET_X_LPARAM(l)<app_px(24)&&A.folder&&*A.folder)run_id("archive.up",FN_FROM_KEY);return 0;}select_row(A.top+(GET_Y_LPARAM(l)-header())/row_h());return 0;
 case WM_LBUTTONDBLCLK:if(GET_Y_LPARAM(l)>=header())run_id("archive.preview",FN_FROM_MOUSE);return 0;
 case WM_RBUTTONDOWN:app_focus_window(h);if(GET_Y_LPARAM(l)>=header())select_row(A.top+(GET_Y_LPARAM(l)-header())/row_h());return 0;
 case WM_MOUSEWHEEL:A.top-=GET_WHEEL_DELTA_WPARAM(w)/WHEEL_DELTA*3;scroll();return 0;
 case WM_VSCROLL:{SCROLLINFO s={sizeof s,SIF_TRACKPOS};GetScrollInfo(h,SB_VERT,&s);switch(LOWORD(w)){case SB_LINEUP:A.top--;break;case SB_LINEDOWN:A.top++;break;case SB_PAGEUP:A.top-=page();break;case SB_PAGEDOWN:A.top+=page();break;case SB_THUMBTRACK:A.top=s.nTrackPos;break;}scroll();return 0;}
 case WM_KEYDOWN:if(w==VK_ESCAPE){app_archive_close();return 0;}if(w==VK_TAB){app_focus_cycle(GetKeyState(VK_SHIFT)<0?-1:1);return 0;}if(w==VK_BACK){run_id("archive.up",FN_FROM_KEY);return 0;}if(w==VK_RETURN){run_id("archive.preview",FN_FROM_KEY);return 0;}
  if(w==VK_UP||w==VK_DOWN||w==VK_HOME||w==VK_END||w==VK_PRIOR||w==VK_NEXT){int at=A.cursor<0?0:A.cursor+(w==VK_UP?-1:w==VK_DOWN?1:w==VK_PRIOR?-page():w==VK_NEXT?page():0);if(w==VK_HOME)at=0;if(w==VK_END)at=(int)A.count-1;if(at<0)at=0;if(at>=(int)A.count)at=(int)A.count-1;select_row(at);return 0;}
  {fn_ctx c;app_preview_ctx(&c,A.hwnd,FN_FROM_KEY);uint8_t mods=(GetKeyState(VK_CONTROL)<0?FN_CTRL:0)|(GetKeyState(VK_SHIFT)<0?FN_SHIFT:0)|(GetKeyState(VK_MENU)<0?FN_ALT:0);fn_key((uint8_t)w,mods,&c);app_ctx_free(&c);}return 0;
 case WM_CONTEXTMENU:{if(!selected())return 0;fn_ctx c;app_preview_ctx(&c,A.hwnd,FN_FROM_MENU);uint64_t revision=A.revision;HMENU menu=CreatePopupMenu();fn_menu(menu,FN_MENU_ARCHIVE,&c);POINT p={GET_X_LPARAM(l),GET_Y_LPARAM(l)};if(l==-1){p=(POINT){app_px(30),header()+(A.cursor-A.top+1)*row_h()};ClientToScreen(h,&p);}UINT id=app_menu_track(menu,h,p);DestroyMenu(menu);if(id&&revision==A.revision&&app_menu_context_current(&c))fn_menu_run(id,&c);app_ctx_free(&c);return 0;}
 }return DefWindowProcW(h,m,w,l);}
int archiveview_available(void){wchar_t path[MAX_PATH+20];DWORD n=GetModuleFileNameW(NULL,path,MAX_PATH);wchar_t *s=n&&n<MAX_PATH?wcsrchr(path,L'\\'):NULL;if(!s)return 0;wcscpy(s+1,L"folderwing-zip.exe");return GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES;}
int archiveview_start(HWND parent){if(A.hwnd)return 0;WNDCLASSW wc={0};wc.lpfnWndProc=proc;wc.hInstance=GetModuleHandleW(NULL);wc.lpszClassName=L"OpenPilotArchive";wc.hCursor=LoadCursorW(NULL,IDC_ARROW);wc.style=CS_DBLCLKS;RegisterClassW(&wc);
 A.hwnd=CreateWindowExW(0,wc.lpszClassName,L"Archive contents",WS_CHILD|WS_VSCROLL|WS_CLIPSIBLINGS,0,0,0,0,parent,NULL,wc.hInstance,NULL);if(!A.hwnd)return -1;A.client=arc_client_start(A.hwnd,ARC_RESULT);A.cursor=-1;if(!A.client){DestroyWindow(A.hwnd);A.hwnd=NULL;return -1;}return 0;}
HWND archiveview_window(void){return A.hwnd;}
void archiveview_clear(void){if(!A.hwnd)return;A.revision++;clear_content();app_clipboard_cancel(A.hwnd);arc_client_stop(&A.client);arc_rows_free(A.rows,A.count);A.rows=NULL;A.count=0;free(A.path);free(A.folder);free(A.destination);A.path=A.folder=A.destination=NULL;A.busy=0;A.cursor=-1;A.generation=0;A.status[0]=0;}
void archiveview_stop(void){if(!A.hwnd)return;archiveview_clear();arc_client_stop(&A.client);HWND h=A.hwnd;A.hwnd=NULL;DestroyWindow(h);memset(&A,0,sizeof A);}
void archiveview_open(const wchar_t *path,uint64_t size,int64_t mtime){if(provider_path_check(path,1)){archiveview_clear();set_status(L"Archive path is unsupported by Windows access");return;}if(A.path&&!wcscmp(A.path,path)&&A.size==size&&A.mtime==mtime)return;archiveview_clear();if(!A.client)A.client=arc_client_start(A.hwnd,ARC_RESULT);A.path=_wcsdup(path);A.folder=_wcsdup(L"");A.size=size;A.mtime=mtime;A.busy=1;A.cursor=-1;if(!A.client||!A.path||!A.folder){set_status(L"Archive preview could not start");return;}A.generation=arc_client_request(A.client,ARC_OPEN,A.path,A.folder,0);set_status(L"Reading archive contents…");}
