#include "../src/namecodec.h"
/* Cold modeless current-level comparison. One worker, one pending update. */
#include "foldercompare.h"
#include "app.h"
#include "theme.h"
#include "renameutil.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <stdlib.h>
#include <stdio.h>
enum { ID_LEFT=101,ID_RIGHT,ID_HASH,ID_SCAN,ID_STOP,ID_ROWS,ID_STATUS,ID_DETAIL,ID_COPY_RIGHT,ID_COPY_LEFT,ID_SUBFOLDERS };
#define CM_BUILD (WM_APP+1)
#define CM_UPDATE (WM_APP+2)
#define CM_SHOW (WM_APP+3)
typedef struct compare_window {
    HWND hwnd;HANDLE thread;HBRUSH bg;const fn_def *owner;wchar_t *path[2];
    CRITICAL_SECTION lock;LONG refs;volatile LONG stop;int busy,closing,queued,finished,root,hashes,build,recursive;
    uint64_t notified;fpe_comparison result;
} compare_window;
static HWND window;
static const wchar_t *labels[]={L"",L"Only left",L"Only right",L"Metadata matches (contents unchecked)",L"Size differs",L"Date differs (contents unchecked)",
    L"Contents differ (SHA-256)",L"Contents match (SHA-256)",L"Folder — contents not compared",L"File / folder conflict",L"Link or special item — excluded",L"Ambiguous name — excluded",L"Error — unverified"};
static void release(compare_window *s){if(!InterlockedDecrement(&s->refs)){fpe_compare_free(&s->result);DeleteCriticalSection(&s->lock);DeleteObject(s->bg);free(s->path[0]);free(s->path[1]);free(s);}}
static int pulse(void *user)
{
    compare_window *s=user;uint64_t now=GetTickCount64();
    if(now-s->notified>=100){s->notified=now;EnterCriticalSection(&s->lock);if(s->hwnd&&!s->queued)s->queued=PostMessageW(s->hwnd,CM_UPDATE,0,0)!=0;LeaveCriticalSection(&s->lock);}
    return !InterlockedCompareExchange(&s->stop,0,0);
}
static DWORD WINAPI worker(void *user)
{
    compare_window *s=user;fpe_comparison result;(s->recursive?fpe_compare_tree:fpe_compare_folders)(s->path[0],s->path[1],s->hashes,s->root,pulse,s,&result);
    EnterCriticalSection(&s->lock);if(InterlockedCompareExchange(&s->stop,0,0))result.cancelled=1;s->result=result;s->finished=1;if(s->hwnd&&!s->queued)s->queued=PostMessageW(s->hwnd,CM_UPDATE,0,0)!=0;LeaveCriticalSection(&s->lock);
    release(s);return 0;
}
static const fpe_entry *entry(compare_window *s,const fpe_compare_row *r,int side)
{int i=side?r->right:r->left;return i>=0?fpe_dir_entries(s->result.dirs[side])+i:NULL;}
static const wchar_t *name(compare_window *s,const fpe_compare_row *r,int side)
{const fpe_entry *e=entry(s,r,side);return e?fpe_dir_name(s->result.dirs[side],e):L"";}
/* Relative paths can exceed one filename; escape controls without the rename
 * editor's 255-character component limit. */
