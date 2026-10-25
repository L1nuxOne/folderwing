/* Read-only diff panel. Files, decoding and alignment stay on one worker.
 * The UI publishes a result only after joining that worker; old pairs never
 * supply navigation for a newly requested pair. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>
#include "diff.h"
#include "diffproto.h"
#include "gitread.h"
#include "panelproto.h"
#include "theme.h"
#define WM_HOST (WM_APP+1)
#define WM_GONE (WM_APP+2)
typedef struct msg {panelp_head h;uint8_t data[];} msg;
typedef struct hot {RECT r;int id;} hot;
static struct {
 HANDLE in,out,section,thread,cancel;HWND hwnd;HDC dc;HBITMAP bitmap,original;HFONT font,bold,mono;uint8_t *shared;void *pixels;
 int w,h,dpi,next,waiting,pending,shown,focused,top,column,selected,focus,ready,timed_out;
 uint32_t seq;wchar_t *path[2],*queued[2];diff_open request,queued_request;
 git_diff_open git,queued_git;diff_result result,work;uint64_t started;hot hot[5];int nhot;RECT list,track;
uint32_t run_wsl;
} A;
static int S(int n){return MulDiv(n,A.dpi,96);}
static int io(HANDLE h,void *p,DWORD n,int output){while(n){DWORD got=0;BOOL ok=output?WriteFile(h,p,n,&got,NULL):ReadFile(h,p,n,&got,NULL);if(!ok||!got)return 0;p=(char*)p+got;n-=got;}return 1;}
static void send(uint32_t type,const void *data,uint32_t n){panelp_head h={type,n};if(!io(A.out,&h,sizeof h,1)||(n&&!io(A.out,(void*)data,n,1)))PostQuitMessage(1);}
static DWORD WINAPI reader(void *window){for(;;){panelp_head h;if(!io(A.in,&h,sizeof h,0)||h.size>PANELP_MAX)break;msg *m=malloc(sizeof *m+h.size);if(!m)break;m->h=h;if(!io(A.in,m->data,h.size,0)){free(m);break;}if(!PostMessageW(window,WM_HOST,0,(LPARAM)m)){free(m);break;}}PostMessageW(window,WM_GONE,0,0);return 0;}
static void fonts(void){if(A.font)DeleteObject(A.font);if(A.bold)DeleteObject(A.bold);if(A.mono)DeleteObject(A.mono);A.font=CreateFontW(-MulDiv(90,A.dpi,720),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");A.bold=CreateFontW(-MulDiv(95,A.dpi,720),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");A.mono=CreateFontW(-MulDiv(90,A.dpi,720),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");}
static int map(HANDLE section,int w,int h){if(w<1||h<1||w>16384||h>16384)return 0;if(A.shared)UnmapViewOfFile(A.shared);if(A.section)CloseHandle(A.section);if(A.bitmap){SelectObject(A.dc,A.original);DeleteObject(A.bitmap);}A.section=section;A.shared=MapViewOfFile(section,FILE_MAP_WRITE,0,0,0);A.w=w;A.h=h;A.waiting=0;A.next=0;if(!A.dc)A.dc=CreateCompatibleDC(NULL);BITMAPINFO b={{sizeof b.bmiHeader,w,-h,1,32,BI_RGB}};A.bitmap=CreateDIBSection(NULL,&b,DIB_RGB_COLORS,&A.pixels,NULL,0);if(A.bitmap)A.original=SelectObject(A.dc,A.bitmap);return A.shared&&A.bitmap;}
static void fill(RECT r,COLORREF color){HBRUSH b=CreateSolidBrush(color);FillRect(A.dc,&r,b);DeleteObject(b);}
static void text(const wchar_t *s,RECT r,COLORREF color,HFONT font,UINT flags){HGDIOBJ old=SelectObject(A.dc,font);SetTextColor(A.dc,color);SetBkMode(A.dc,TRANSPARENT);DrawTextW(A.dc,s,-1,&r,flags|DT_NOPREFIX);SelectObject(A.dc,old);}
static void button(const wchar_t *s,int x,int width,int id){RECT r={x,S(11),x+width,S(39)};fill(r,A.focus==id?C_SEL:C_EDIT);text(s,r,C_TEXT,A.font,DT_CENTER|DT_SINGLELINE|DT_VCENTER);if(A.nhot<5)A.hot[A.nhot++]=(hot){r,id};}
static void present(void){if(!A.shared||!A.pixels||!A.shown)return;if(A.waiting){A.pending=1;return;}GdiFlush();size_t n=(size_t)A.w*A.h*4;memcpy(A.shared+n*A.next,A.pixels,n);panelp_frame f={(uint32_t)A.next,A.seq,{0,0,A.w,A.h}};send(PANELP_FRAME,&f,sizeof f);A.next^=1;A.waiting=1;A.pending=0;}
static int visible(void){int n=(A.list.bottom-A.list.top)/S(21);return n<1?1:n;}
static void clamp(void){int max=(int)A.result.count-visible();if(max<0)max=0;if(A.top>max)A.top=max;if(A.top<0)A.top=0;if(A.column<0)A.column=0;if(A.column>1000000)A.column=1000000;}
/* Expand only the visible slice of a line. Very long lines never allocate or
 * render their complete width on a UI tick. Tabs use four-column stops. */
