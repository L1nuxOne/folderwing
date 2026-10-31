/* The explorer only paints copied pixels and sends bounded commands. Media APIs,
 * source reads, codec loading and audio playback belong to folderwing-media.exe. */
#include "mediaview.h"
#include "mediautil.h"
#include "previewlaunch.h"
#include "previewreader.h"
#include "pipewriter.h"
#include "app.h"
#include "theme.h"
#include <windowsx.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#define WM_MEDIA_MSG (WM_APP+140)
#define WM_MEDIA_GONE (WM_APP+141)
#define WM_MEDIA_LAUNCHED (WM_APP+142)
typedef struct media_message {mediap_head h;unsigned char payload[];} media_message;
static struct {
 HWND hwnd;HANDLE process,job,section;preview_launch *launcher;preview_reader *reader;pipe_writer *writer;
 const unsigned char *shared;DWORD pid;uint32_t generation,doc,ticket;wchar_t *path;uint64_t bytes,last_message;int64_t mtime;
 mediap_state info;HBITMAP bitmap;void *pixels;uint32_t fw,fh;int cw,ch,drag,focus;double drag_value,volume;int muted;
 HDC paint_dc;HBITMAP paint_bitmap;HGDIOBJ paint_old;int paint_w,paint_h;
 RECT buttons[6],seek,volume_bar;
} R;
static int S(int n){return app_px(n);}
static int control_columns(void){return R.cw>=S(390)?4:R.cw>=S(240)?3:2;}
static int controls_top(void){return R.ch-S(40+28*((4+control_columns()-1)/control_columns())+28);}
static int read_all(HANDLE pipe,void *p,DWORD n){while(n){DWORD got=0;if(!ReadFile(pipe,p,n,&got,NULL)||!got)return 0;p=(char*)p+got;n-=got;}return 1;}
static DWORD WINAPI reader(void *arg){preview_reader *r=arg;for(;;){mediap_head h;if(!read_all(r->pipe,&h,sizeof h)||!((h.type==MEDIAP_STATE&&h.size==sizeof(mediap_state))||(h.type==MEDIAP_FRAME&&h.size==sizeof(mediap_frame))))break;media_message *m=malloc(sizeof *m+h.size);if(!m)break;m->h=h;if(!read_all(r->pipe,m->payload,h.size)){free(m);break;}if(InterlockedCompareExchange(&r->cancelled,0,0)||!PostMessageW(r->window,WM_MEDIA_MSG,r->generation,(LPARAM)m))free(m);}if(!InterlockedCompareExchange(&r->cancelled,0,0))PostMessageW(r->window,WM_MEDIA_GONE,r->generation,0);preview_reader_finish(r);return 0;}
static int send(uint32_t type,uint32_t ticket,const void *payload,uint32_t bytes){if(!R.writer)return -1;unsigned char buffer[sizeof(mediap_head)+sizeof(mediap_control)];size_t n=sizeof(mediap_head)+bytes;unsigned char *p=n<=sizeof buffer?buffer:malloc(n);if(!p)return -1;mediap_head h={type,bytes,R.doc,ticket};memcpy(p,&h,sizeof h);if(bytes)memcpy(p+sizeof h,payload,bytes);uint32_t tag=0;if(type==MEDIAP_CONTROL){uint32_t a=((const mediap_control*)payload)->action;tag=a==MEDIA_PLAY||a==MEDIA_PAUSE?1u:1u<<a;}int result=pipe_writer_send(R.writer,p,n,tag,tag);if(p!=buffer)free(p);if(result)PostMessageW(R.hwnd,WM_MEDIA_GONE,R.generation,0);return result;}
static void helper_stop(void){++R.generation;preview_launch_cancel(&R.launcher);pipe_writer_cancel(&R.writer);preview_reader_cancel(&R.reader);if(R.job)TerminateJobObject(R.job,1);else if(R.process)TerminateProcess(R.process,1);if(R.process)CloseHandle(R.process);if(R.job)CloseHandle(R.job);if(R.shared)UnmapViewOfFile(R.shared);if(R.section)CloseHandle(R.section);R.process=R.job=R.section=NULL;R.shared=NULL;R.pid=0;if(R.hwnd){MSG m;while(PeekMessageW(&m,R.hwnd,WM_MEDIA_MSG,WM_MEDIA_MSG,PM_REMOVE))free((void*)m.lParam);}}
static int helper_start(void){SECURITY_ATTRIBUTES sa={sizeof sa,NULL,TRUE};HANDLE ci=NULL,co=NULL,to=NULL,from=NULL;R.section=CreateFileMappingW(INVALID_HANDLE_VALUE,&sa,PAGE_READWRITE,0,MEDIAP_SECTION_BYTES,NULL);R.shared=R.section?MapViewOfFile(R.section,FILE_MAP_READ,0,0,MEDIAP_SECTION_BYTES):NULL;int ok=R.shared&&CreatePipe(&ci,&to,&sa,1<<20)&&CreatePipe(&from,&co,&sa,1<<20);if(ok){SetHandleInformation(to,HANDLE_FLAG_INHERIT,0);SetHandleInformation(from,HANDLE_FLAG_INHERIT,0);uint32_t generation=++R.generation;R.writer=pipe_writer_begin(to,1<<20,R.hwnd,WM_MEDIA_GONE,generation);R.reader=preview_reader_begin(from,R.hwnd,generation,reader);if(R.writer&&R.reader)R.launcher=preview_launch_begin(L"folderwing-media.exe",ci,co,R.section,(SIZE_T)1536<<20,R.hwnd,WM_MEDIA_LAUNCHED,generation);ok=R.launcher!=NULL;}if(ci)CloseHandle(ci);if(co)CloseHandle(co);if(to)CloseHandle(to);if(from)CloseHandle(from);if(!ok)helper_stop();return ok;}
static void title(void){wchar_t text[512];const wchar_t *name=R.path?wcsrchr(R.path,L'\\'):NULL;name=name?name+1:R.path?R.path:L"Media preview";const wchar_t *status=R.info.state==MEDIAP_READY?(R.info.paused?L"Paused":L"Playing"):R.info.state==MEDIAP_FAILED?L"Unavailable":L"Loading";swprintf(text,512,L"%ls | %ls",name,status);SetWindowTextW(R.hwnd,text);}
static void fail(uint32_t error){helper_stop();R.info.state=MEDIAP_FAILED;R.info.error=error;R.info.paused=1;title();InvalidateRect(R.hwnd,NULL,FALSE);}
static int state_valid(const mediap_state *s){return s->state>=MEDIAP_LOADING&&s->state<=MEDIAP_FAILED&&s->backend>=1&&s->backend<=2&&s->has_video<=1&&s->has_audio<=1&&s->paused<=1&&s->muted<=1&&s->ended<=1&&s->audio_unavailable<=1&&s->width<100000&&s->height<100000&&isfinite(s->duration)&&s->duration>=0&&s->duration<1e12&&isfinite(s->position)&&s->position>=0&&s->position<1e12&&isfinite(s->volume)&&s->volume>=0&&s->volume<=1.001;}
static void receive(media_message *m){if(m->h.doc!=R.doc||!R.path)return;R.last_message=GetTickCount64();if(m->h.type==MEDIAP_STATE){mediap_state s;memcpy(&s,m->payload,sizeof s);if(!state_valid(&s)){fail(ERROR_INVALID_DATA);return;}if(m->h.ticket<R.ticket&&s.state!=MEDIAP_FAILED)return;R.info=s;if(s.state==MEDIAP_FAILED){helper_stop();}title();InvalidateRect(R.hwnd,NULL,FALSE);}
 else{mediap_frame f;memcpy(&f,m->payload,sizeof f);if(!media_frame_valid(&f)||!R.shared){fail(ERROR_INVALID_DATA);return;}if(!R.bitmap||R.fw!=f.width||R.fh!=f.height){if(R.bitmap)DeleteObject(R.bitmap);BITMAPINFO bi={.bmiHeader={sizeof(BITMAPINFOHEADER),(LONG)f.width,-(LONG)f.height,1,32,BI_RGB}};R.bitmap=CreateDIBSection(NULL,&bi,DIB_RGB_COLORS,&R.pixels,NULL,0);R.fw=f.width;R.fh=f.height;}
  if(!R.bitmap){fail(ERROR_OUTOFMEMORY);return;}MemoryBarrier();memcpy(R.pixels,R.shared+(size_t)f.bank*MEDIAP_BANK_BYTES,(size_t)f.width*f.height*4);send(MEDIAP_ACK,m->h.ticket,NULL,0);RECT dirty={0,S(65),R.cw,controls_top()-S(35)};InvalidateRect(R.hwnd,&dirty,FALSE);}}
