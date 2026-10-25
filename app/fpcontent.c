/* Find in files: on-demand panel, one cancellable search, bounded retained matches. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>
#include <locale.h>
#include "content.h"
#include "contentproto.h"
#include "panelproto.h"
#include "theme.h"
#define WM_HOST (WM_APP+1)
#define WM_GONE (WM_APP+2)
typedef struct msg {panelp_head h;uint8_t data[];} msg;
typedef struct hot {RECT r;int action;uint32_t id;} hot;
static struct {
 HANDLE in,out,section,thread,cancel;HWND hwnd;HDC dc;HBITMAP bitmap,original;HFONT font,bold,small;uint8_t *shared;void *pixels;
 int w,h,dpi,next,waiting,pending,shown,focused;uint32_t seq;CRITICAL_SECTION lock;
 cf_model model;wchar_t *root,*pending_root;content_open request,pending_request;cf_options run_options;
 wchar_t query[CF_QUERY],pattern[CF_QUERY],message[200];uint32_t flags,generation,run_generation;
 uint32_t selected;int top,focus,caret[2],all[2],stale,pending_start;hot hot[32];int nhot;RECT list,fields[2];
 uint64_t started,ended;content_state last_report;int reported;
uint32_t run_wsl;
} A;
static int S(int n){return MulDiv(n,A.dpi,96);}
static int io(HANDLE h,void *p,DWORD n,int output){while(n){DWORD done;BOOL ok=output?WriteFile(h,p,n,&done,NULL):ReadFile(h,p,n,&done,NULL);if(!ok||!done)return 0;p=(char*)p+done;n-=done;}return 1;}
static void send(uint32_t type,const void *data,uint32_t n){panelp_head h={type,n};if(!io(A.out,&h,sizeof h,1)||(n&&!io(A.out,(void*)data,n,1)))PostQuitMessage(1);}
static DWORD WINAPI reader(void *arg){for(;;){panelp_head h;if(!io(A.in,&h,sizeof h,0)||h.size>PANELP_MAX)break;msg *m=malloc(sizeof *m+h.size);if(!m)break;m->h=h;if(!io(A.in,m->data,h.size,0)){free(m);break;}if(!PostMessageW(arg,WM_HOST,0,(LPARAM)m)){free(m);break;}}PostMessageW(arg,WM_GONE,0,0);return 0;}
static void fonts(void){if(A.font)DeleteObject(A.font);if(A.bold)DeleteObject(A.bold);if(A.small)DeleteObject(A.small);A.font=CreateFontW(-MulDiv(90,A.dpi,720),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");A.bold=CreateFontW(-MulDiv(95,A.dpi,720),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");A.small=CreateFontW(-MulDiv(80,A.dpi,720),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");}
static int map(HANDLE section,int w,int h){if(w<1||h<1||w>16384||h>16384)return 0;if(A.shared)UnmapViewOfFile(A.shared);if(A.section)CloseHandle(A.section);if(A.bitmap){SelectObject(A.dc,A.original);DeleteObject(A.bitmap);}A.section=section;A.shared=MapViewOfFile(section,FILE_MAP_WRITE,0,0,0);A.w=w;A.h=h;A.waiting=0;A.next=0;if(!A.dc)A.dc=CreateCompatibleDC(NULL);BITMAPINFO b={{sizeof b.bmiHeader,w,-h,1,32,BI_RGB}};A.bitmap=CreateDIBSection(NULL,&b,DIB_RGB_COLORS,&A.pixels,NULL,0);if(A.bitmap)A.original=SelectObject(A.dc,A.bitmap);return A.shared&&A.bitmap;}
static void fill(RECT r,COLORREF color){HBRUSH b=CreateSolidBrush(color);FillRect(A.dc,&r,b);DeleteObject(b);}
static void text(const wchar_t *s,RECT r,COLORREF color,HFONT font,UINT flags){HGDIOBJ old=SelectObject(A.dc,font);SetTextColor(A.dc,color);SetBkMode(A.dc,TRANSPARENT);DrawTextW(A.dc,s,-1,&r,flags|DT_NOPREFIX);SelectObject(A.dc,old);}
static void button(const wchar_t *s,int x,int y,int w,int action,uint32_t id,int chosen){RECT r={x,y,x+w,y+S(26)};fill(r,chosen?C_SEL:C_EDIT);if(A.focused&&A.focus==action){HBRUSH b=CreateSolidBrush(C_ACCENT);FrameRect(A.dc,&r,b);DeleteObject(b);}text(s,r,C_TEXT,A.font,DT_CENTER|DT_VCENTER|DT_SINGLELINE);if(A.nhot<48)A.hot[A.nhot++]=(hot){r,action,id};}
static void present(void){if(!A.shared||!A.pixels||!A.shown)return;if(A.waiting){A.pending=1;return;}GdiFlush();size_t n=(size_t)A.w*A.h*4;memcpy(A.shared+n*A.next,A.pixels,n);panelp_frame f={(uint32_t)A.next,A.seq,{0,0,A.w,A.h}};send(PANELP_FRAME,&f,sizeof f);A.next^=1;A.waiting=1;A.pending=0;}
static void report(void){EnterCriticalSection(&A.lock);content_state r={A.request.session,A.generation,A.stale?CF_STOPPED:A.thread?CF_RUNNING:A.model.summary.state,A.model.count};LeaveCriticalSection(&A.lock);if(!A.reported||r.session!=A.last_report.session||r.generation!=A.last_report.generation||r.state!=A.last_report.state||r.matches!=A.last_report.matches){A.last_report=r;A.reported=1;send(PANELP_CONTENT_STATE,&r,sizeof r);}}
static void paint(void){if(!A.bitmap||!A.shown)return;EnterCriticalSection(&A.lock);A.nhot=0;fill((RECT){0,0,A.w,A.h},C_BG);wchar_t line[1000];
 text(A.root?A.root:L"Choose a folder",(RECT){S(10),S(7),A.w-S(10),S(29)},C_TEXT,A.bold,DT_SINGLELINE|DT_PATH_ELLIPSIS);
 for(int field=0;field<2;field++){int y=S(field?91:40);A.fields[field]=(RECT){S(10),y,A.w-S(10),y+S(35)};fill(A.fields[field],A.focus==field+1?C_SEL_IDLE:C_EDIT);const wchar_t *value=field?A.pattern:A.query;swprintf(line,1000,L"%ls: %ls%ls",field?L"Files":L"Find",value,A.focus==field+1?L"│":L"");text(line,(RECT){S(16),y,A.w-S(15),y+S(35)},*value?C_TEXT:C_DIM,A.font,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);A.hot[A.nhot++]=(hot){A.fields[field],field+1,0};}
 int gap=S(5),third=(A.w-S(20)-gap*2)/3;button(A.flags&CF_CASE?L"✓ Case":L"Case",S(10),S(137),third,3,0,A.flags&CF_CASE);button(A.flags&CF_WORD?L"✓ Whole word":L"Whole word",S(10)+third+gap,S(137),third,4,0,A.flags&CF_WORD);button(A.flags&CF_RECURSE?L"✓ Subfolders":L"Subfolders",S(10)+2*(third+gap),S(137),third,5,0,A.flags&CF_RECURSE);
 button(A.thread&&!A.stale?L"Stop":L"Start",S(10),S(173),S(86),6,0,0);
 cf_summary sum=A.model.summary;const wchar_t *state=A.stale?L"Changed query — Start to search":!A.started?L"Enter text, then Start":A.thread?WaitForSingleObject(A.cancel,0)==WAIT_OBJECT_0?L"Stopping…":L"Searching…":sum.state==CF_COMPLETE?L"Complete":sum.state==CF_STOPPED?L"Stopped — incomplete":L"Partial — limits or unreadable files";
 text(state,(RECT){S(105),S(173),A.w-S(10),S(201)},sum.state==CF_PARTIAL?C_ERROR:C_DIM,A.small,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
 swprintf(line,1000,L"%u matches · %llu files read · %.1f MiB",A.model.count,(unsigned long long)sum.files,sum.bytes/(1024.0*1024));text(line,(RECT){S(10),S(208),A.w-S(10),S(228)},C_TEXT,A.font,DT_SINGLELINE);
 swprintf(line,1000,L"%llu binary · %llu large · %llu excluded · %llu errors",(unsigned long long)sum.binary,(unsigned long long)sum.large,(unsigned long long)sum.excluded,(unsigned long long)sum.errors);text(line,(RECT){S(10),S(232),A.w-S(10),S(250)},C_DIM,A.small,DT_SINGLELINE|DT_END_ELLIPSIS);
 A.list=(RECT){0,S(261),A.w,A.h-S(98)};int visible=(A.list.bottom-A.list.top)/S(66);if(visible<1)visible=1;if(A.top>(int)A.model.count-visible)A.top=(int)A.model.count-visible;if(A.top<0)A.top=0;
 int saved=SaveDC(A.dc);IntersectClipRect(A.dc,0,A.list.top,A.w,A.list.bottom);
 for(uint32_t i=(uint32_t)A.top;i<A.model.count;i++){int y=A.list.top+((int)i-A.top)*S(66);if(y>=A.list.bottom)break;cf_result *r=&A.model.rows[i];if(i==A.selected)fill((RECT){0,y,A.w,y+S(65)},C_SEL_IDLE);
  swprintf(line,1000,L"%ls : %u : %u",r->path,r->hit.line,r->hit.column);text(line,(RECT){S(10),y+S(3),A.w-S(10),y+S(26)},A.stale?C_DIM:C_ACCENT,A.font,DT_SINGLELINE|DT_END_ELLIPSIS);
  wchar_t excerpt[CF_EXCERPT*4];uint32_t at=0,start=0,end=0;for(uint32_t j=0;j<CF_EXCERPT&&r->hit.excerpt[j];j++){if(j==r->hit.highlight)start=at;if(j==r->hit.highlight+r->hit.length)end=at;uint16_t c=r->hit.excerpt[j];if(c=='	'){for(int k=0;k<4;k++)excerpt[at++]=' ';}else excerpt[at++]=(wchar_t)c;}if(!end)end=at;excerpt[at]=0;
  SelectObject(A.dc,A.font);SIZE prefix={0},match={0};GetTextExtentPoint32W(A.dc,excerpt,(int)start,&prefix);GetTextExtentPoint32W(A.dc,excerpt+start,(int)(end-start),&match);
  int clip=SaveDC(A.dc);IntersectClipRect(A.dc,S(10),y+S(29),A.w-S(10),y+S(53));if(!A.stale)fill((RECT){S(10)+prefix.cx,y+S(29),S(10)+prefix.cx+match.cx,y+S(51)},C_SEL);text(excerpt,(RECT){S(10),y+S(29),A.w-S(10),y+S(53)},A.stale?C_DIM:C_TEXT,A.font,DT_SINGLELINE|DT_END_ELLIPSIS);RestoreDC(A.dc,clip);
 }
 if(!A.model.count)text(A.thread?L"Looking for matching text…":A.started?L"No matching results":L"Literal text search · e.g. files: *.c;*.h",(RECT){S(12),A.list.top+S(15),A.w-S(12),A.list.top+S(45)},C_DIM,A.font,DT_SINGLELINE|DT_END_ELLIPSIS);RestoreDC(A.dc,saved);
 text(A.message,(RECT){S(10),A.h-S(95),A.w-S(10),A.h-S(72)},C_DIM,A.small,DT_SINGLELINE|DT_END_ELLIPSIS);
 button(L"Preview match",S(10),A.h-S(68),(A.w-S(25))/2,10,0,0);button(L"Reveal file",S(15)+(A.w-S(25))/2,A.h-S(68),(A.w-S(25))/2,11,0,0);
 text(L"Enter: search/open · Tab: controls · Right-click: result actions",(RECT){S(10),A.h-S(32),A.w-S(10),A.h-S(9)},C_DIM,A.small,DT_SINGLELINE|DT_END_ELLIPSIS);
 LeaveCriticalSection(&A.lock);present();}
static void stop(void){if(A.cancel)SetEvent(A.cancel);if(A.thread)CancelSynchronousIo(A.thread);}
static void edited(void){A.generation++;A.stale=1;A.pending_start=0;stop();A.message[0]=0;report();}
static int collect(void *user,const cf_record *r,const wchar_t *relative){(void)user;EnterCriticalSection(&A.lock);int ok=A.generation==A.run_generation&&content_model_add(&A.model,r,relative);LeaveCriticalSection(&A.lock);return ok&&WaitForSingleObject(A.cancel,0)!=WAIT_OBJECT_0;}
static DWORD WINAPI worker(void *unused){(void)unused;DWORD error=content_scan(A.root,&A.run_options,A.run_wsl,A.request.as_root,A.cancel,collect,NULL);if(error&&error!=ERROR_CANCELLED){EnterCriticalSection(&A.lock);if(A.generation==A.run_generation&&A.model.summary.state==CF_RUNNING&&!A.model.summary.errors){A.model.summary.state=CF_PARTIAL;A.model.summary.errors++;A.model.summary.error=error;}LeaveCriticalSection(&A.lock);}return 0;}
static void open_pending(void){if(!A.pending_root||A.thread)return;free(A.root);A.root=A.pending_root;A.pending_root=NULL;A.request=A.pending_request;content_model_free(&A.model);A.model.summary.state=CF_STOPPED;A.selected=UINT32_MAX;A.top=0;A.started=0;A.stale=0;A.message[0]=0;report();}
static void begin(void){if(!A.root){wcscpy(A.message,L"Choose a folder first");return;}cf_options options={.flags=A.flags};for(uint32_t i=0;i<CF_QUERY;i++){options.query[i]=(uint16_t)A.query[i];options.pattern[i]=(uint16_t)A.pattern[i];}if(!content_options_valid(&options)){wcscpy(A.message,L"Enter a nonempty, single-line query");return;}
 if(A.thread){stop();A.pending_start=1;return;}open_pending();A.generation++;A.run_generation=A.generation;A.run_options=options;A.stale=0;A.pending_start=0;A.selected=UINT32_MAX;A.top=0;A.message[0]=0;content_model_free(&A.model);ResetEvent(A.cancel);A.started=GetTickCount64();A.run_wsl=A.request.wsl;A.thread=CreateThread(NULL,0,worker,NULL,0,NULL);if(!A.thread){A.model.summary.state=CF_PARTIAL;A.model.summary.errors=1;A.model.summary.error=GetLastError();}report();}
static void queue_root(const wchar_t *root,const content_open *request){if((A.root&&A.request.session==request->session&&!wcscmp(A.root,root))||(A.pending_root&&A.pending_request.session==request->session&&!wcscmp(A.pending_root,root)))return;wchar_t *copy=_wcsdup(root);if(!copy)return;edited();free(A.pending_root);A.pending_root=copy;A.pending_request=*request;open_pending();}
static void result_action(uint32_t action){if(A.stale||A.pending_root)return;EnterCriticalSection(&A.lock);uint32_t selected=A.selected;if(selected>=A.model.count){LeaveCriticalSection(&A.lock);return;}cf_result r=A.model.rows[selected];content_action h={.session=A.request.session,.action=action,.search_generation=A.generation,.bytes=r.bytes,.mtime=r.mtime,.hit=r.hit};size_t n=(wcslen(r.path)+1)*2;unsigned char *p=malloc(sizeof h+n);if(p){memcpy(p,&h,sizeof h);memcpy(p+sizeof h,r.path,n);}LeaveCriticalSection(&A.lock);if(p){send(PANELP_CONTENT_ACTION,p,(uint32_t)(sizeof h+n));free(p);}}
static void activate(int id){A.focus=id;if(id>=3&&id<=5){A.flags^=id==3?CF_CASE:id==4?CF_WORD:CF_RECURSE;edited();}else if(id==6){if(A.thread&&!A.stale)stop();else begin();}else if(id==10)result_action(CONTENT_PREVIEW);else if(id==11)result_action(CONTENT_LEFT);paint();}
static wchar_t *field(void){return A.focus==1?A.query:A.focus==2?A.pattern:NULL;}
static void insert(wchar_t c){wchar_t *p=field();if(!p||c<32)return;int f=A.focus-1;size_t n=wcslen(p);if(A.all[f]){p[0]=0;n=0;A.caret[f]=0;A.all[f]=0;}if(n>=CF_QUERY-1)return;int at=A.caret[f];if(at<0||at>(int)n)at=(int)n;memmove(p+at+1,p+at,(n-(size_t)at+1)*2);p[at]=c;A.caret[f]=at+1;edited();}
static void paste(void){if(!field())return;if(!OpenClipboard(A.hwnd)){wcscpy(A.message,L"Clipboard busy — try paste again");return;}HANDLE h=GetClipboardData(CF_UNICODETEXT);const wchar_t *s=h?GlobalLock(h):NULL;if(s){size_t units=GlobalSize(h)/2;for(size_t i=0;i<units&&i<CF_QUERY&&s[i]&&s[i]>=32;i++)insert(s[i]);GlobalUnlock(h);}CloseClipboard();}
static void input(const panelp_input *in){
 if(in->msg==WM_LBUTTONDOWN||in->msg==WM_RBUTTONDOWN||in->msg==WM_LBUTTONDBLCLK){if(in->y>=A.list.top&&in->y<A.list.bottom){int row=A.top+(in->y-A.list.top)/S(66);EnterCriticalSection(&A.lock);int valid=row>=0&&row<(int)A.model.count;if(valid)A.selected=(uint32_t)row;LeaveCriticalSection(&A.lock);A.focus=0;if(valid&&in->msg==WM_LBUTTONDBLCLK)result_action(CONTENT_PREVIEW);else if(valid&&in->msg==WM_RBUTTONDOWN)result_action(CONTENT_MENU);}else if(in->msg==WM_LBUTTONDOWN)for(int i=0;i<A.nhot;i++)if(PtInRect(&A.hot[i].r,(POINT){in->x,in->y})){activate(A.hot[i].action);return;}}
 else if(in->msg==WM_MOUSEWHEEL)A.top-=(short)HIWORD(in->wparam)/WHEEL_DELTA*3;
 else if(in->msg==WM_CHAR){if(in->wparam>=32)insert((wchar_t)in->wparam);}
 else if(in->msg==WM_KEYDOWN){UINT k=in->wparam;wchar_t *p=field();int f=A.focus-1;
  if(k==VK_TAB){int ids[]={1,2,3,4,5,6,0,10,11},at=0,n=9;for(int i=0;i<n;i++)if(ids[i]==A.focus)at=i;A.focus=ids[(at+(in->mods&PANELP_M_SHIFT?n-1:1))%n];}
  else if(k==VK_RETURN){if(A.focus==0)result_action(CONTENT_PREVIEW);else if(A.focus==1||A.focus==2)begin();else activate(A.focus);}
  else if(k==VK_SPACE&&A.focus>=3)activate(A.focus);
  else if(k==VK_ESCAPE){if(A.thread)stop();else send(PANELP_CLOSE,NULL,0);}
  else if(p&&k=='A'&&in->mods&PANELP_M_CTRL)A.all[f]=1;
  else if(p&&k=='V'&&in->mods&PANELP_M_CTRL)paste();
  else if(p&&(k==VK_LEFT||k==VK_RIGHT||k==VK_HOME||k==VK_END)){int n=(int)wcslen(p);A.caret[f]=k==VK_HOME?0:k==VK_END?n:A.caret[f]+(k==VK_LEFT?-1:1);if(A.caret[f]<0)A.caret[f]=0;if(A.caret[f]>n)A.caret[f]=n;A.all[f]=0;}
  else if(p&&(k==VK_BACK||k==VK_DELETE)){int n=(int)wcslen(p),at=A.caret[f];if(A.all[f]){p[0]=0;A.caret[f]=0;A.all[f]=0;edited();}else if(k==VK_BACK&&at>0){memmove(p+at-1,p+at,(size_t)(n-at+1)*2);A.caret[f]--;edited();}else if(k==VK_DELETE&&at<n){memmove(p+at,p+at+1,(size_t)(n-at)*2);edited();}}
  else if(k==VK_APPS||(k==VK_F10&&in->mods&PANELP_M_SHIFT))result_action(CONTENT_MENU);
  else if(!p&&k=='C'&&in->mods&PANELP_M_CTRL)result_action(CONTENT_COPY);
  else if(A.focus==0&&(k==VK_UP||k==VK_DOWN||k==VK_HOME||k==VK_END||k==VK_PRIOR||k==VK_NEXT)){EnterCriticalSection(&A.lock);int count=(int)A.model.count,at=A.selected==UINT32_MAX?0:(int)A.selected;at=k==VK_HOME?0:k==VK_END?count-1:at+(k==VK_UP?-1:k==VK_DOWN?1:(k==VK_PRIOR?-8:8));if(at<0)at=0;if(at>=count)at=count-1;if(at>=0){A.selected=(uint32_t)at;int visible=(A.list.bottom-A.list.top)/S(66);if(at<A.top)A.top=at;if(at>=A.top+visible)A.top=at-visible+1;}LeaveCriticalSection(&A.lock);}
 }else return;paint();}
static void tick(void){int was_running=A.thread!=NULL;if(A.thread&&WaitForSingleObject(A.thread,0)==WAIT_OBJECT_0){CloseHandle(A.thread);A.thread=NULL;A.ended=GetTickCount64();open_pending();if(A.pending_start)begin();}report();if(was_running||A.thread)paint();}
static void on_host(msg *m){switch(m->h.type){
 case PANELP_CONTENT_WSL_POLICY:if(m->h.size==4){uint32_t enabled;memcpy(&enabled,m->data,4);if(enabled<=1){A.request.wsl=enabled;A.pending_request.wsl=enabled;}}break;
 case PANELP_PING:if(m->h.size==4)send(PANELP_PONG,m->data,4);break;
 case PANELP_FRAME_ACK:A.waiting=0;if(A.pending)present();break;
 case PANELP_INPUT:if(m->h.size==sizeof(panelp_input))input((panelp_input*)m->data);break;
 case PANELP_FOCUS:if(m->h.size==4)A.focused=*(uint32_t*)m->data!=0;paint();break;
 case PANELP_SHOW:if(m->h.size==4)A.shown=*(uint32_t*)m->data!=0;if(A.shown)paint();break;
 case PANELP_CONTENT_OPEN:if(m->h.size>=sizeof(content_open)+2&&(m->h.size-sizeof(content_open))%2==0){content_open r;memcpy(&r,m->data,sizeof r);wchar_t *path=(wchar_t*)(m->data+sizeof r);size_t n=(m->h.size-sizeof r)/2;if(n<=32768&&path[n-1]==0&&wcslen(path)==n-1&&r.session)queue_root(path,&r);}paint();break;
 case PANELP_RESIZE:if(m->h.size==sizeof(panelp_size)){panelp_size s;memcpy(&s,m->data,sizeof s);if(s.dpi&&s.dpi!=(uint32_t)A.dpi){A.dpi=s.dpi;fonts();}A.seq=s.seq;if(map((HANDLE)(uintptr_t)s.section,s.w,s.h))paint();}break;
 case PANELP_QUIT:PostQuitMessage(0);break;}}
static LRESULT CALLBACK hidden(HWND h,UINT m,WPARAM w,LPARAM l){if(m==WM_TIMER){tick();return 0;}return DefWindowProcW(h,m,w,l);}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,PWSTR command,int show){setlocale(LC_CTYPE,".UTF8");A.in=GetStdHandle(STD_INPUT_HANDLE);A.out=GetStdHandle(STD_OUTPUT_HANDLE);A.shown=1;A.focus=1;A.selected=UINT32_MAX;A.flags=CF_RECURSE;wcscpy(A.pattern,L"*");A.caret[1]=1;InitializeCriticalSection(&A.lock);A.cancel=CreateEventW(NULL,TRUE,FALSE,NULL);if(!A.cancel)return 1;
 panelp_head h;panelp_hello hi;if(!io(A.in,&h,sizeof h,0)||h.type!=PANELP_HELLO||h.size!=sizeof hi||!io(A.in,&hi,sizeof hi,0)||hi.magic!=PANELP_MAGIC||hi.version!=PANELP_VERSION)return 1;
 SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);A.dpi=hi.dpi?hi.dpi:96;A.seq=hi.seq;fonts();WNDCLASSW wc={0};wc.lpfnWndProc=hidden;wc.hInstance=instance;wc.lpszClassName=L"FpContentHidden";RegisterClassW(&wc);A.hwnd=CreateWindowExW(WS_EX_TOOLWINDOW,wc.lpszClassName,L"Find in files",WS_POPUP,0,0,0,0,NULL,NULL,instance,NULL);if(!A.hwnd||!map((HANDLE)(uintptr_t)hi.section,hi.w,hi.h))return 1;
 HANDLE t=CreateThread(NULL,65536,reader,A.hwnd,STACK_SIZE_PARAM_IS_A_RESERVATION,NULL);if(!t)return 1;CloseHandle(t);panelp_ready ready={GetCurrentProcessId(),0,L"Find in files"};send(PANELP_READY,&ready,sizeof ready);SetTimer(A.hwnd,1,200,NULL);paint();MSG m;while(GetMessageW(&m,NULL,0,0)>0){if(m.message==WM_HOST){on_host((msg*)m.lParam);free((void*)m.lParam);}else if(m.message==WM_GONE)break;else DispatchMessageW(&m);}KillTimer(A.hwnd,1);stop();if(A.thread&&WaitForSingleObject(A.thread,3000)!=WAIT_OBJECT_0)return 0;if(A.thread)CloseHandle(A.thread);content_model_free(&A.model);free(A.root);free(A.pending_root);CloseHandle(A.cancel);return 0;
}