static void line_text(const diff_doc *doc,uint32_t index,wchar_t out[1024]){
 const diff_line *l=&doc->lines[index];uint32_t col=0,used=0,end=l->start+l->length;
 for(uint32_t i=l->start;i<end&&used<1000;){uint32_t units=1,cells=text_cell(doc->data.text,i,end,col,&units);if(!cells)cells=1;
  if(doc->data.text[i]=='\t'){for(uint32_t k=0;k<cells&&used<1000;k++)if(col+k>=(uint32_t)A.column)out[used++]=' ';}
  else if(col>=(uint32_t)A.column){for(uint32_t k=0;k<units&&used<1000;k++)out[used++]=doc->data.text[i+k];}
  col+=cells;i+=units;if(col>(uint32_t)A.column+700)break;
 }if(!l->newline&&used<990){const wchar_t *e=L"  [no final newline]";if(index+1==doc->count)while(*e&&used<1023)out[used++]=*e++;}out[used]=0;
}
static void paint(void){if(!A.bitmap||!A.shown)return;A.nhot=0;fill((RECT){0,0,A.w,A.h},C_BG);button(L"Previous",S(10),S(82),1);button(L"Next",S(99),S(60),2);button(A.thread?L"Stop":L"Refresh",S(166),S(75),3);
 wchar_t s[700];int chosen=A.selected>=0&&A.selected<(int)A.result.count?(int)A.result.rows[A.selected].hunk:0;
 if(A.thread)wcscpy(s,A.queued[0]?L"Changing comparison…":WaitForSingleObject(A.cancel,0)==WAIT_OBJECT_0?L"Stopping…":L"Comparing…");
 else if(!A.path[0])wcscpy(s,L"Select one file in each pane");
 else if(A.result.error==ERROR_CANCELLED)wcscpy(s,A.timed_out?L"Stopped — 60 second limit":L"Stopped — comparison incomplete");
 else if(A.result.error)swprintf(s,700,L"%ls file could not be compared (Windows error %lu)",A.result.error_side?L"Right":L"Left",(unsigned long)A.result.error);
 else if(A.ready&&A.result.mode==DIFF_TEXT)swprintf(s,700,L"%u change%ls%ls%ls · %ls",A.result.hunks,A.result.hunks==1?L"":L"s",A.result.coarse?L" · coarse alignment":L"",A.result.hunks&&!chosen?L"":L"",A.result.bytes_equal?L"bytes identical":L"bytes differ");
 else if(A.ready)swprintf(s,700,L"%ls · %ls",A.result.mode==DIFF_BINARY?L"Binary comparison":L"Byte comparison (text view limit)",A.result.bytes_equal?L"identical":L"different");else wcscpy(s,L"Loading…");
 text(s,(RECT){S(10),S(47),A.w-S(10),S(72)},A.result.error?C_ERROR:C_TEXT,A.font,DT_SINGLELINE|DT_END_ELLIPSIS);
 int half=(A.w-S(12))/2;for(int side=0;side<2;side++){int x=side*half;fill((RECT){x,S(80),x+half,S(139)},C_HEADER);const wchar_t *p=A.queued[side]?A.queued[side]:A.path[side];wchar_t gitlabel[32768];if(A.request.reserved&&!A.queued[0]){swprintf(gitlabel,32768,L"%ls · %ls",A.request.reserved==2?(side?L"Index":L"HEAD"):(side?L"Working file":L"Index"),A.path[1]);p=gitlabel;}text(p?p:side?L"Right":L"Left",(RECT){x+S(9),S(85),x+half-S(6),S(109)},side?C_LINK:C_ACCENT,A.font,DT_SINGLELINE|DT_PATH_ELLIPSIS);
  if(A.ready){swprintf(s,700,L"%llu bytes · %ls%ls",(unsigned long long)A.result.doc[side].bytes,A.result.mode==DIFF_TEXT?text_encoding_name(A.result.doc[side].data.encoding):L"complete byte comparison",A.result.mode==DIFF_TEXT&&A.result.doc[side].data.flags&(TEXTP_GUESSED|TEXTP_REPLACED)?L" (guessed/replaced)":L"");text(s,(RECT){x+S(9),S(112),x+half-S(6),S(134)},C_DIM,A.font,DT_SINGLELINE|DT_END_ELLIPSIS);}}
 A.list=(RECT){0,S(141),A.w-S(12),A.h-S(58)};A.track=(RECT){A.w-S(11),A.list.top,A.w,A.list.bottom};clamp();
 int saved=SaveDC(A.dc);IntersectClipRect(A.dc,0,A.list.top,A.list.right,A.list.bottom);
 if(A.ready&&A.result.mode==DIFF_TEXT){for(int i=A.top;i<(int)A.result.count&&i<A.top+visible()+1;i++){diff_row *r=&A.result.rows[i];int y=A.list.top+(i-A.top)*S(21);for(int side=0;side<2;side++){int x=side*half;uint32_t index=side?r->right:r->left;COLORREF color=r->kind==DIFF_SAME?C_BG:index==DIFF_NO_LINE?C_TREE_BG:r->kind==DIFF_CHANGED?RGB(65,56,32):side?RGB(30,61,44):RGB(68,35,38);fill((RECT){x,y,x+half,y+S(21)},color);
   if(index!=DIFF_NO_LINE){swprintf(s,700,L"%u",index+1);text(s,(RECT){x+S(3),y,x+S(48),y+S(21)},C_DIM,A.mono,DT_SINGLELINE|DT_RIGHT|DT_VCENTER);wchar_t line[1024];line_text(&A.result.doc[side],index,line);int clip=SaveDC(A.dc);IntersectClipRect(A.dc,x+S(57),y,x+half-S(3),y+S(21));text(line,(RECT){x+S(57),y,x+half-S(3),y+S(21)},C_TEXT,A.mono,DT_SINGLELINE|DT_VCENTER);RestoreDC(A.dc,clip);}
   if(i==A.selected){HBRUSH brush=CreateSolidBrush(C_ACCENT);FrameRect(A.dc,&(RECT){x,y,x+half,y+S(21)},brush);DeleteObject(brush);}}}
  if(!A.result.count)text(L"Both files are empty",(RECT){S(12),A.list.top+S(12),A.list.right,A.list.top+S(45)},C_DIM,A.font,DT_SINGLELINE);
 }else{const wchar_t *explain=A.thread?L"File I/O and alignment run in the helper worker.":A.result.error?L"No equality conclusion is available. Refresh to try again.":A.ready?(A.result.mode==DIFF_BINARY?L"Binary data: full-file equality and sizes are shown above.":L"Text view: up to 2 MiB and 100,000 lines per file. Byte equality covers the whole file."):L"Compare files to see aligned text here.";text(explain,(RECT){S(12),A.list.top+S(16),A.list.right-S(10),A.list.top+S(110)},C_DIM,A.font,DT_WORDBREAK);}
 RestoreDC(A.dc,saved);fill((RECT){half,S(80),half+1,A.list.bottom},C_BORDER);
 if(A.ready&&A.result.count>(uint32_t)visible()){fill(A.track,C_EDIT);int height=A.track.bottom-A.track.top;int thumb=MulDiv(visible(),height,(int)A.result.count);if(thumb<S(18))thumb=S(18);int y=A.track.top+MulDiv(A.top,height-thumb,(int)A.result.count-visible());fill((RECT){A.track.left,y,A.track.right,y+thumb},C_DIM);}
 swprintf(s,700,L"%ls%ls",A.ready&&A.result.mode==DIFF_TEXT&&!A.result.hunks&&!A.result.bytes_equal?L"Decoded text matches; encoding, line endings or decoding differ. ":L"",A.result.coarse?L"Large rewrite: middle shown as one coarse change.":L"Read-only · scrolling is linked");text(s,(RECT){S(10),A.h-S(54),A.w-S(10),A.h-S(32)},C_DIM,A.font,DT_SINGLELINE|DT_END_ELLIPSIS);
 text(L"F7 / Shift+F7: changes · ↑↓ PgUp/PgDn: scroll · ←→: horizontal",(RECT){S(10),A.h-S(29),A.w-S(10),A.h-S(7)},C_DIM,A.font,DT_SINGLELINE|DT_END_ELLIPSIS);present();
}
static void report(void){diff_state s={.session=A.request.session,.state=A.thread?DIFF_LOADING:A.result.error==ERROR_CANCELLED?DIFF_STOPPED:A.result.error?DIFF_FAILED:A.ready?DIFF_READY:DIFF_LOADING,.hunks=A.ready?A.result.hunks:0,.mode=A.result.mode,.error=A.result.error,.bytes_equal=A.ready?A.result.bytes_equal:-1,.coarse=A.result.coarse};send(PANELP_DIFF_STATE,&s,sizeof s);}
static void stop(void){if(A.cancel)SetEvent(A.cancel);if(A.thread)CancelSynchronousIo(A.thread);}
static DWORD WINAPI worker(void *unused){(void)unused;if(A.request.reserved)git_diff_read(A.path[0],A.path[1],&A.git,A.cancel,&A.work);else diff_files(A.path[0],A.path[1],A.run_wsl,A.cancel,&A.work);return 0;}
static void begin(void){if(A.thread||!A.path[0]||!A.path[1])return;A.ready=0;A.top=A.column=0;A.selected=-1;A.timed_out=0;diff_free(&A.result);diff_free(&A.work);ResetEvent(A.cancel);A.started=GetTickCount64();A.run_wsl=A.request.wsl;A.git.wsl=A.run_wsl;A.thread=CreateThread(NULL,0,worker,NULL,0,NULL);if(!A.thread)A.result.error=GetLastError();report();paint();}
static void adopt(void){if(A.thread||!A.queued[0])return;for(int i=0;i<2;i++){free(A.path[i]);A.path[i]=A.queued[i];A.queued[i]=NULL;}A.request=A.queued_request;A.git=A.queued_git;begin();}
static void queue(const wchar_t *left,const wchar_t *right,const diff_open *request){
 if(A.path[0]&&A.path[1]&&A.request.session==request->session&&!wcscmp(A.path[0],left)&&!wcscmp(A.path[1],right))return;
 wchar_t *a=_wcsdup(left),*b=_wcsdup(right);if(!a||!b){free(a);free(b);return;}stop();free(A.queued[0]);free(A.queued[1]);A.queued[0]=a;A.queued[1]=b;A.queued_request=*request;A.ready=0;adopt();paint();}
