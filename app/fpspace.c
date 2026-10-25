/* On-demand Space panel. One worker owns traversal; the UI owns presentation. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>
#include "space.h"
#include "termutil.h"
#include "spaceproto.h"
#include "ntfscache.h"
#include "panelproto.h"
#include "theme.h"
#define WM_HOST (WM_APP+1)
#define WM_GONE (WM_APP+2)
#define WM_QUERY_DONE (WM_APP+3)
typedef struct msg {panelp_head h;uint8_t data[];} msg;
typedef struct hot {RECT r;int action;uint32_t id;} hot;
typedef struct analysis_snapshot {volatile LONG refs;sp_model model;wchar_t *root;uint64_t epoch;uint32_t scope;} analysis_snapshot;
typedef struct query_job {HANDLE thread,cancel;HWND hwnd;uint64_t epoch,ticket,revision;uint32_t scope,count;int view,allocated,sort;wchar_t filter[128],extension[48];sp_row *rows;DWORD error;sp_model details;wchar_t *root;int summary,wsl,as_root;uint64_t device,identity;analysis_snapshot *snapshot;} query_job;
static struct {
 HANDLE in,out,section,thread,cancel,visible;HWND hwnd;HDC dc;HBITMAP bitmap,original;HFONT font,bold,small;uint8_t *shared;void *pixels;
 int w,h,dpi,next,waiting,pending,shown,focused;uint32_t seq;CRITICAL_SECTION lock;
 sp_model model,details;wchar_t *details_root;analysis_snapshot *snapshot;wchar_t *root,*pending_root;space_open request,pending_request;uint32_t run_wsl;
 query_job *query;uint64_t model_epoch,query_ticket;int query_pending;DWORD query_error;
 sp_row *rows;uint32_t count,scope,selected,rows_scope;int rows_view,rows_allocated;wchar_t rows_extension[48];unsigned char *expanded;int view,allocated,sort,top,focus;
 wchar_t filter[128],extension[48];hot hot[48];int nhot;RECT list,filter_rect;
 uint64_t started,ended,refreshed;int dirty,reset_pending,repaint,report_valid;space_state last_report;
} A;
#include "spacebrowser.h"
#include "spacefocus.h"
static void snapshot_release(analysis_snapshot *s){if(s&&InterlockedDecrement(&s->refs)==0){space_model_free(&s->model);free(s->root);free(s);}}
static void details_drop(void){snapshot_release(A.snapshot);A.snapshot=NULL;memset(&A.details,0,sizeof A.details);A.details_root=NULL;}
/* Detail enumeration has its own completion state and counters. A successful
 * transport can publish useful SP_PARTIAL/error=0 rows; their immutable snapshot
 * keeps that qualification across every sort/filter/type query. */