int mediaview_available(void){wchar_t file[MAX_PATH+32];DWORD n=GetModuleFileNameW(NULL,file,MAX_PATH);wchar_t *slash=n&&n<MAX_PATH?wcsrchr(file,L'\\'):NULL;if(!slash)return 0;wcscpy(slash+1,L"folderwing-media.exe");return GetFileAttributesW(file)!=INVALID_FILE_ATTRIBUTES;}
HWND mediaview_window(void){return R.hwnd;}
DWORD mediaview_helper_pid(void){return R.pid;}
int mediaview_state(mediap_state *s){if(s)*s=R.info;return R.path!=NULL;}
void mediaview_configure(unsigned volume,int muted){R.volume=(volume>100?100:volume)/100.;R.muted=muted!=0;if(R.path){mediaview_control(MEDIA_VOLUME,R.volume);mediaview_control(MEDIA_MUTE,R.muted);}}
void mediaview_clear(void){helper_stop();++R.doc;R.ticket=0;free(R.path);R.path=NULL;memset(&R.info,0,sizeof R.info);R.info.paused=1;if(R.bitmap)DeleteObject(R.bitmap);R.bitmap=NULL;R.pixels=NULL;R.fw=R.fh=0;R.drag=0;if(R.hwnd){if(GetCapture()==R.hwnd)ReleaseCapture();KillTimer(R.hwnd,1);title();InvalidateRect(R.hwnd,NULL,FALSE);}}
void mediaview_open(const wchar_t *path,uint64_t size,int64_t mtime){if(!path||!R.hwnd)return;if(R.path&&!wcscmp(R.path,path)&&R.bytes==size&&R.mtime==mtime&&R.info.state!=MEDIAP_FAILED)return;wchar_t *copy=_wcsdup(path);if(!copy)return;mediaview_clear();R.path=copy;R.bytes=size;R.mtime=mtime;R.last_message=GetTickCount64();R.info.state=MEDIAP_LOADING;R.info.backend=1;R.info.paused=1;title();if(!media_path_valid(path)||!helper_start()){fail(ERROR_NOT_SUPPORTED);return;}uint32_t n=(uint32_t)(wcslen(path)+1)*2;unsigned char *payload=malloc(sizeof(mediap_open)+n);if(!payload){fail(ERROR_OUTOFMEMORY);return;}mediap_open o={640,360,(media_audio_extension(path)?MEDIAP_AUDIO:0)|(R.muted?MEDIAP_MUTED:0),0};memcpy(payload,&o,sizeof o);memcpy(payload+sizeof o,path,n);send(MEDIAP_OPEN,0,payload,sizeof o+n);free(payload);mediaview_control(MEDIA_VOLUME,R.volume);SetTimer(R.hwnd,1,250,NULL);InvalidateRect(R.hwnd,NULL,FALSE);}
void mediaview_control(uint32_t action,double value){if(!R.path||!R.writer||!isfinite(value)||action<MEDIA_PLAY||action>MEDIA_MUTE)return;if(action==MEDIA_VOLUME){value=fmax(0,fmin(1,value));R.volume=value;R.info.volume=value;}if(action==MEDIA_MUTE){R.muted=value!=0;R.info.muted=R.muted;}if(action==MEDIA_PLAY)R.info.paused=0;if(action==MEDIA_PAUSE)R.info.paused=1;if(action==MEDIA_SEEK){value=fmax(0,fmin(value,R.info.duration>0?fmax(0,R.info.duration-.01):value));}mediap_control c={action,0,value};send(MEDIAP_CONTROL,++R.ticket,&c,sizeof c);title();InvalidateRect(R.hwnd,NULL,FALSE);}
void mediaview_suspend(void){if(R.path&&!R.info.paused)mediaview_control(MEDIA_PAUSE,0);}
static void fill(HDC dc,RECT r,COLORREF color){HBRUSH b=CreateSolidBrush(color);FillRect(dc,&r,b);DeleteObject(b);}
static void text(HDC dc,const wchar_t *value,RECT r,COLORREF color,UINT flags){SetTextColor(dc,color);DrawTextW(dc,value,-1,&r,flags|DT_NOPREFIX);}
static void geometry(void){
 int y=controls_top(),pad=S(12),columns=control_columns(),gap=S(4);if(y<S(85))y=S(85);
 int width=(R.cw-2*pad-gap*(columns-1))/columns;if(width<1)width=1;
 for(int i=0;i<4;i++){int x=pad+(i%columns)*(width+gap),top=y+(i/columns)*S(28);R.buttons[i]=(RECT){x,top,x+width,top+S(26)};}
 int top=y+((4+columns-1)/columns)*S(28),button=(R.cw-2*pad-2*gap)/3;if(button>S(50))button=S(50);if(button<1)button=1;
 R.buttons[4]=(RECT){pad,top,pad+button,top+S(26)};
 R.buttons[5]=(RECT){R.cw-pad-button,top,R.cw-pad,top+S(26)};
 R.volume_bar=(RECT){R.buttons[4].right+gap,top,R.buttons[5].left-gap,top+S(26)};
 R.seek=(RECT){pad,y-S(23),R.cw-pad,y-S(9)};
}
static void volume_track(int *left,int *right){int inset=S(6);*left=R.volume_bar.left+inset;*right=R.volume_bar.right-inset;if(*right<=*left){*left=R.volume_bar.left;*right=R.volume_bar.right;}}
static void volume_point(int x){int left,right;volume_track(&left,&right);if(right>left)mediaview_control(MEDIA_VOLUME,fmax(0,fmin(1,(double)(x-left)/(right-left))));}
static void volume_step(int direction){double scaled=R.volume*10.;double next=direction>0?floor(scaled+1e-6)+1:ceil(scaled-1e-6)-1;mediaview_control(MEDIA_VOLUME,next/10.);}
static void clock_text(wchar_t *out,size_t cap,double value){uint64_t seconds=isfinite(value)&&value>0?(uint64_t)value:0;swprintf(out,cap,L"%02llu:%02llu",(unsigned long long)(seconds/60),(unsigned long long)(seconds%60));}
static void paint(HDC dc){RECT all={0,0,R.cw,R.ch};fill(dc,all,C_BG);SetBkMode(dc,TRANSPARENT);HGDIOBJ font=SelectObject(dc,app_font());geometry();RECT r={S(12),S(8),R.cw-S(12),S(32)};const wchar_t *name=R.path?wcsrchr(R.path,L'\\'):NULL;text(dc,name?name+1:R.path?R.path:L"Media preview",r,C_TEXT,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);wchar_t line[512];r.top=S(34);r.bottom=S(58);if(R.info.state==MEDIAP_READY){if(R.info.has_video)swprintf(line,512,L"Video  ·  %u × %u%ls",R.info.width,R.info.height,R.info.has_audio?L"  ·  Audio":L"");else wcscpy(line,L"Audio");if(R.info.audio_unavailable)wcscat(line,L"  ·  Audio output unavailable");}else if(R.info.state==MEDIAP_FAILED)wcscpy(line,(R.info.error==0xc00d36fau||R.info.error==0xfffffff2u)?L"Audio output is unavailable. Wake or reconnect the output, then Retry.":L"This media could not be previewed. Check the file, then Retry.");else wcscpy(line,L"Loading media…");text(dc,line,r,R.info.state==MEDIAP_FAILED?C_ERROR:C_DIM,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
 RECT area={S(12),S(65),R.cw-S(12),R.seek.top-S(12)};if(area.bottom>area.top&&area.right>area.left){fill(dc,area,RGB(0,0,0));if(R.bitmap&&R.fw&&R.fh){double scale=fmin((double)(area.right-area.left)/R.fw,(double)(area.bottom-area.top)/R.fh);int w=(int)(R.fw*scale),h=(int)(R.fh*scale),x=area.left+(area.right-area.left-w)/2,y=area.top+(area.bottom-area.top-h)/2;BITMAPINFO bi={.bmiHeader={sizeof(BITMAPINFOHEADER),(LONG)R.fw,-(LONG)R.fh,1,32,BI_RGB}};SetStretchBltMode(dc,HALFTONE);StretchDIBits(dc,x,y,w,h,0,0,R.fw,R.fh,R.pixels,&bi,DIB_RGB_COLORS,SRCCOPY);}else{text(dc,R.info.state==MEDIAP_READY?L"Audio preview — press Play":R.info.state==MEDIAP_FAILED?L"Preview unavailable":L"Preparing preview…",area,C_DIM,DT_SINGLELINE|DT_CENTER|DT_VCENTER);}}
 fill(dc,R.seek,C_EDIT);double ratio=R.drag==1?R.drag_value:(R.info.duration>0?R.info.position/R.info.duration:0);RECT progress=R.seek;progress.right=progress.left+(int)((progress.right-progress.left)*fmax(0,fmin(1,ratio)));fill(dc,progress,C_ACCENT);const wchar_t *labels[]={R.info.state==MEDIAP_FAILED?L"Retry":R.info.paused?L"Play":L"Pause",L"−5 s",L"+5 s",R.info.muted?L"Unmute":L"Mute",L"−",L"+"};for(int i=0;i<6;i++){int focus=i==5?6:i;fill(dc,R.buttons[i],R.focus==focus?C_SEL:C_EDIT);text(dc,labels[i],R.buttons[i],R.info.state==MEDIAP_READY||i==0?C_TEXT:C_DIM,DT_SINGLELINE|DT_CENTER|DT_VCENTER);}
 if(R.volume_bar.right>R.volume_bar.left){if(R.focus==5)fill(dc,R.volume_bar,C_SEL);int left,right;volume_track(&left,&right);if(right>left){int middle=(R.volume_bar.top+R.volume_bar.bottom)/2,position=left+(int)((right-left)*R.volume+.5);RECT track={left,middle-S(2),right,middle+S(2)},active=track;fill(dc,track,C_BORDER);active.right=position;fill(dc,active,R.info.state==MEDIAP_READY?C_ACCENT:C_DIM);RECT knob={position-S(4),middle-S(7),position+S(4),middle+S(7)};fill(dc,knob,R.info.state==MEDIAP_READY?C_TEXT:C_DIM);}}
 wchar_t a[64],b[64];clock_text(a,64,R.drag==1?R.drag_value*R.info.duration:R.info.position);clock_text(b,64,R.info.duration);swprintf(line,512,L"%ls / %ls",a,b);r=(RECT){S(12),R.buttons[5].bottom+S(5),R.cw-S(66),R.ch};text(dc,line,r,C_DIM,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);swprintf(line,512,L"%u%%",(unsigned)(R.volume*100+.5));r.left=R.cw-S(66);r.right=R.cw-S(12);text(dc,line,r,C_DIM,DT_SINGLELINE|DT_VCENTER|DT_RIGHT|DT_END_ELLIPSIS);SelectObject(dc,font);}
/* Compose the whole pane before presenting it: state updates also repaint the
 * buttons while paused, so painting each layer to the window visibly flashes. */
static int prepare_paint_buffer(HDC dc){
 if(!R.paint_dc){R.paint_dc=CreateCompatibleDC(dc);if(!R.paint_dc)return 0;}
 if(!R.paint_bitmap||R.paint_w!=R.cw||R.paint_h!=R.ch){
  HBITMAP bitmap=CreateCompatibleBitmap(dc,R.cw>0?R.cw:1,R.ch>0?R.ch:1);if(!bitmap)return 0;
  HGDIOBJ previous=SelectObject(R.paint_dc,bitmap);if(!previous||previous==HGDI_ERROR){DeleteObject(bitmap);return 0;}
  if(R.paint_bitmap)DeleteObject(R.paint_bitmap);else R.paint_old=previous;
  R.paint_bitmap=bitmap;R.paint_w=R.cw;R.paint_h=R.ch;
 }
 return 1;
}
static void release_paint_buffer(void){if(R.paint_dc){if(R.paint_bitmap){SelectObject(R.paint_dc,R.paint_old);DeleteObject(R.paint_bitmap);}DeleteDC(R.paint_dc);}R.paint_dc=NULL;R.paint_bitmap=NULL;R.paint_old=NULL;R.paint_w=R.paint_h=0;}
static void activate(int id){if(id==0){if(R.info.state==MEDIAP_FAILED){wchar_t *path=R.path?_wcsdup(R.path):NULL;if(path){mediaview_open(path,R.bytes,R.mtime);free(path);}}else if(R.info.state==MEDIAP_READY){if(R.info.ended)mediaview_control(MEDIA_SEEK,0);mediaview_control(R.info.paused?MEDIA_PLAY:MEDIA_PAUSE,0);}}else if(R.info.state==MEDIAP_READY){if(id==1||id==2)mediaview_control(MEDIA_SEEK,R.info.position+(id==1?-5:5));if(id==3)mediaview_control(MEDIA_MUTE,!R.muted);if(id==4||id==5)volume_step(id==4?-1:1);}}
static LRESULT CALLBACK window_proc(HWND hwnd,UINT message,WPARAM w,LPARAM l){switch(message){case WM_SIZE:R.cw=LOWORD(l);R.ch=HIWORD(l);geometry();InvalidateRect(hwnd,NULL,FALSE);return 0;case WM_ERASEBKGND:return 1;case WM_PRINTCLIENT:paint((HDC)w);return 0;case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);if(prepare_paint_buffer(dc)){paint(R.paint_dc);BitBlt(dc,ps.rcPaint.left,ps.rcPaint.top,ps.rcPaint.right-ps.rcPaint.left,ps.rcPaint.bottom-ps.rcPaint.top,R.paint_dc,ps.rcPaint.left,ps.rcPaint.top,SRCCOPY);}else paint(dc);EndPaint(hwnd,&ps);return 0;}case WM_DESTROY:release_paint_buffer();return 0;case WM_SHOWWINDOW:if(!w)mediaview_suspend();return 0;case WM_TIMER:if(R.path&&R.info.state!=MEDIAP_FAILED&&GetTickCount64()-R.last_message>(R.info.state==MEDIAP_READY?5000:20000))fail(ERROR_TIMEOUT);return 0;
 case WM_MEDIA_LAUNCHED:if((uint32_t)w==R.generation&&preview_launch_take(&R.launcher,&R.process,&R.job,&R.pid)<0)fail(ERROR_ACCESS_DENIED);return 0;
 case WM_MEDIA_MSG:{media_message *m=(void*)l;if((uint32_t)w==R.generation)receive(m);free(m);return 0;}
 case WM_MEDIA_GONE:if((uint32_t)w==R.generation&&R.path)fail(ERROR_BROKEN_PIPE);return 0;
 case WM_LBUTTONDOWN:{SetFocus(hwnd);POINT p={GET_X_LPARAM(l),GET_Y_LPARAM(l)};if(PtInRect(&R.seek,p)&&R.info.state==MEDIAP_READY&&R.info.duration>0&&R.seek.right>R.seek.left){R.drag=1;R.drag_value=fmax(0,fmin(1,(double)(p.x-R.seek.left)/(R.seek.right-R.seek.left)));SetCapture(hwnd);InvalidateRect(hwnd,NULL,FALSE);return 0;}if(PtInRect(&R.volume_bar,p)&&R.info.state==MEDIAP_READY){R.focus=5;R.drag=2;SetCapture(hwnd);volume_point(p.x);return 0;}for(int i=0;i<6;i++)if(PtInRect(&R.buttons[i],p)){R.focus=i==5?6:i;activate(i);InvalidateRect(hwnd,NULL,FALSE);break;}return 0;}
 case WM_MOUSEMOVE:if(R.drag==1){int width=R.seek.right-R.seek.left;R.drag_value=width>0?fmax(0,fmin(1,(double)(GET_X_LPARAM(l)-R.seek.left)/width)):0;InvalidateRect(hwnd,NULL,FALSE);}else if(R.drag==2)volume_point(GET_X_LPARAM(l));return 0;
 case WM_LBUTTONUP:if(R.drag){int drag=R.drag;R.drag=0;if(GetCapture()==hwnd)ReleaseCapture();if(drag==1)mediaview_control(MEDIA_SEEK,R.drag_value*R.info.duration);else volume_point(GET_X_LPARAM(l));}return 0;
 case WM_CAPTURECHANGED:R.drag=0;return 0;
 case WM_KEYDOWN:if(GetKeyState(VK_CONTROL)>=0&&GetKeyState(VK_MENU)>=0){if(w==VK_ESCAPE){app_preview_close();return 0;}if(w==VK_TAB){if(GetKeyState(VK_SHIFT)<0){if(R.focus>0)R.focus--;else app_focus_cycle(-1);}else if(R.focus<6)R.focus++;else app_focus_cycle(1);InvalidateRect(hwnd,NULL,FALSE);return 0;}if(w==VK_SPACE){activate(0);return 0;}if(w==VK_RETURN){if(R.focus!=5)activate(R.focus==6?5:R.focus);return 0;}if(w==VK_LEFT||w==VK_RIGHT){if(R.focus==5)activate(w==VK_LEFT?4:5);else activate(w==VK_LEFT?1:2);return 0;}if(w==VK_UP||w==VK_DOWN){activate(w==VK_DOWN?4:5);return 0;}if(w=='M'){activate(3);return 0;}if(w==VK_HOME){if(R.focus==5&&R.info.state==MEDIAP_READY)mediaview_control(MEDIA_VOLUME,0);else mediaview_control(MEDIA_SEEK,0);return 0;}if(w==VK_END&&R.focus==5&&R.info.state==MEDIAP_READY){mediaview_control(MEDIA_VOLUME,1);return 0;}}
 {fn_ctx c;app_preview_ctx(&c,hwnd,FN_FROM_KEY);uint8_t mods=(GetKeyState(VK_CONTROL)<0?FN_CTRL:0)|(GetKeyState(VK_SHIFT)<0?FN_SHIFT:0)|(GetKeyState(VK_MENU)<0?FN_ALT:0);fn_key((uint8_t)w,mods,&c);app_ctx_free(&c);}return 0;
 case WM_SETFOCUS:case WM_KILLFOCUS:InvalidateRect(hwnd,NULL,FALSE);return 0;case WM_CHAR:return 0;case WM_GETDLGCODE:return DLGC_WANTALLKEYS;
 }return DefWindowProcW(hwnd,message,w,l);}
int mediaview_start(HWND parent){if(R.hwnd)return 0;memset(&R,0,sizeof R);R.volume=.25;R.info.paused=1;WNDCLASSW c={0};c.lpfnWndProc=window_proc;c.hInstance=GetModuleHandleW(NULL);c.hCursor=LoadCursorW(NULL,IDC_ARROW);c.lpszClassName=L"OpenPilotMedia";RegisterClassW(&c);R.hwnd=CreateWindowExW(0,c.lpszClassName,L"Media preview",WS_CHILD|WS_CLIPSIBLINGS|WS_TABSTOP,0,0,640,480,parent,NULL,c.hInstance,NULL);return R.hwnd?0:-1;}
void mediaview_stop(void){mediaview_clear();if(R.hwnd)DestroyWindow(R.hwnd);R.hwnd=NULL;}