static void tick(void){if(!A.thread)return;if(GetTickCount64()-A.started>60000){A.timed_out=1;stop();}if(WaitForSingleObject(A.thread,0)!=WAIT_OBJECT_0)return;CloseHandle(A.thread);A.thread=NULL;if(WaitForSingleObject(A.cancel,0)==WAIT_OBJECT_0)A.work.error=ERROR_CANCELLED;diff_free(&A.result);A.result=A.work;memset(&A.work,0,sizeof A.work);A.ready=!A.result.error;if(A.queued[0])adopt();else{report();paint();}}
static void navigate(int direction){if(!A.ready||A.result.mode!=DIFF_TEXT)return;int row=diff_change(&A.result,A.selected,direction);if(row<0)return;A.selected=row;A.top=row>2?row-2:0;clamp();}
static void activate(int id){A.focus=id;if(id==1||id==2)navigate(id==1?-1:1);else if(id==3){if(A.thread)stop();else begin();}paint();}
static void input(const panelp_input *in){if(in->msg==WM_LBUTTONDOWN){for(int i=0;i<A.nhot;i++)if(PtInRect(&A.hot[i].r,(POINT){in->x,in->y})){activate(A.hot[i].id);return;}A.focus=0;if(PtInRect(&A.track,(POINT){in->x,in->y})&&A.ready){int span=A.track.bottom-A.track.top;if(span>0)A.top=MulDiv(in->y-A.track.top,(int)A.result.count,span);}else if(PtInRect(&A.list,(POINT){in->x,in->y})&&A.ready){int at=A.top+(in->y-A.list.top)/S(21);if(at<(int)A.result.count)A.selected=at;}}
 else if(in->msg==WM_MOUSEWHEEL){int delta=(short)HIWORD(in->wparam)/WHEEL_DELTA;if(in->mods&PANELP_M_SHIFT)A.column-=delta*4;else A.top-=delta*3;}
 else if(in->msg==WM_KEYDOWN){UINT key=in->wparam;if(key==VK_TAB)A.focus=(A.focus+(in->mods&PANELP_M_SHIFT?3:1))%4;else if((key==VK_RETURN||key==VK_SPACE)&&A.focus)activate(A.focus);else if(key==VK_F7)navigate(in->mods&PANELP_M_SHIFT?-1:1);else if(key==VK_F5){if(!A.thread)begin();}else if(key==VK_ESCAPE){if(A.thread)stop();else send(PANELP_CLOSE,NULL,0);}else if(key==VK_UP)A.top--;else if(key==VK_DOWN)A.top++;else if(key==VK_PRIOR)A.top-=visible();else if(key==VK_NEXT)A.top+=visible();else if(key==VK_HOME)A.top=0;else if(key==VK_END)A.top=(int)A.result.count;else if(key==VK_LEFT)A.column-=4;else if(key==VK_RIGHT)A.column+=4;else return;}
 else return;clamp();paint();}