static int analysis_status(wchar_t *out,size_t cap){
 if(!A.rows_view||!A.snapshot||A.snapshot->model.state==SP_COMPLETE)return 0;
 const sp_model *m=&A.snapshot->model;sp_totals sum={0};if(m->count)sum=m->nodes[0].sum;
 swprintf(out,cap,L"File analysis incomplete · %llu errors · %llu excluded%ls",(unsigned long long)sum.errors,(unsigned long long)sum.skipped,A.query_error?L" · update unavailable":A.query||A.query_pending?L" · updating…":L"");return 1;
}
static int S(int n){return MulDiv(n,A.dpi,96);}
static int io(HANDLE h,void *p,DWORD n,int output){while(n){DWORD done;BOOL ok=output?WriteFile(h,p,n,&done,NULL):ReadFile(h,p,n,&done,NULL);if(!ok||!done)return 0;p=(char*)p+done;n-=done;}return 1;}
static void send(uint32_t type,const void *data,uint32_t n){panelp_head h={type,n};if(!io(A.out,&h,sizeof h,1)||(n&&!io(A.out,(void*)data,n,1)))PostQuitMessage(1);}
static DWORD WINAPI reader(void *arg){for(;;){panelp_head h;if(!io(A.in,&h,sizeof h,0)||h.size>PANELP_MAX)break;msg *m=malloc(sizeof *m+h.size);if(!m)break;m->h=h;if(!io(A.in,m->data,h.size,0)){free(m);break;}if(!PostMessageW(arg,WM_HOST,0,(LPARAM)m)){free(m);break;}}PostMessageW(arg,WM_GONE,0,0);return 0;}
static void fonts(void){if(A.font)DeleteObject(A.font);if(A.bold)DeleteObject(A.bold);if(A.small)DeleteObject(A.small);A.font=CreateFontW(-MulDiv(90,A.dpi,720),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");A.bold=CreateFontW(-MulDiv(95,A.dpi,720),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");A.small=CreateFontW(-MulDiv(80,A.dpi,720),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");}
static int map(HANDLE section,int w,int h){if(w<1||h<1||w>16384||h>16384)return 0;if(A.shared)UnmapViewOfFile(A.shared);if(A.section)CloseHandle(A.section);if(A.bitmap){SelectObject(A.dc,A.original);DeleteObject(A.bitmap);}A.section=section;A.shared=MapViewOfFile(section,FILE_MAP_WRITE,0,0,0);A.w=w;A.h=h;A.waiting=0;A.next=0;if(!A.dc)A.dc=CreateCompatibleDC(NULL);BITMAPINFO b={{sizeof b.bmiHeader,w,-h,1,32,BI_RGB}};A.bitmap=CreateDIBSection(NULL,&b,DIB_RGB_COLORS,&A.pixels,NULL,0);if(A.bitmap)A.original=SelectObject(A.dc,A.bitmap);return A.shared&&A.bitmap;}
static void fill(RECT r,COLORREF color){HBRUSH b=CreateSolidBrush(color);FillRect(A.dc,&r,b);DeleteObject(b);}
static void text(const wchar_t *s,RECT r,COLORREF color,HFONT font,UINT flags){HGDIOBJ old=SelectObject(A.dc,font);SetTextColor(A.dc,color);SetBkMode(A.dc,TRANSPARENT);DrawTextW(A.dc,s,-1,&r,flags|DT_NOPREFIX);SelectObject(A.dc,old);}
static void bytes(wchar_t *out,size_t cap,uint64_t n){double x=(double)n;const wchar_t *u=L"B";if(n>=(1ull<<40)){x/=1ull<<40;u=L"TiB";}else if(n>=(1ull<<30)){x/=1ull<<30;u=L"GiB";}else if(n>=(1ull<<20)){x/=1ull<<20;u=L"MiB";}else if(n>=1024){x/=1024;u=L"KiB";}swprintf(out,cap,n<1024?L"%.0f %ls":L"%.2f %ls",x,u);}
static void button(const wchar_t *s,int x,int y,int w,int action,uint32_t id,int chosen){RECT r={x,y,x+w,y+S(26)};fill(r,chosen?C_SEL:C_EDIT);if(A.focused&&A.focus==action){HBRUSH b=CreateSolidBrush(C_ACCENT);FrameRect(A.dc,&r,b);DeleteObject(b);}text(s,r,C_TEXT,A.font,DT_CENTER|DT_VCENTER|DT_SINGLELINE);if(A.nhot<48)A.hot[A.nhot++]=(hot){r,action,id};}
static void present(void){if(!A.shared||!A.pixels||!A.shown)return;if(A.waiting){A.pending=1;return;}GdiFlush();size_t n=(size_t)A.w*A.h*4;memcpy(A.shared+n*A.next,A.pixels,n);panelp_frame f={(uint32_t)A.next,A.seq,{0,0,A.w,A.h}};send(PANELP_FRAME,&f,sizeof f);A.next^=1;A.waiting=1;A.pending=0;}
static void paint(int rebuild_rows);
static void query_invalidate(void){A.query_ticket++;A.query_pending=0;A.query_error=0;if(A.query)SetEvent(A.query->cancel);}
static int analysis_receive(void *user,const sp_record *r,const wchar_t *name){query_job *job=user;if(r->type==SP_ENTRY&&!r->id&&(r->device!=job->device||r->identity!=job->identity)){job->error=ERROR_FILE_INVALID;return 0;}return WaitForSingleObject(job->cancel,0)!=WAIT_OBJECT_0&&space_model_record(&job->details,r,name);}
static DWORD WINAPI query_worker(void *arg){query_job *job=arg;sp_query *q=NULL;int done=0;
 if(job->summary&&!job->snapshot){DWORD error=space_scan_mode(job->root,job->wsl,job->as_root,job->cancel,analysis_receive,job,3);if(!job->error)job->error=error;if(!job->error)job->error=job->details.error;if(!job->error&&job->details.count)q=space_query_begin(&job->details,0,job->view,job->allocated,job->sort,job->filter,job->extension);if(q)job->revision=space_query_revision(q);else if(!job->error)job->error=ERROR_INVALID_DATA;}else if(job->summary){q=space_query_begin(&job->snapshot->model,0,job->view,job->allocated,job->sort,job->filter,job->extension);if(q)job->revision=space_query_revision(q);else job->error=GetLastError();}else{
 EnterCriticalSection(&A.lock);if(job->epoch==A.model_epoch)q=space_query_begin(&A.model,job->scope,job->view,job->allocated,job->sort,job->filter,job->extension);else job->error=ERROR_CANCELLED;if(q)job->revision=space_query_revision(q);else if(!job->error)job->error=GetLastError();LeaveCriticalSection(&A.lock);}
 while(q&&!job->error&&!done){if(WaitForSingleObject(job->cancel,0)==WAIT_OBJECT_0){job->error=ERROR_CANCELLED;break;}EnterCriticalSection(&A.lock);job->error=job->summary?space_query_capture(q,job->snapshot?&job->snapshot->model:&job->details,&done):job->epoch==A.model_epoch?space_query_capture(q,&A.model,&done):ERROR_CANCELLED;LeaveCriticalSection(&A.lock);if(!job->error)job->error=space_query_work(q,job->cancel);}
 if(q&&!job->error)job->rows=space_query_finish(q,job->cancel,&job->count,&job->error);space_query_free(q);if(job->summary&&job->rows)for(uint32_t i=0;i<job->count;i++)job->rows[i].id|=SPACE_DETAIL_BIT;PostMessageW(job->hwnd,WM_QUERY_DONE,0,0);return 0;
}
static void query_start(void){if(A.query||!A.query_pending||!A.shown||!A.view)return;A.query_pending=0;if(A.scope>=A.model.count){A.dirty=1;return;}
 query_job *job=calloc(1,sizeof *job);if(!job){A.query_error=ERROR_NOT_ENOUGH_MEMORY;A.repaint=1;return;}job->epoch=A.model_epoch;job->ticket=A.query_ticket;job->scope=A.scope;job->view=A.view;job->allocated=A.allocated;job->sort=A.sort;job->hwnd=A.hwnd;job->summary=A.model.backend==SP_BACKEND_SUMMARY;job->wsl=A.request.wsl;job->as_root=A.request.as_root;if(job->summary){job->root=space_model_path(&A.model,A.scope,A.root);job->device=A.model.stamps[A.scope].device;job->identity=A.model.stamps[A.scope].identity;}wcscpy(job->filter,A.filter);wcscpy(job->extension,A.extension);if(job->summary&&A.snapshot&&A.snapshot->epoch==job->epoch&&A.snapshot->scope==job->scope){job->snapshot=A.snapshot;InterlockedIncrement(&job->snapshot->refs);}job->cancel=CreateEventW(NULL,TRUE,FALSE,NULL);if(job->cancel&&(!job->summary||job->root))job->thread=CreateThread(NULL,0,query_worker,job,0,NULL);
 if(!job->thread){A.query_error=GetLastError();if(job->cancel)CloseHandle(job->cancel);snapshot_release(job->snapshot);free(job->root);free(job);A.repaint=1;return;}A.query=job;
}
static void query_request(void){query_invalidate();A.query_pending=1;A.refreshed=GetTickCount64();A.dirty=0;query_start();}
static void rows_replace(sp_row *rows,uint32_t count,int view,int allocated,const wchar_t *extension){
 int old=0;for(uint32_t i=0;A.selected!=SP_NONE&&i<A.count;i++)if(A.rows[i].id==A.selected){old=(int)i-A.top;break;}free(A.rows);A.rows=rows;A.count=count;A.rows_view=view;A.rows_allocated=allocated;A.rows_scope=A.scope;wcscpy(A.rows_extension,extension);
 int found=0;for(uint32_t i=0;A.selected!=SP_NONE&&i<A.count;i++)if(A.rows[i].id==A.selected){A.top=(int)i-old;found=1;break;}if(!found)A.selected=SP_NONE;
}
static int query_complete(void){EnterCriticalSection(&A.lock);query_job *job=A.query;if(!job||WaitForSingleObject(job->thread,0)!=WAIT_OBJECT_0){LeaveCriticalSection(&A.lock);return 0;}A.query=NULL;int current=job->ticket==A.query_ticket&&job->epoch==A.model_epoch&&job->view==A.view&&job->scope==A.scope;int applied=0;
 if(current&&!job->error&&job->summary&&!job->snapshot){job->snapshot=calloc(1,sizeof *job->snapshot);if(!job->snapshot)job->error=ERROR_NOT_ENOUGH_MEMORY;else{job->snapshot->refs=1;job->snapshot->model=job->details;memset(&job->details,0,sizeof job->details);job->snapshot->root=job->root;job->root=NULL;job->snapshot->epoch=job->epoch;job->snapshot->scope=job->scope;}}
 if(current&&!job->error&&(job->summary||job->revision==A.model.revision)){details_drop();if(job->snapshot){A.snapshot=job->snapshot;job->snapshot=NULL;A.details=A.snapshot->model;A.details_root=A.snapshot->root;}rows_replace(job->rows,job->count,job->view,job->allocated,job->extension);job->rows=NULL;A.dirty=0;A.query_error=0;A.refreshed=GetTickCount64();applied=1;}
 else if(current){if(job->error==ERROR_RETRY||job->error==ERROR_CANCELLED||!job->error){A.dirty=1;A.refreshed=0;}else{A.query_error=job->error;A.dirty=0;}A.repaint=1;}
 free(job->rows);snapshot_release(job->snapshot);space_model_free(&job->details);free(job->root);CloseHandle(job->thread);CloseHandle(job->cancel);free(job);query_start();LeaveCriticalSection(&A.lock);return applied;
}
static int query_shutdown(unsigned timeout){EnterCriticalSection(&A.lock);query_invalidate();query_job *job=A.query;LeaveCriticalSection(&A.lock);if(job&&WaitForSingleObject(job->thread,timeout)!=WAIT_OBJECT_0)return 0;query_complete();return 1;}
static void rebuild(void){
 if(A.reset_pending){query_invalidate();free(A.rows);A.rows=NULL;A.count=0;A.rows_view=0;A.scope=0;A.selected=SP_NONE;memset(A.expanded,0,SP_NTFS_LIMIT);A.reset_pending=0;}if(A.scope>=A.model.count||(A.model.count&&A.model.nodes[A.scope].flags&SP_REMOVED))A.scope=0;if(A.selected<A.model.count&&A.model.nodes[A.selected].flags&SP_REMOVED)A.selected=SP_NONE;
 if(A.view){query_request();return;}details_drop();query_invalidate();uint32_t count=0;sp_row *rows=space_model_rows(&A.model,A.scope,0,A.allocated,A.sort,A.filter,A.extension,A.expanded,&count);rows_replace(rows,count,0,A.allocated,L"");A.refreshed=GetTickCount64();A.dirty=0;
}
static void paint(int rebuild_rows){if(!A.bitmap||!A.shown)return;if(B.enabled){browser_paint(rebuild_rows);return;}EnterCriticalSection(&A.lock);if(rebuild_rows||A.reset_pending)rebuild();int view=A.rows_view,metric=A.rows_allocated;A.nhot=0;fill((RECT){0,0,A.w,A.h},C_BG);
 wchar_t line[1024],logical[48],allocated[48];sp_totals sum={0};if(A.scope<A.model.count)sum=A.model.nodes[A.scope].sum;bytes(logical,48,sum.logical);bytes(allocated,48,sum.allocated);
 text(A.root?A.root:L"Choose a folder to analyze",(RECT){S(10),S(6),A.w-S(10),S(27)},C_TEXT,A.bold,DT_SINGLELINE|DT_PATH_ELLIPSIS);
 const wchar_t *state=!A.root?L"Ready":A.thread?WaitForSingleObject(A.cancel,0)==WAIT_OBJECT_0?L"Stopping…":L"Scanning — so far":A.model.state==SP_COMPLETE?L"Complete":A.model.state==SP_STOPPED?L"Stopped — incomplete":L"Partial — incomplete";
 if(A.model.backend==SP_BACKEND_NTFS){switch(A.model.phase){case NTFS_LOADING:state=L"Loading NTFS cache…";break;case NTFS_CACHED:state=L"NTFS cache loaded — catching up…";break;case NTFS_CATCHUP:state=L"Updating NTFS results…";break;case NTFS_BASELINE:state=L"Building NTFS disk model…";break;case NTFS_LIVE:state=L"NTFS · live estimate";break;case NTFS_PAUSED:state=L"NTFS · paused estimate";break;default:state=L"NTFS · incomplete estimate";break;}}
 int incomplete_analysis=analysis_status(line,1024);if(!incomplete_analysis)swprintf(line,1024,L"%ls%ls%ls%ls",state,A.model.phase==NTFS_FALLBACK?L" · standard scan":L"",A.model.backend_error?L" · update unavailable":L"",A.view&&(A.query||A.query_pending||A.dirty)?L" · updating results…":A.query_error?L" · results unavailable":L"");text(line,(RECT){S(10),S(31),A.w-S(10),S(50)},incomplete_analysis||A.model.backend_error?C_ERROR:C_DIM,A.small,DT_SINGLELINE|DT_END_ELLIPSIS);
 swprintf(line,1024,L"Logical: %ls   ·   On disk: %ls%ls",logical,allocated,sum.unknown?L" + unknown":L"");text(line,(RECT){S(10),S(53),A.w-S(10),S(76)},C_TEXT,A.font,DT_SINGLELINE|DT_END_ELLIPSIS);
 swprintf(line,1024,L"%ls%llu files · %llu folders · %llu excluded · %llu errors",incomplete_analysis?L"Folder summary: ":L"",(unsigned long long)sum.files,(unsigned long long)sum.folders,(unsigned long long)sum.skipped,(unsigned long long)sum.errors);text(line,(RECT){S(10),S(79),A.w-S(10),S(99)},sum.errors?C_ERROR:C_DIM,A.small,DT_SINGLELINE|DT_END_ELLIPSIS);
 int gap=S(5),third=(A.w-S(20)-gap*2)/3;button(L"Folders",S(10),S(106),third,10,0,A.view==0);button(L"Largest files",S(10)+third+gap,S(106),third,11,0,A.view==1);button(L"File types",S(10)+2*(third+gap),S(106),third,12,0,A.view>=2);
 button(L"Logical",S(10),S(139),S(66),13,0,!A.allocated);button(L"On disk",S(81),S(139),S(66),14,0,A.allocated);button(A.model.backend==SP_BACKEND_NTFS?(A.thread?L"Pause":L"Refresh"):(A.thread?L"Stop":L"Rescan"),A.w-S(90),S(139),S(80),15,0,0);
 /* Compact breadcrumb: root and the last two ancestors remain clickable. */
 button(L"Root",S(10),S(172),S(44),20,0,A.scope==0);uint32_t chain[130],depth=0,at=A.scope;while(at<A.model.count&&at&&depth<130){chain[depth++]=at;at=A.model.nodes[at].parent;}
 int x=S(59);for(int i=(int)(depth>2?2:depth)-1;i>=0;i--){uint32_t id=chain[i];int w=(A.w-x-S(10))/(i+1);if(w>S(20)){button(A.model.nodes[id].name,x,S(172),w-S(4),21+i,id,id==A.scope);x+=w;}}
 A.filter_rect=(RECT){S(10),S(205),A.w-S(10),S(231)};fill(A.filter_rect,A.focus==9?C_SEL_IDLE:C_EDIT);swprintf(line,1024,L"Filter: %ls%ls",A.filter,A.focus==9?L"│":L"");text(line,A.filter_rect,*A.filter?C_TEXT:C_DIM,A.font,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);A.hot[A.nhot++]=(hot){A.filter_rect,9,0};
 int name_end=A.w-S(165);text(view==3?A.rows_extension:L"Name",(RECT){S(10),S(236),name_end,S(259)},C_DIM,A.font,DT_SINGLELINE|DT_VCENTER);
 button(A.sort==2?L"Name A–Z":A.sort==1?L"Size ↑":L"Size ↓",A.w-S(157),S(235),S(83),16,0,0);text(L"Share",(RECT){A.w-S(70),S(236),A.w-S(8),S(259)},C_DIM,A.small,DT_SINGLELINE|DT_RIGHT|DT_VCENTER);
 A.list=(RECT){0,S(265),A.w,A.h-S(100)};int row_h=S(view==1||view==3?42:29),visible=(A.list.bottom-A.list.top)/row_h;if(visible<1)visible=1;if(A.top>(int)A.count-visible)A.top=(int)A.count-visible;if(A.top<0)A.top=0;
 int saved=SaveDC(A.dc);IntersectClipRect(A.dc,0,A.list.top,A.w,A.list.bottom);uint64_t total=A.rows_scope<A.model.count?(metric?A.model.nodes[A.rows_scope].sum.allocated:A.model.nodes[A.rows_scope].sum.logical):0;
 for(uint32_t i=(uint32_t)A.top;i<A.count;i++){int y=A.list.top+((int)i-A.top)*row_h;if(y>=A.list.bottom)break;sp_row *r=&A.rows[i];sp_node *n=display_node(r->id);if(!n||n->flags&SP_REMOVED)continue;sp_totals *t=&n->sum;uint64_t size=view?r->size:metric?t->allocated:t->logical;
  if(r->id==A.selected)fill((RECT){0,y,A.w,y+row_h-1},A.focused?C_SEL:C_SEL_IDLE);
  double ratio=total?(double)size/total:0;if(ratio>1)ratio=1;fill((RECT){S(8),y+row_h-S(3),S(8)+(int)((A.w-S(16))*ratio),y+row_h-S(1)},C_CURSOR);
  int indent=view==0?(int)r->depth*S(12):0;if(indent>name_end/2)indent=name_end/2;
  swprintf(line,1024,L"%ls%ls",view==0&&(n->flags&SP_DIR)?A.expanded[r->id]?L"− ":L"+ ":L"",view==2?r->type:n->name);
  if(view==2||(view==0&&(n->flags&SP_DIR))){size_t used=wcslen(line);swprintf(line+used,1024-used,L" (%llu)",(unsigned long long)(view==2?r->files:n->sum.files));}
  text(line,(RECT){S(10)+indent,y,name_end,y+S(25)},n->flags&(SP_SKIP|SP_FAILED)?C_DIM:n->flags&SP_DIR?C_FOLDER:C_TEXT,A.font,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
  bytes(line,1024,size);if(metric&&view!=2&&t->unknown)wcscat(line,L" + ?");text(line,(RECT){name_end,y,A.w-S(73),y+S(25)},C_TEXT,A.font,DT_SINGLELINE|DT_VCENTER|DT_RIGHT);
  swprintf(line,1024,L"%.1f%%",ratio*100);text(line,(RECT){A.w-S(69),y,A.w-S(8),y+S(25)},C_DIM,A.small,DT_SINGLELINE|DT_VCENTER|DT_RIGHT);
  if(view==1||view==3){wchar_t *path=display_path(r->id);if(path){text(path,(RECT){S(10),y+S(23),A.w-S(8),y+S(39)},C_DIM,A.small,DT_SINGLELINE|DT_PATH_ELLIPSIS);free(path);}}
 }
 if(!A.count)text(A.query||A.query_pending?L"Updating results…":A.query_error?L"Results unavailable":A.thread?L"Collecting metadata…":L"No matching results",(RECT){S(12),A.list.top+S(16),A.w-S(12),A.list.top+S(40)},C_DIM,A.font,DT_SINGLELINE);RestoreDC(A.dc,saved);
 if(display_node(A.selected)){sp_node *n=display_node(A.selected);uint64_t files=n->sum.files;if(view==2)for(uint32_t i=0;i<A.count;i++)if(A.rows[i].id==A.selected){files=A.rows[i].files;break;}swprintf(line,1024,L"%llu files · %llu folders%ls%ls%ls",(unsigned long long)files,(unsigned long long)n->sum.folders,n->flags&SP_HARDLINK?L" · shared allocation":L"",n->sum.unknown?L" · allocation unknown":L"",n->sum.errors?L" · incomplete":L"");text(line,(RECT){S(10),A.h-S(95),A.w-S(10),A.h-S(76)},C_DIM,A.small,DT_SINGLELINE|DT_END_ELLIPSIS);}
 int bw=(A.w-S(30))/3;button(L"Reveal left",S(10),A.h-S(70),bw,30,0,0);button(L"Reveal right",S(15)+bw,A.h-S(70),bw,31,0,0);button(L"Preview",S(20)+bw*2,A.h-S(70),bw,32,0,0);
 text(L"Enter: open · ←/→: tree · Backspace: up · Right-click: actions",(RECT){S(10),A.h-S(35),A.w-S(10),A.h-S(10)},C_DIM,A.small,DT_SINGLELINE|DT_END_ELLIPSIS);
 A.repaint=0;LeaveCriticalSection(&A.lock);present();}
/* The ordinary scanner uses one-record delivery. NTFS batches at most 256
 * validated records and never holds the model lock while waiting for pipe IO. */
static int collect_locked(const sp_record *r,const wchar_t *name){if(r->type==SP_RESET){A.model_epoch++;query_invalidate();A.reset_pending=1;}
 uint32_t backend=A.model.backend,phase=A.model.phase,error=A.model.backend_error,state=A.model.state,model_error=A.model.error;
 int ok=space_model_record(&A.model,r,name);
 /* A newly complete model should appear on the next UI tick, even when a
  * partial view was just rebuilt. Unchanged live commits keep the throttle. */
 if(r->type==SP_END&&A.model.state==SP_COMPLETE&&state!=SP_COMPLETE)A.refreshed=0;
 if(r->type==SP_ENTRY||r->type==SP_UPDATE||r->type==SP_REMOVE||r->type==SP_LEAVE||r->type==SP_TOTAL||r->type==SP_RESET)A.dirty=1;
 if(backend!=A.model.backend||phase!=A.model.phase||error!=A.model.backend_error||state!=A.model.state||model_error!=A.model.error)A.repaint=1;
 return ok;}
static int collect(void *user,const sp_record *r,const wchar_t *name){(void)user;EnterCriticalSection(&A.lock);int ok=collect_locked(r,name);LeaveCriticalSection(&A.lock);return ok&&WaitForSingleObject(A.cancel,0)!=WAIT_OBJECT_0;}
static int collect_batch(void *user,const sp_batch_item *items,uint32_t count){(void)user;if(WaitForSingleObject(A.cancel,0)==WAIT_OBJECT_0)return 0;int ok=1;EnterCriticalSection(&A.lock);
 for(uint32_t i=0;i<count&&ok;i++){const sp_record *r=&items[i].record;ok=collect_locked(r,r->type==SP_ENTRY||r->type==SP_UPDATE?items[i].name:NULL);}
 LeaveCriticalSection(&A.lock);return ok&&WaitForSingleObject(A.cancel,0)!=WAIT_OBJECT_0;}
static DWORD WINAPI worker(void *unused){(void)unused;
 if(space_ntfs_root(A.root)){
  DWORD error=space_ntfs_scan_batched(A.root,A.cancel,A.visible,collect_batch,NULL);int cancelled=WaitForSingleObject(A.cancel,0)==WAIT_OBJECT_0;
  EnterCriticalSection(&A.lock);int have=A.model.backend==SP_BACKEND_NTFS&&A.model.count;LeaveCriticalSection(&A.lock);
  if(cancelled||have){collect(NULL,&(sp_record){.magic=SP_MAGIC,.type=SP_STATUS,.identity=SP_BACKEND_NTFS,.flags=cancelled?NTFS_PAUSED:NTFS_INCOMPLETE,.error=cancelled?0:error},NULL);collect(NULL,&(sp_record){.magic=SP_MAGIC,.type=SP_END,.flags=cancelled?SP_STOPPED:SP_PARTIAL,.error=cancelled?0:error},NULL);return 0;}
  collect(NULL,&(sp_record){.magic=SP_MAGIC,.type=SP_RESET},NULL);collect(NULL,&(sp_record){.magic=SP_MAGIC,.type=SP_STATUS,.flags=NTFS_FALLBACK,.error=error},NULL);
 }
 wchar_t distro[128],path[32768];int summary=B.enabled&&term_wsl_split(A.root,distro,128,path,32768);space_scan_mode(A.root,A.run_wsl,A.request.as_root,A.cancel,collect,NULL,summary?1:0);return 0;
}
static void stop(void){if(A.cancel)SetEvent(A.cancel);if(A.thread)CancelSynchronousIo(A.thread);}
static void start_pending(void){if(!A.pending_root||A.thread)return;EnterCriticalSection(&A.lock);query_invalidate();focus_reset();A.model_epoch++;details_drop();free(A.rows);A.rows=NULL;A.count=0;A.rows_view=0;space_model_free(&A.model);free(A.root);A.root=A.pending_root;A.pending_root=NULL;A.request=A.pending_request;A.scope=0;A.selected=SP_NONE;A.reset_pending=0;A.report_valid=0;A.top=0;A.view=0;A.filter[0]=0;A.extension[0]=0;memset(A.expanded,0,SP_NTFS_LIMIT);ResetEvent(A.cancel);A.started=GetTickCount64();A.ended=A.started;A.refreshed=0;A.run_wsl=A.request.wsl;A.thread=CreateThread(NULL,0,worker,NULL,0,NULL);if(!A.thread){A.model.state=SP_PARTIAL;A.model.error=GetLastError();}LeaveCriticalSection(&A.lock);paint(1);}
static void queue_root(const wchar_t *root,const space_open *request){wchar_t *copy=_wcsdup(root);if(!copy)return;free(A.pending_root);A.pending_root=copy;A.pending_request=*request;if(A.thread)stop();else start_pending();}
/* Main-view commands follow the focused list, whose retained highlight is the
 * authority. The legacy selected ID can lag focus-only keyboard transitions.
 * Tree/tools and empty or filtered-out selections have no file-action target. */
static uint32_t action_selection(void){
 if(!B.enabled)return A.selected;
 if(A.focus||(B.focus!=1&&B.focus!=2))return SP_NONE;
 uint32_t selected=B.focus==1?B.left_selected:B.right_selected;
 const sp_row *rows=B.focus==1?A.rows:B.right;uint32_t count=B.focus==1?A.count:B.right_count;
 if(selected!=SP_NONE)for(uint32_t i=0;i<count;i++)if(rows[i].id==selected)return selected;
 return SP_NONE;
}
static void action(uint32_t action){uint32_t selected=action_selection();if(selected==SP_NONE)return;if(A.rows_view==2){if(display_node(selected))for(uint32_t i=0;i<A.count;i++)if(A.rows[i].id==selected){wcscpy(A.extension,A.rows[i].type);A.view=3;A.selected=SP_NONE;A.top=0;paint(1);return;}return;}
 EnterCriticalSection(&A.lock);uint32_t id=selected;sp_node *node=display_node(id);wchar_t *path=node?display_path(id):NULL;uint32_t flags=node?node->flags:0;LeaveCriticalSection(&A.lock);if(!path)return;
 if(flags&(SP_SKIP|SP_FAILED|SP_REMOVED)){free(path);return;}space_action h={A.request.session,action,flags&SP_DIR};size_t bytes=(wcslen(path)+1)*2;uint8_t *data=malloc(sizeof h+bytes);if(data){memcpy(data,&h,sizeof h);memcpy(data+sizeof h,path,bytes);send(PANELP_SPACE_ACTION,data,(uint32_t)(sizeof h+bytes));free(data);}free(path);}
static void scope(uint32_t id){EnterCriticalSection(&A.lock);if(id<A.model.count&&(A.model.nodes[id].flags&SP_DIR)){A.scope=id;A.top=0;A.selected=SP_NONE;if(A.view==3)A.view=2;}LeaveCriticalSection(&A.lock);paint(1);}
static void activate(int button_id,uint32_t id){A.focus=button_id;
 if(button_id>=10&&button_id<=12){A.view=button_id-10;A.top=0;A.selected=SP_NONE;A.extension[0]=0;paint(1);return;}
 if(button_id==13||button_id==14){A.allocated=button_id==14;paint(1);return;}
 if(button_id==15){if(A.thread)stop();else if(A.root)queue_root(A.root,&A.request);paint(0);return;}
 if(button_id==16){A.sort=(A.sort+1)%3;paint(1);return;}
 if(button_id>=20&&button_id<=22){scope(id);return;}
 if(button_id>=30&&button_id<=32){action(button_id-30);return;}paint(0);}
static int selected_row(void){if(A.selected==SP_NONE)return -1;for(uint32_t i=0;i<A.count;i++)if(A.rows[i].id==A.selected)return (int)i;return -1;}
static void open_selected(void){if(A.rows_view==2){action(SPACE_PREVIEW);return;}EnterCriticalSection(&A.lock);uint32_t id=A.selected;int dir=id<A.model.count&&(A.model.nodes[id].flags&SP_DIR);LeaveCriticalSection(&A.lock);if(dir)scope(id);else action(SPACE_PREVIEW);}
#include "spacefocus.c"
#include "spacebrowser.c"
static void input_locked(const panelp_input *in){int rh=S(A.rows_view==1||A.rows_view==3?42:29);
 if(in->msg==WM_LBUTTONDOWN||in->msg==WM_RBUTTONDOWN||in->msg==WM_LBUTTONDBLCLK){
  if(in->y>=A.list.top&&in->y<A.list.bottom){int row=A.top+(in->y-A.list.top)/rh;if(row>=0&&row<(int)A.count){A.selected=A.rows[row].id;A.focus=0;if(in->msg==WM_RBUTTONDOWN)action(SPACE_MENU);else if(in->msg==WM_LBUTTONDBLCLK)open_selected();else if(A.rows_view==0&&A.view==0&&in->x<S(26)+(int)A.rows[row].depth*S(12)){A.expanded[A.selected]^=1;paint(1);return;}}}
  else if(in->msg==WM_LBUTTONDOWN)for(int i=0;i<A.nhot;i++)if(PtInRect(&A.hot[i].r,(POINT){in->x,in->y})){activate(A.hot[i].action,A.hot[i].id);return;}
 }else if(in->msg==WM_MOUSEWHEEL)A.top-=(short)HIWORD(in->wparam)/WHEEL_DELTA*3;
 else if(in->msg==WM_CHAR&&A.focus==9){wchar_t ch=(wchar_t)in->wparam;size_t n=wcslen(A.filter);if(ch==8){if(n)A.filter[n-1]=0;}else if(ch>=32&&n<127){A.filter[n]=ch;A.filter[n+1]=0;}A.top=0;paint(1);return;}
 else if(in->msg==WM_KEYDOWN){UINT k=in->wparam;
  if(k==VK_TAB){int ids[]={0,10,11,12,13,14,15,20,9,16,30,31,32};size_t n=sizeof ids/sizeof *ids;int at=0;for(size_t i=0;i<n;i++)if(A.focus==ids[i])at=(int)i;A.focus=ids[(at+(in->mods&PANELP_M_SHIFT?(int)n-1:1))%(int)n];}
  else if(k==VK_ESCAPE){if(*A.filter){A.filter[0]=0;paint(1);return;}send(PANELP_CLOSE,NULL,0);}
  else if(k==VK_BACK&&A.focus!=9){if(A.view==3){A.view=2;A.selected=SP_NONE;paint(1);return;}EnterCriticalSection(&A.lock);uint32_t parent=A.scope<A.model.count?A.model.nodes[A.scope].parent:SP_NONE;LeaveCriticalSection(&A.lock);if(parent!=SP_NONE)scope(parent);return;}
  else if(k==VK_RETURN||k==VK_SPACE){if(A.focus){for(int i=0;i<A.nhot;i++)if(A.hot[i].action==A.focus){activate(A.focus,A.hot[i].id);return;}}else open_selected();}
  else if(k==VK_APPS||(k==VK_F10&&in->mods&PANELP_M_SHIFT))action(SPACE_MENU);
  else if(k=='C'&&in->mods&PANELP_M_CTRL)action(SPACE_COPY);
  else if(A.focus!=9&&(k==VK_LEFT||k==VK_RIGHT)&&A.rows_view==0&&A.view==0&&A.selected!=SP_NONE){A.expanded[A.selected]=k==VK_RIGHT;paint(1);return;}
  else if(A.focus!=9&&(k==VK_UP||k==VK_DOWN||k==VK_HOME||k==VK_END||k==VK_PRIOR||k==VK_NEXT)){
   int at=selected_row();at=k==VK_HOME?0:k==VK_END?(int)A.count-1:at+(k==VK_UP?-1:k==VK_DOWN?1:(k==VK_PRIOR?-1:1)*8);if(at<0)at=0;if(at>=(int)A.count)at=(int)A.count-1;if(at>=0){A.selected=A.rows[at].id;int visible=(A.list.bottom-A.list.top)/rh;if(at<A.top)A.top=at;if(at>=A.top+visible)A.top=at-visible+1;}}
 }else return;paint(0);}
static void input(const panelp_input *in){EnterCriticalSection(&A.lock);if(A.reset_pending)paint(1);else if(B.enabled)browser_input(in);else input_locked(in);LeaveCriticalSection(&A.lock);}
static void tick(void){int catalog_changed=browser_tick();int query_applied=query_complete();int finished=A.thread&&WaitForSingleObject(A.thread,0)==WAIT_OBJECT_0;if(finished){A.ended=GetTickCount64();CloseHandle(A.thread);A.thread=NULL;if(A.pending_root){start_pending();return;}}
 EnterCriticalSection(&A.lock);int focus_changed=focus_tick();uint32_t count=A.model.count,state=A.model.state;int refresh=A.dirty&&!A.query&&!A.query_pending&&(finished||GetTickCount64()-A.refreshed>=1000);int redraw=focus_changed||catalog_changed||query_applied||refresh||finished||A.repaint;sp_totals sum={0};if(count)sum=A.model.nodes[0].sum;DWORD error=A.model.error?A.model.error:A.model.backend_error;space_state report={A.request.session,A.thread&&A.model.backend!=SP_BACKEND_NTFS?SP_RUNNING:state,error,sum.files,sum.folders};LeaveCriticalSection(&A.lock);
 if(redraw)paint(refresh);if(!A.report_valid||memcmp(&report,&A.last_report,sizeof report)){send(PANELP_SPACE_STATE,&report,sizeof report);A.last_report=report;A.report_valid=1;}}
static void on_host(msg *m){switch(m->h.type){
 case PANELP_PING:if(m->h.size==4)send(PANELP_PONG,m->data,4);break;
 case PANELP_FRAME_ACK:A.waiting=0;if(A.pending)present();break;
 case PANELP_INPUT:if(m->h.size==sizeof(panelp_input))input((panelp_input*)m->data);break;
 case PANELP_FOCUS:if(m->h.size==4)A.focused=*(uint32_t*)m->data!=0;paint(0);break;
 case PANELP_SHOW:if(m->h.size==4){int was=A.shown;A.shown=*(uint32_t*)m->data!=0;if(B.enabled&&A.shown&&!was)browser_refresh_catalog();}if(!A.shown){EnterCriticalSection(&A.lock);if(A.query||A.query_pending){query_invalidate();A.dirty=1;}LeaveCriticalSection(&A.lock);}if(A.visible){if(A.shown)SetEvent(A.visible);else ResetEvent(A.visible);}if(A.shown)paint(A.dirty);break;
 case PANELP_SPACE_BROWSER:if(m->h.size==sizeof(space_open)){space_open config;memcpy(&config,m->data,sizeof config);if(config.session&&config.wsl<=1){A.request.wsl=A.pending_request.wsl=config.wsl;B.enabled=1;browser_config(config.wsl!=0);paint(1);}}break;
 case PANELP_SPACE_OPEN:if(m->h.size>=sizeof(space_open)+2&&(m->h.size-sizeof(space_open))%2==0){space_open r;memcpy(&r,m->data,sizeof r);wchar_t *path=(wchar_t*)(m->data+sizeof r);size_t chars=(m->h.size-sizeof r)/2;if(chars<=32768&&path[chars-1]==0&&wcslen(path)==chars-1&&r.session&&!(A.root&&r.session==A.request.session&&!wcscmp(path,A.root))&&!(A.pending_root&&r.session==A.pending_request.session&&!wcscmp(path,A.pending_root)))queue_root(path,&r);}break;
 case PANELP_RESIZE:if(m->h.size==sizeof(panelp_size)){panelp_size s;memcpy(&s,m->data,sizeof s);if(s.dpi&&s.dpi!=(uint32_t)A.dpi){A.dpi=s.dpi;fonts();}A.seq=s.seq;if(map((HANDLE)(uintptr_t)s.section,s.w,s.h))paint(0);}break;
 case PANELP_QUIT:PostQuitMessage(0);break;}}
static LRESULT CALLBACK hidden(HWND h,UINT m,WPARAM w,LPARAM l){if(m==WM_TIMER){tick();return 0;}if(m==WM_QUERY_DONE){if(query_complete())paint(0);return 0;}return DefWindowProcW(h,m,w,l);}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,PWSTR command,int show){
 A.in=GetStdHandle(STD_INPUT_HANDLE);A.out=GetStdHandle(STD_OUTPUT_HANDLE);A.shown=1;A.selected=SP_NONE;A.allocated=1;InitializeCriticalSection(&A.lock);A.cancel=CreateEventW(NULL,TRUE,FALSE,NULL);A.visible=CreateEventW(NULL,TRUE,TRUE,NULL);A.expanded=calloc(SP_NTFS_LIMIT,1);if(!A.cancel||!A.visible||!A.expanded)return 1;
 panelp_head h;panelp_hello hi;if(!io(A.in,&h,sizeof h,0)||h.type!=PANELP_HELLO||h.size!=sizeof hi||!io(A.in,&hi,sizeof hi,0)||hi.magic!=PANELP_MAGIC||hi.version!=PANELP_VERSION)return 1;
 SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);A.dpi=hi.dpi?hi.dpi:96;A.seq=hi.seq;fonts();WNDCLASSW wc={0};wc.lpfnWndProc=hidden;wc.hInstance=instance;wc.lpszClassName=L"FpSpaceHidden";RegisterClassW(&wc);A.hwnd=CreateWindowExW(WS_EX_TOOLWINDOW,wc.lpszClassName,L"Space",WS_POPUP,0,0,0,0,NULL,NULL,instance,NULL);
 if(!A.hwnd||!map((HANDLE)(uintptr_t)hi.section,hi.w,hi.h))return 1;HANDLE t=CreateThread(NULL,65536,reader,A.hwnd,STACK_SIZE_PARAM_IS_A_RESERVATION,NULL);if(!t)return 1;CloseHandle(t);
 panelp_ready ready={GetCurrentProcessId(),0,L"Space"};send(PANELP_READY,&ready,sizeof ready);SetTimer(A.hwnd,1,200,NULL);paint(0);
 MSG m;while(GetMessageW(&m,NULL,0,0)>0){if(m.message==WM_HOST){on_host((msg*)m.lParam);free((void*)m.lParam);}else if(m.message==WM_GONE)break;else DispatchMessageW(&m);}KillTimer(A.hwnd,1);stop();if(A.thread&&WaitForSingleObject(A.thread,3000)!=WAIT_OBJECT_0)return 0;
 if(!focus_shutdown())return 0;if(!browser_shutdown())return 0;if(!query_shutdown(3000))return 0;if(A.thread)CloseHandle(A.thread);space_model_free(&A.model);details_drop();free(A.root);free(A.pending_root);free(A.rows);free(A.expanded);CloseHandle(A.cancel);if(A.visible)CloseHandle(A.visible);return 0;
}