static void display_path(const wchar_t *name,int linux,wchar_t *out,size_t cap)
{
    if(linux){fpe_lx_display(name,out,cap,0);return;}
    size_t at=0;for(size_t i=0;name[i]&&at+1<cap;i++){
        wchar_t c=name[i];if(linux&&c>=0xf001&&c<0xf080&&(c<0xf020||wcschr(L"<>:\"\\|?*",c-0xf000)))c-=0xf000;
        if(c>=0xd800&&c<=0xdbff&&name[i+1]>=0xdc00&&name[i+1]<=0xdfff){if(at+2>=cap)break;out[at++]=c;out[at++]=name[++i];continue;}
        if(c<32||c==127||(c>=0xdc80&&c<=0xdcff)){if(at+6>=cap)break;swprintf(out+at,cap-at,L"\\u%04x",c);at+=6;}
        else out[at++]=c;
    }out[at]=0;
}
static void metadata(const fpe_entry *e,wchar_t *out,size_t cap)
{
    if(!e){swprintf(out,cap,L"absent");return;}
    FILETIME ft={(DWORD)e->mtime,(DWORD)((uint64_t)e->mtime>>32)};SYSTEMTIME t;
    if(FileTimeToSystemTime(&ft,&t))swprintf(out,cap,L"%llu bytes; %04u-%02u-%02u %02u:%02u:%02u.%03u UTC",(unsigned long long)e->size,t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
    else swprintf(out,cap,L"%llu bytes; date unavailable",(unsigned long long)e->size);
}
static const wchar_t *label(compare_window *s,uint32_t state)
{return state==FPE_COMPARE_FOLDER&&s->recursive?L"Folder group — see descendants":labels[state];}
static void detail(compare_window *s)
{
    if(s->busy)return;int i=ListView_GetNextItem(GetDlgItem(s->hwnd,ID_ROWS),-1,LVNI_SELECTED);if(i<0||(uint32_t)i>=s->result.count){SetDlgItemTextW(s->hwnd,ID_DETAIL,s->recursive?L"Select differing files by relative path. Operations reviews one copy job; missing parent folders are created after Start. Folder rows group descendants; empty folders are not copied.":L"Select files to stage a copy. Operations will ask you to choose conflicts and Start. Check Include subfolders and Compare again to scan the tree.");return;}
    fpe_compare_row *r=s->result.rows+i;wchar_t a[1600],b[1600],text[4400],reason[320]=L"",ma[180],mb[180];
    display_path(name(s,r,0),fpe_dir_is_linux(s->result.dirs[0]),a,1600);display_path(name(s,r,1),fpe_dir_is_linux(s->result.dirs[1]),b,1600);
    if(r->error)FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,NULL,r->error,0,reason,320,NULL);
    metadata(entry(s,r,0),ma,180);metadata(entry(s,r,1),mb,180);
    swprintf(text,4400,L"Left: %ls     Right: %ls\r\n%ls%ls%ls%ls\r\nLeft: %ls     Right: %ls",a[0]?a:L"(absent)",b[0]?b:L"(absent)",label(s,r->state),r->error?L" — ":L"",reason,s->result.cancelled?L" — scan cancelled; run Compare again before copying":L"",ma,mb);
    SetDlgItemTextW(s->hwnd,ID_DETAIL,text);
    SetPropW(s->hwnd,L"FolderCompare.SelectedState",(HANDLE)(UINT_PTR)r->state);
}
static void enable(compare_window *s,int complete)
{
    EnableWindow(GetDlgItem(s->hwnd,ID_SCAN),!s->busy);EnableWindow(GetDlgItem(s->hwnd,ID_HASH),!s->busy);EnableWindow(GetDlgItem(s->hwnd,ID_SUBFOLDERS),!s->busy);
    EnableWindow(GetDlgItem(s->hwnd,ID_STOP),s->busy);EnableWindow(GetDlgItem(s->hwnd,ID_COPY_LEFT),complete);EnableWindow(GetDlgItem(s->hwnd,ID_COPY_RIGHT),complete);
}
static void show(compare_window *s)
{
    fpe_comparison *r=&s->result;uint32_t counts[13]={0};for(uint32_t i=0;i<r->count;i++)if(r->rows[i].state<=12)counts[r->rows[i].state]++;
    wchar_t text[700];
    if(r->error)swprintf(text,700,L"Could not compare both folders (Windows error %u). No copy is available. Recheck access and Compare again.",r->error);
    else swprintf(text,700,L"%ls — %u rows; only left %u; only right %u; size/date/content differences %u; metadata matches %u; SHA-256 matches %u; folders %u; excluded/conflicts/errors %u.",
        r->cancelled?L"Cancelled — incomplete":L"Complete",r->count,counts[1],counts[2],counts[4]+counts[5]+counts[6],counts[3],counts[7],counts[8],counts[9]+counts[10]+counts[11]+counts[12]);
    SetDlgItemTextW(s->hwnd,ID_STATUS,text);ListView_SetItemCountEx(GetDlgItem(s->hwnd,ID_ROWS),r->count,LVSICF_NOSCROLL);
    const wchar_t *props[]={L"OnlyLeft",L"OnlyRight",L"Metadata",L"Size",L"Date",L"Different",L"Equal",L"Folders",L"Types",L"Excluded",L"Ambiguous",L"Errors"};
    for(int i=0;i<12;i++){wchar_t p[80];swprintf(p,80,L"FolderCompare.%ls",props[i]);SetPropW(s->hwnd,p,(HANDLE)(UINT_PTR)counts[i+1]);}
    SetPropW(s->hwnd,L"FolderCompare.Count",(HANDLE)(UINT_PTR)r->count);SetPropW(s->hwnd,L"FolderCompare.Error",(HANDLE)(UINT_PTR)r->error);
    SetPropW(s->hwnd,L"FolderCompare.Cancelled",(HANDLE)(UINT_PTR)r->cancelled);SetPropW(s->hwnd,L"FolderCompare.Done",(HANDLE)1);
    detail(s);enable(s,!r->error&&!r->cancelled);
}
static void start(compare_window *s)
{
    if(s->busy)return;ListView_SetItemCount(GetDlgItem(s->hwnd,ID_ROWS),0);fpe_compare_free(&s->result);s->stop=0;s->finished=0;s->busy=1;s->hashes=IsDlgButtonChecked(s->hwnd,ID_HASH)==BST_CHECKED;s->recursive=IsDlgButtonChecked(s->hwnd,ID_SUBFOLDERS)==BST_CHECKED;
    SetPropW(s->hwnd,L"FolderCompare.Recursive",(HANDLE)(UINT_PTR)s->recursive);
    SetPropW(s->hwnd,L"FolderCompare.Busy",(HANDLE)1);SetPropW(s->hwnd,L"FolderCompare.Done",NULL);SetPropW(s->hwnd,L"FolderCompare.SelectedState",NULL);
    SetDlgItemTextW(s->hwnd,ID_STATUS,s->hashes?L"Comparing… reading matching-size files for SHA-256 verification":L"Comparing… reading folder metadata");SetDlgItemTextW(s->hwnd,ID_DETAIL,L"Paths are captured when this window opens. Files can change after a scan; Compare again to refresh. No files are modified by comparison.");enable(s,0);
    InterlockedIncrement(&s->refs);s->thread=CreateThread(NULL,0,worker,s,CREATE_SUSPENDED,NULL);
    if(s->thread)ResumeThread(s->thread);else{release(s);s->busy=0;s->result.error=GetLastError();SetPropW(s->hwnd,L"FolderCompare.Busy",NULL);show(s);}
}
static void cancel(compare_window *s)
{
    if(!s->busy)return;InterlockedExchange(&s->stop,1);if(s->thread)CancelSynchronousIo(s->thread);EnableWindow(GetDlgItem(s->hwnd,ID_STOP),FALSE);SetDlgItemTextW(s->hwnd,ID_STATUS,L"Cancelling… finishing the current filesystem request");
}
static void stage(compare_window *s,int side)
{
    if(s->busy||s->result.error||s->result.cancelled)return;HWND list=GetDlgItem(s->hwnd,ID_ROWS);wchar_t *paths[256]={0};uint32_t n=0;int valid=1;size_t chars=0;
    for(int i=ListView_GetNextItem(list,-1,LVNI_SELECTED);i>=0;i=ListView_GetNextItem(list,i,LVNI_SELECTED)){
        fpe_compare_row *r=s->result.rows+i;const fpe_entry *e=entry(s,r,side);
        if(n==256||!e||r->state==FPE_COMPARE_FOLDER||r->state>=FPE_COMPARE_TYPE||r->state==FPE_COMPARE_EQUAL||r->state==FPE_COMPARE_METADATA){valid=0;break;}
        size_t len=wcslen(name(s,r,side));chars+=len+1;if(chars>60000||wcslen(s->path[side])+len+2>32768||wcslen(s->path[!side])+len+2>32768){valid=0;break;}
        paths[n]=_wcsdup(name(s,r,side));if(!paths[n]){valid=0;break;}n++;
    }
    if(valid&&n){if(app_stage_tree_copy(s->path[side],(const wchar_t *const*)paths,n,s->path[!side]))SetDlgItemTextW(s->hwnd,ID_DETAIL,L"Copy staged in Operations. Review conflicts and choose Start there. This comparison is a snapshot; Compare again after the transfer.");else SetDlgItemTextW(s->hwnd,ID_DETAIL,L"Copy could not be staged. Check Operations is enabled and available, then try again.");}
    else SetDlgItemTextW(s->hwnd,ID_DETAIL,L"Select 1–256 differing regular files present on the source side. Matching, excluded, ambiguous and folder rows cannot be copied here. Selections allow up to 60,000 path characters.");
    for(uint32_t i=0;i<n;i++)free(paths[i]);
}
static HWND child(compare_window *s,const wchar_t *cls,const wchar_t *title,DWORD style,int id,int x,int y,int w,int h)
{
    HWND c=CreateWindowExW(0,cls,title,WS_CHILD|WS_VISIBLE|style,app_px(x),app_px(y),app_px(w),app_px(h),s->hwnd,(HMENU)(INT_PTR)id,GetModuleHandleW(NULL),NULL);SendMessageW(c,WM_SETFONT,(WPARAM)app_font(),FALSE);SetWindowTheme(c,L"DarkMode_Explorer",NULL);return c;
}
static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l)
{
    compare_window *s=(void*)GetWindowLongPtrW(h,GWLP_USERDATA);if(m==WM_NCCREATE){s=((CREATESTRUCTW*)l)->lpCreateParams;s->hwnd=h;SetWindowLongPtrW(h,GWLP_USERDATA,(LONG_PTR)s);}if(!s)return DefWindowProcW(h,m,w,l);
    switch(m){
    case WM_CREATE:PostMessageW(h,CM_BUILD,0,0);return 0;
    case CM_SHOW:ShowWindow(h,SW_SHOW);SetForegroundWindow(h);return 0;
    case CM_BUILD:{
        uint64_t began=fpe_prof_begin();
        const struct control {const wchar_t *cls,*title;DWORD style;int id,x,y,w,h;} controls[]={
            {L"STATIC",s->path[0],SS_PATHELLIPSIS,ID_LEFT,16,14,948,24},
            {L"STATIC",s->path[1],SS_PATHELLIPSIS,ID_RIGHT,16,42,948,24},
            {L"STATIC",L"Left and right pane folders • current level only • hidden items included • links/special items excluded",0,0,16,72,948,24},
            {L"BUTTON",L"Verify file contents with SHA-256",BS_AUTOCHECKBOX|WS_TABSTOP,ID_HASH,16,103,300,28},
            {L"BUTTON",L"Compare again",WS_TABSTOP,ID_SCAN,500,101,140,30},
            {L"BUTTON",L"Cancel scan",WS_TABSTOP,ID_STOP,654,101,140,30},
            {WC_LISTVIEWW,L"",LVS_REPORT|LVS_OWNERDATA|LVS_SHOWSELALWAYS|WS_TABSTOP,ID_ROWS,16,146,948,330},
            {L"STATIC",L"",0,ID_STATUS,16,488,948,44},
            {L"STATIC",L"",0,ID_DETAIL,16,540,948,70},
            {L"BUTTON",L"Copy selected → right",WS_TABSTOP,ID_COPY_RIGHT,16,630,210,32},
            {L"BUTTON",L"Copy selected ← left",WS_TABSTOP,ID_COPY_LEFT,244,630,210,32},
            {L"BUTTON",L"Close",WS_TABSTOP,IDCANCEL,854,630,110,32},
            {L"BUTTON",L"Include subfolders",BS_AUTOCHECKBOX|WS_TABSTOP,ID_SUBFOLDERS,320,103,176,28}};
        if(s->build<(int)(sizeof controls/sizeof *controls)){
            int i=s->build++;const struct control *c=controls+i;wchar_t title[33000];const wchar_t *label=c->title;
            if(c->id==ID_LEFT||c->id==ID_RIGHT){swprintf(title,33000,L"%ls: %ls",c->id==ID_LEFT?L"Left":L"Right",c->title);label=title;}
            if(i==2)label=s->root?L"Optional subfolders • hidden items included • links/mounts excluded • WSL hashes: configured root user":L"Optional subfolders • hidden items included • links/mounts excluded • WSL hashes: distro default user";
            HWND added=child(s,c->cls,label,c->style,c->id,c->x,c->y,c->w,c->h);
            if(c->id==ID_ROWS){ListView_SetExtendedListViewStyle(added,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);ListView_SetBkColor(added,C_BG);ListView_SetTextBkColor(added,C_BG);ListView_SetTextColor(added,C_TEXT);}
        }else if(s->build<18){
            int i=s->build++-13;wchar_t *titles[]={L"Left relative path",L"Right relative path",L"Comparison",L"Left bytes",L"Right bytes"};int widths[]={180,180,340,110,110};LVCOLUMNW col={.mask=LVCF_TEXT|LVCF_WIDTH,.pszText=titles[i],.cx=app_px(widths[i])};ListView_InsertColumn(GetDlgItem(h,ID_ROWS),i,&col);
        }else{start(s);FPE_PROF_END("compare.build_control",began);return 0;}
        FPE_PROF_END("compare.build_control",began);PostMessageW(h,CM_BUILD,0,0);return 0;
    }
    case CM_UPDATE:{
        EnterCriticalSection(&s->lock);int done=s->finished;s->queued=0;LeaveCriticalSection(&s->lock);
        if(done&&s->busy){s->busy=0;CloseHandle(s->thread);s->thread=NULL;SetPropW(h,L"FolderCompare.Busy",NULL);if(s->closing)DestroyWindow(h);else show(s);}return 0;
    }
    case WM_NOTIFY:{
        NMHDR *head=(void*)l;if(head->idFrom!=ID_ROWS||s->busy)return 0;
        if(head->code==LVN_GETDISPINFOW){
            NMLVDISPINFOW *d=(void*)l;if(d->item.iItem<0||(uint32_t)d->item.iItem>=s->result.count)return 0;
            static wchar_t value[1800];fpe_compare_row *r=s->result.rows+d->item.iItem;value[0]=0;
            if(d->item.iSubItem<2)display_path(name(s,r,d->item.iSubItem),fpe_dir_is_linux(s->result.dirs[d->item.iSubItem]),value,1800);
            else if(d->item.iSubItem==2)swprintf(value,1800,L"%ls%ls",label(s,r->state),r->state==FPE_COMPARE_FOLDER?(r->left<0?L" (only right)":r->right<0?L" (only left)":L""):L"");
            else{const fpe_entry *e=entry(s,r,d->item.iSubItem-3);if(e&&!fpe_entry_is_dir(e))swprintf(value,1800,L"%llu",(unsigned long long)e->size);}
            if(d->item.mask&LVIF_TEXT)lstrcpynW(d->item.pszText,value,d->item.cchTextMax);return 0;
        }
        if(head->code==NM_CUSTOMDRAW){NMLVCUSTOMDRAW *draw=(void*)l;if(draw->nmcd.dwDrawStage==CDDS_PREPAINT)return CDRF_NOTIFYITEMDRAW;
            if(draw->nmcd.dwDrawStage==CDDS_ITEMPREPAINT&&draw->nmcd.dwItemSpec<s->result.count){uint32_t state=s->result.rows[draw->nmcd.dwItemSpec].state;draw->clrText=state==FPE_COMPARE_LEFT?C_ACCENT:state==FPE_COMPARE_RIGHT?C_FOLDER:state==FPE_COMPARE_EQUAL?C_LINK:state==FPE_COMPARE_METADATA||state==FPE_COMPARE_FOLDER?C_DIM:C_ERROR;if(ListView_GetItemState(GetDlgItem(h,ID_ROWS),(int)draw->nmcd.dwItemSpec,LVIS_SELECTED)){draw->clrText=C_TEXT;draw->clrTextBk=C_SEL;}return CDRF_NEWFONT;}}
        if(head->code==LVN_ITEMCHANGED)detail(s);return 0;
    }
    case WM_COMMAND:
        if(LOWORD(w)==ID_SCAN)start(s);else if(LOWORD(w)==ID_STOP)cancel(s);else if(LOWORD(w)==ID_COPY_RIGHT)stage(s,0);else if(LOWORD(w)==ID_COPY_LEFT)stage(s,1);else if(LOWORD(w)==IDCANCEL)SendMessageW(h,WM_CLOSE,0,0);return 0;
    case WM_CLOSE:if(s->busy){s->closing=1;cancel(s);}else DestroyWindow(h);return 0;
    case WM_CTLCOLORSTATIC:case WM_CTLCOLORBTN:SetTextColor((HDC)w,C_TEXT);SetBkColor((HDC)w,C_BG);return (LRESULT)s->bg;
    case WM_ERASEBKGND:{RECT r;GetClientRect(h,&r);FillRect((HDC)w,&r,s->bg);return 1;}
    case WM_DESTROY:
        cancel(s);EnterCriticalSection(&s->lock);s->hwnd=NULL;LeaveCriticalSection(&s->lock);if(s->thread){CloseHandle(s->thread);s->thread=NULL;}
        MSG pending;while(PeekMessageW(&pending,h,CM_BUILD,CM_SHOW,PM_REMOVE)){}
        if(window==h)window=NULL;SetWindowLongPtrW(h,GWLP_USERDATA,0);fn_release(s->owner);release(s);return 0;
    }return DefWindowProcW(h,m,w,l);
}
void foldercompare_run(fn_ctx *c)
{
    if(window){ShowWindow(window,SW_SHOW);SetForegroundWindow(window);return;}if(!c->pane)return;
    const wchar_t *left=app_pane_path(app_left_pane()),*right=app_pane_path(app_other_pane(app_left_pane()));if(!left||!right)return;
    compare_window *s=calloc(1,sizeof *s);if(!s)return;s->refs=1;s->owner=fn_self();s->path[0]=_wcsdup(left);s->path[1]=_wcsdup(right);InitializeCriticalSection(&s->lock);s->bg=CreateSolidBrush(C_BG);
    s->root=!_stricmp(fn_option(fn_find("wsl.helper"),"user"),"root");if(!s->path[0]||!s->path[1]||fn_hold(s->owner)){release(s);return;}
    WNDCLASSW cls={.lpfnWndProc=proc,.hInstance=GetModuleHandleW(NULL),.hCursor=LoadCursorW(NULL,IDC_ARROW),.lpszClassName=L"OpenPilotFolderCompare"};RegisterClassW(&cls);
    RECT r={0,0,app_px(980),app_px(682)},parent;AdjustWindowRectEx(&r,WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN,0,WS_EX_CONTROLPARENT);GetWindowRect(app_window(),&parent);
    HWND opened=CreateWindowExW(WS_EX_CONTROLPARENT,cls.lpszClassName,L"Compare folders",WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN,
        parent.left+32,parent.top+32,r.right-r.left,r.bottom-r.top,app_window(),NULL,cls.hInstance,s);
    if(!opened){fn_release(s->owner);release(s);return;}window=opened;PostMessageW(opened,CM_SHOW,0,0);
}
int foldercompare_message(MSG *msg){return window&&(msg->hwnd==window||IsChild(window,msg->hwnd))&&IsDialogMessageW(window,msg);}
void foldercompare_shutdown(void){if(window)DestroyWindow(window);}