static void on_host(msg *m){switch(m->h.type){case PANELP_DIFF_WSL_POLICY:if(m->h.size==4){uint32_t enabled;memcpy(&enabled,m->data,4);if(enabled<=1){A.request.wsl=enabled;A.queued_request.wsl=enabled;}}break;
 case PANELP_PING:if(m->h.size==4)send(PANELP_PONG,m->data,4);break;case PANELP_FRAME_ACK:A.waiting=0;if(A.pending)present();break;
 case PANELP_INPUT:if(m->h.size==sizeof(panelp_input))input((panelp_input*)m->data);break;
 case PANELP_SHOW:if(m->h.size==4)A.shown=*(uint32_t*)m->data!=0;if(A.shown)paint();break;
 case PANELP_FOCUS:if(m->h.size==4)A.focused=*(uint32_t*)m->data!=0;paint();break;
 case PANELP_DIFF_GIT_OPEN:if(m->h.size>=sizeof(git_diff_open)){git_diff_open r;memcpy(&r,m->data,sizeof r);uint64_t units=(uint64_t)r.root_chars+r.path_chars;if(!r.session||r.wsl>1||r.as_root>1||r.staged>1||r.root_chars<2||r.root_chars>32768||r.path_chars<2||r.path_chars>32768||r.meta.head[64]||r.meta.index[64]||m->h.size!=sizeof r+units*2)break;const wchar_t *root=(const wchar_t*)(m->data+sizeof r),*path=root+r.root_chars;if(root[r.root_chars-1]||path[r.path_chars-1]||wcslen(root)!=r.root_chars-1||wcslen(path)!=r.path_chars-1||!git_path_safe(path))break;if(A.request.session==r.session&&A.request.reserved)break;A.queued_git=r;diff_open request={.session=r.session,.wsl=r.wsl,.left_chars=r.root_chars,.right_chars=r.path_chars,.reserved=r.staged?2:1};queue(root,path,&request);}break;
 case PANELP_DIFF_OPEN:if(m->h.size>=sizeof(diff_open)){diff_open r;memcpy(&r,m->data,sizeof r);uint64_t units=(uint64_t)r.left_chars+r.right_chars;if(r.session&&r.wsl<=1&&r.left_chars>1&&r.left_chars<=32768&&r.right_chars>1&&r.right_chars<=32768&&m->h.size==sizeof r+units*2){const wchar_t *a=(const wchar_t*)(m->data+sizeof r),*b=a+r.left_chars;if(!a[r.left_chars-1]&&!b[r.right_chars-1]&&wcslen(a)==r.left_chars-1&&wcslen(b)==r.right_chars-1)queue(a,b,&r);}}break;
 case PANELP_RESIZE:if(m->h.size==sizeof(panelp_size)){panelp_size s;memcpy(&s,m->data,sizeof s);if(s.dpi&&s.dpi!=(uint32_t)A.dpi){A.dpi=s.dpi;fonts();}A.seq=s.seq;if(map((HANDLE)(uintptr_t)s.section,s.w,s.h))paint();}break;
 case PANELP_QUIT:PostQuitMessage(0);break;}}
static LRESULT CALLBACK hidden(HWND h,UINT m,WPARAM w,LPARAM l){if(m==WM_TIMER){tick();return 0;}return DefWindowProcW(h,m,w,l);}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,PWSTR command,int show){A.in=GetStdHandle(STD_INPUT_HANDLE);A.out=GetStdHandle(STD_OUTPUT_HANDLE);A.shown=1;A.selected=-1;A.cancel=CreateEventW(NULL,TRUE,FALSE,NULL);if(!A.cancel)return 1;panelp_head head;panelp_hello hi;if(!io(A.in,&head,sizeof head,0)||head.type!=PANELP_HELLO||head.size!=sizeof hi||!io(A.in,&hi,sizeof hi,0)||hi.magic!=PANELP_MAGIC||hi.version!=PANELP_VERSION)return 1;SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);A.dpi=hi.dpi?hi.dpi:96;A.seq=hi.seq;fonts();WNDCLASSW cls={.lpfnWndProc=hidden,.hInstance=instance,.lpszClassName=L"FpDiffHidden"};RegisterClassW(&cls);A.hwnd=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"File differences",WS_POPUP,0,0,0,0,NULL,NULL,instance,NULL);if(!A.hwnd||!map((HANDLE)(uintptr_t)hi.section,hi.w,hi.h))return 1;HANDLE t=CreateThread(NULL,65536,reader,A.hwnd,STACK_SIZE_PARAM_IS_A_RESERVATION,NULL);if(!t)return 1;CloseHandle(t);panelp_ready ready={GetCurrentProcessId(),0,L"File differences"};send(PANELP_READY,&ready,sizeof ready);SetTimer(A.hwnd,1,100,NULL);paint();MSG m;while(GetMessageW(&m,NULL,0,0)>0){if(m.message==WM_HOST){on_host((msg*)m.lParam);free((void*)m.lParam);}else if(m.message==WM_GONE)break;else DispatchMessageW(&m);}KillTimer(A.hwnd,1);stop();if(A.thread&&WaitForSingleObject(A.thread,3000)!=WAIT_OBJECT_0)return 0;if(A.thread)CloseHandle(A.thread);diff_free(&A.work);diff_free(&A.result);for(int i=0;i<2;i++){free(A.path[i]);free(A.queued[i]);}CloseHandle(A.cancel);return 0;}
