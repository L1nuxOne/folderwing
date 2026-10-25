/* A cold modeless draft: edits invalidate review; workers own all filesystem
 * access and immutable batch requests; results remain visible after stopping. */
#include "bulkrename.h"
#include "renameutil.h"
#include "app.h"
#include "pathentry.h"
#include "theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uxtheme.h>
enum {ID_FIND=101,ID_REPLACE,ID_PREFIX,ID_SUFFIX,ID_KEEP,ID_NUMBER,ID_START,ID_DIGITS,
      ID_ROWS,ID_STATUS,ID_PREVIEW,ID_APPLY,ID_STOP,ID_DETAIL,ID_ROLE};
enum {REQUEST_LOAD,REQUEST_PREVIEW,REQUEST_APPLY};
#define BM_RESULT (WM_APP+1)
#define BM_PROGRESS (WM_APP+2)
#define BM_BUILD (WM_APP+3)
#define BM_SHOW (WM_APP+4)
typedef struct editor {
    HWND hwnd;const fn_def *owner;Pane *pane;wchar_t *folder;wchar_t (*sources)[260];
    fpe_rename_item *items;uint32_t count;int root,linux,busy,reviewed,sync,closing,uncertain,building;
    uint32_t build_index;
    LONG refs;volatile LONG stop;CRITICAL_SECTION lock;HBRUSH bg,edit;
    uint32_t completed;int applying,queued;
} editor;
typedef struct request {editor *e;fpe_rename_item *items;rename_rules rules;int kind;DWORD error;} request;
static HWND window;static WNDPROC edit_proc;
static LRESULT CALLBACK field_proc(HWND h,UINT m,WPARAM w,LPARAM l);
static void release(editor *e)
{
    if(!InterlockedDecrement(&e->refs)){DeleteCriticalSection(&e->lock);DeleteObject(e->bg);DeleteObject(e->edit);free(e->folder);free(e->sources);free(e->items);free(e);}
}
static void request_free(request *r){if(r){free(r->items);free(r);}}
static void text(editor *e,int id,const wchar_t *s){SetDlgItemTextW(e->hwnd,id,s);}
static const wchar_t *state_name(const fpe_rename_item *it)
{
    switch(it->state){
    case FPE_RENAME_READY:return L"Ready";case FPE_RENAME_UNCHANGED:return L"Unchanged";
    case FPE_RENAME_RENAMED:return L"Renamed";case FPE_RENAME_STOPPED:return L"Not attempted; stopped";
    case FPE_RENAME_BLOCKED:return L"Not applied; fix preview";case FPE_RENAME_UNCERTAIN:return L"Outcome unknown; reopen";
    case FPE_RENAME_ERROR:
        switch(it->error){case ERROR_FILE_EXISTS:case ERROR_ALREADY_EXISTS:return L"Name collision";
        case ERROR_INVALID_NAME:case ERROR_FILENAME_EXCED_RANGE:return L"Invalid / too long name";
        case ERROR_FILE_INVALID:return L"Source changed; preview again";
        case ERROR_FILE_NOT_FOUND:case ERROR_PATH_NOT_FOUND:return L"Source missing";
        case ERROR_ACCESS_DENIED:return L"Permission denied";
        case ERROR_NOT_SUPPORTED:return L"Native helper unavailable";default:return L"Failed; see details";}
    default:return L"Waiting for preview";
    }
}
static void details(editor *e)
{
    int row=(int)SendDlgItemMessageW(e->hwnd,ID_ROWS,LB_GETCURSEL,0,0);if(row<0||(uint32_t)row>=e->count)row=0;
    wchar_t before[1600],after[1600],s[3800],reason[256]=L"";fpe_rename_item *it=&e->items[row];
    rename_display(it->from,e->linux,before,1600);rename_display(it->to,e->linux,after,1600);
    if(it->error)FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,NULL,it->error,0,reason,256,NULL);
    swprintf(s,3800,L"Before: %ls\nAfter:  %ls\n%ls%ls%ls",before,after,state_name(it),it->error?L" — ":L"",reason);text(e,ID_DETAIL,s);
}
static void rows(editor *e)
{
    HWND list=GetDlgItem(e->hwnd,ID_ROWS);int selected=(int)SendMessageW(list,LB_GETCURSEL,0,0);
    SendMessageW(list,WM_SETREDRAW,FALSE,0);SendMessageW(list,LB_RESETCONTENT,0,0);
    size_t longest=0;
    for(uint32_t i=0;i<e->count;i++){
        wchar_t before[1600],after[1600],row[3400];rename_display(e->items[i].from,e->linux,before,1600);rename_display(e->items[i].to,e->linux,after,1600);
        swprintf(row,3400,L"%u. %ls  →  %ls   [%ls]",i+1,before,after,state_name(&e->items[i]));
        size_t length=wcslen(row);if(length>longest)longest=length;SendMessageW(list,LB_ADDSTRING,0,(LPARAM)row);
    }
    /* Escaped control/raw-byte names can be much wider than normal names. */
    SendMessageW(list,LB_SETHORIZONTALEXTENT,app_px((int)(longest*24>6500?longest*24:6500)),0);
    SendMessageW(list,LB_SETCURSEL,selected>=0?selected:0,0);SendMessageW(list,WM_SETREDRAW,TRUE,0);InvalidateRect(list,NULL,TRUE);details(e);
}
static void controls(editor *e)
{
    for(int id=ID_FIND;id<=ID_DIGITS;id++)EnableWindow(GetDlgItem(e->hwnd,id),!e->busy);
    EnableWindow(GetDlgItem(e->hwnd,ID_PREVIEW),!e->busy&&!e->uncertain);
    EnableWindow(GetDlgItem(e->hwnd,ID_APPLY),!e->busy&&e->reviewed&&!e->uncertain);
    EnableWindow(GetDlgItem(e->hwnd,ID_STOP),e->busy&&!e->stop);
    SetPropW(e->hwnd,L"BulkRename.Reviewed",(HANDLE)(UINT_PTR)e->reviewed);SetPropW(e->hwnd,L"BulkRename.Busy",(HANDLE)(UINT_PTR)e->busy);
}
static int progress(void *user,uint32_t completed,int applying)
{
    editor *e=user;EnterCriticalSection(&e->lock);e->completed=completed;e->applying=applying;
    if(e->hwnd&&!e->queued)e->queued=PostMessageW(e->hwnd,BM_PROGRESS,0,0)!=0;
    LeaveCriticalSection(&e->lock);return !InterlockedCompareExchange(&e->stop,0,0);
}
static DWORD WINAPI work(void *arg)
{
    request *r=arg;editor *e=r->e;int valid=1;
    if(r->kind==REQUEST_PREVIEW){
        /* Refresh type/identity before applying extension rules. A file which
         * was replaced by a folder must not reuse stale file-extension policy. */
        r->error=fpe_rename_batch(e->folder,e->root,r->items,e->count,0,progress,e);
        if(r->error)goto reply;
    }
    if(r->kind==REQUEST_PREVIEW)for(uint32_t i=0;i<e->count;i++){
        if(!rename_transform(r->items[i].from,!!(r->items[i].identity.attrs&FILE_ATTRIBUTE_DIRECTORY),e->linux,i,&r->rules,r->items[i].to)){
            r->items[i].state=FPE_RENAME_ERROR;r->items[i].error=ERROR_INVALID_NAME;valid=0;
        }else{r->items[i].state=FPE_RENAME_PENDING;r->items[i].error=0;}
    }
    if(valid)r->error=fpe_rename_batch(e->folder,e->root,r->items,e->count,r->kind==REQUEST_APPLY,progress,e);
    else{
        r->error=ERROR_INVALID_NAME;for(uint32_t i=0;i<e->count;i++)if(r->items[i].state==FPE_RENAME_PENDING)r->items[i].state=FPE_RENAME_BLOCKED;
    }
reply:
    EnterCriticalSection(&e->lock);if(!e->hwnd||!PostMessageW(e->hwnd,BM_RESULT,0,(LPARAM)r))request_free(r);LeaveCriticalSection(&e->lock);release(e);return 0;
}
static int input(editor *e,rename_rules *r)
{
    int ids[]={ID_FIND,ID_REPLACE,ID_PREFIX,ID_SUFFIX};wchar_t *targets[]={r->find,r->replace,r->prefix,r->suffix};
    for(int i=0;i<4;i++){
        if(GetWindowTextLengthW(GetDlgItem(e->hwnd,ids[i]))>=RENAME_TEXT){text(e,ID_STATUS,L"A rule is too long. No text will be truncated.");return 0;}
        GetDlgItemTextW(e->hwnd,ids[i],targets[i],RENAME_TEXT);
    }
    r->keep_extension=SendDlgItemMessageW(e->hwnd,ID_KEEP,BM_GETCHECK,0,0)==BST_CHECKED;
    r->numbering=SendDlgItemMessageW(e->hwnd,ID_NUMBER,BM_GETCHECK,0,0)==BST_CHECKED;
    r->start=1;r->digits=3;
    if(r->numbering){
        wchar_t start[24],digits[24];GetDlgItemTextW(e->hwnd,ID_START,start,24);GetDlgItemTextW(e->hwnd,ID_DIGITS,digits,24);
        if(!rename_decimal(start,0,999999999u-e->count+1,&r->start)||!rename_decimal(digits,1,9,&r->digits)){
            text(e,ID_STATUS,L"Numbering needs a valid nonnegative start (up to 999999999) and 1–9 digits, without overflow.");return 0;}
    }
    return 1;
}
static void start(editor *e,int kind)
{
    if(e->busy||e->uncertain||(kind==REQUEST_APPLY&&!e->reviewed))return;
    request *r=calloc(1,sizeof *r);if(!r){text(e,ID_STATUS,L"Not enough memory.");return;}
    r->e=e;r->kind=kind;
    if(kind==REQUEST_PREVIEW&&!input(e,&r->rules)){request_free(r);return;}
    r->items=calloc(e->count,sizeof *r->items);if(!r->items){request_free(r);text(e,ID_STATUS,L"Not enough memory.");return;}
    if(kind==REQUEST_APPLY)memcpy(r->items,e->items,e->count*sizeof *e->items);
    else for(uint32_t i=0;i<e->count;i++){
        wcscpy(r->items[i].from,e->sources[i]);wcscpy(r->items[i].to,e->sources[i]);r->items[i].identity=e->items[i].identity;
    }
    e->busy=1;e->reviewed=0;e->completed=0;e->applying=0;InterlockedExchange(&e->stop,0);
    SetPropW(e->hwnd,L"BulkRename.Completed",NULL);controls(e);
    text(e,ID_STATUS,kind==REQUEST_APPLY?L"Rechecking the reviewed batch before renaming…":kind==REQUEST_LOAD?L"Reading selected items…":L"Checking proposed names and existing targets…");
    InterlockedIncrement(&e->refs);HANDLE thread=CreateThread(NULL,0,work,r,0,NULL);
    if(thread)CloseHandle(thread);
    else{DWORD error=GetLastError();release(e);request_free(r);e->busy=0;controls(e);wchar_t msg[80];swprintf(msg,80,L"Could not start the rename worker (%u).",error);text(e,ID_STATUS,msg);}
}
static void stop(editor *e)
{
    if(!e->busy)return;InterlockedExchange(&e->stop,1);EnableWindow(GetDlgItem(e->hwnd,ID_STOP),FALSE);
    text(e,ID_STATUS,L"Stopping remaining items after the current request. Completed renames remain applied.");
}
static HWND child(editor *e,const wchar_t *cls,const wchar_t *label,DWORD style,int id,int x,int y,int w,int h)
{
    HWND c=CreateWindowExW(!wcscmp(cls,L"EDIT")?WS_EX_CLIENTEDGE:0,cls,label,WS_CHILD|WS_VISIBLE|style,
        app_px(x),app_px(y),app_px(w),app_px(h),e->hwnd,(HMENU)(INT_PTR)id,GetModuleHandleW(NULL),NULL);
    SendMessageW(c,WM_SETFONT,(WPARAM)app_font(),FALSE);SetWindowTheme(c,L"DarkMode_Explorer",NULL);
    if(!wcscmp(cls,L"EDIT")){SendMessageW(c,EM_LIMITTEXT,RENAME_TEXT,0);WNDPROC old=(WNDPROC)SetWindowLongPtrW(c,GWLP_WNDPROC,(LONG_PTR)field_proc);if(!edit_proc)edit_proc=old;}
    return c;
}
static LRESULT CALLBACK field_proc(HWND h,UINT m,WPARAM w,LPARAM l)
{
    if((m==WM_KEYDOWN&&w=='A'&&GetKeyState(VK_CONTROL)<0)||(m==WM_CHAR&&w==1)){SendMessageW(h,EM_SETSEL,0,-1);return 0;}
    return CallWindowProcW(edit_proc,h,m,w,l);
}
static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l)
{
    editor *e=(editor*)GetWindowLongPtrW(h,GWLP_USERDATA);
    if(m==WM_NCCREATE){e=((CREATESTRUCTW*)l)->lpCreateParams;e->hwnd=h;SetWindowLongPtrW(h,GWLP_USERDATA,(LONG_PTR)e);}
    if(!e)return DefWindowProcW(h,m,w,l);
    switch(m){
    case WM_CREATE:
        e->sync=e->busy=e->building=1;SetPropW(h,L"BulkRename.Busy",(HANDLE)1);
        PostMessageW(h,BM_BUILD,0,0);return 0;
    case BM_SHOW:
        ShowWindow(h,SW_SHOW);SetForegroundWindow(h);return 0;
    case BM_BUILD:{
        /* One native control per message keeps opening a large draft within
         * the same dispatch budget as ordinary browsing. */
        typedef struct control {const wchar_t *cls,*label;DWORD style;int id,x,y,w,h;} control;
        static const control ui[]={
            {L"STATIC",NULL,SS_PATHELLIPSIS,0,18,14,864,24},
            {L"STATIC",NULL,0,ID_ROLE,18,44,864,24},
            {L"STATIC",L"Find (literal, case-sensitive)",0,0,18,80,420,22},
            {L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,ID_FIND,18,104,420,28},
            {L"STATIC",L"Replace with",0,0,458,80,420,22},
            {L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,ID_REPLACE,458,104,420,28},
            {L"STATIC",L"Prefix",0,0,18,144,420,22},
            {L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,ID_PREFIX,18,168,420,28},
            {L"STATIC",L"Suffix",0,0,458,144,420,22},
            {L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,ID_SUFFIX,458,168,420,28},
            {L"BUTTON",L"Preserve file extensions",BS_AUTOCHECKBOX|WS_TABSTOP,ID_KEEP,18,219,250,26},
            {L"BUTTON",L"Append _number",BS_AUTOCHECKBOX|WS_TABSTOP,ID_NUMBER,300,219,180,26},
            {L"STATIC",L"Start",0,0,500,222,45,22},
            {L"EDIT",L"1",ES_AUTOHSCROLL|WS_TABSTOP,ID_START,548,216,115,28},
            {L"STATIC",L"Digits",0,0,690,222,50,22},
            {L"EDIT",L"3",ES_AUTOHSCROLL|WS_TABSTOP,ID_DIGITS,744,216,88,28},
            {L"STATIC",L"Before → After — numbering follows this captured pane order. Preview never renames files.",0,0,18,261,864,24},
            {L"LISTBOX",L"",LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|WS_TABSTOP|WS_VSCROLL|WS_HSCROLL,ID_ROWS,18,291,864,188},
            {L"STATIC",L"",0,ID_DETAIL,18,491,864,74},
            {L"STATIC",L"",0,ID_STATUS,18,575,864,44},
            {L"STATIC",L"Existing targets and duplicate names block Apply. Folder contents keep their names. Cancel stops remaining items; completed renames stay applied.",0,0,18,624,864,42},
            {L"BUTTON",L"Preview",WS_TABSTOP,ID_PREVIEW,18,684,120,32},
            {L"BUTTON",L"Apply",WS_TABSTOP,ID_APPLY,154,684,120,32},
            {L"BUTTON",L"Cancel remaining",WS_TABSTOP,ID_STOP,290,684,166,32},
            {L"BUTTON",L"Close",WS_TABSTOP,IDCANCEL,762,684,120,32},
        };
        if(e->build_index<sizeof ui/sizeof *ui){
            int64_t began=fpe_prof_begin();const control *c=&ui[e->build_index++];wchar_t role[180];
            swprintf(role,180,e->linux?L"%u selected items — WSL user: %ls":L"%u selected items — Windows",e->count,e->root?L"root":L"distro default");
            HWND added=child(e,c->cls,c->label?c->label:c->id==ID_ROLE?role:e->folder,c->style,c->id,c->x,c->y,c->w,c->h);
            if(c->id==ID_KEEP)SendMessageW(added,BM_SETCHECK,BST_CHECKED,0);
            if(c->id==ID_ROWS)SendMessageW(added,LB_SETHORIZONTALEXTENT,app_px(6500),0);
            FPE_PROF_END("bulk_rename.build_control",began);PostMessageW(h,BM_BUILD,0,0);return 0;
        }
        e->sync=e->busy=e->building=0;rows(e);SetPropW(h,L"BulkRename.ItemCount",(HANDLE)(UINT_PTR)e->count);
        start(e,REQUEST_LOAD);app_focus_window(GetDlgItem(h,ID_FIND));return 0;
    }
    case WM_COMMAND:{
        int id=LOWORD(w),code=HIWORD(w);
        if(id==IDCANCEL){SendMessageW(h,WM_CLOSE,0,0);return 0;}
        if(id==ID_ROWS&&code==LBN_SELCHANGE){details(e);return 0;}
        if(id==ID_STOP){stop(e);return 0;}
        if(e->busy||e->sync)return 0;
        if(id==ID_PREVIEW){start(e,REQUEST_PREVIEW);return 0;}
        if(id==ID_APPLY){start(e,REQUEST_APPLY);return 0;}
        if((id>=ID_FIND&&id<=ID_SUFFIX&&code==EN_CHANGE)||id==ID_KEEP||id==ID_NUMBER||
           ((id==ID_START||id==ID_DIGITS)&&code==EN_CHANGE)){
            e->reviewed=0;text(e,ID_STATUS,L"Rules changed. Preview again before Apply; the rows below show the previous draft.");controls(e);return 0;
        }
        break;
    }
    case BM_PROGRESS:{
        EnterCriticalSection(&e->lock);uint32_t completed=e->completed;int applying=e->applying;e->queued=0;LeaveCriticalSection(&e->lock);
        SetPropW(h,L"BulkRename.Completed",(HANDLE)(UINT_PTR)completed);
        if(!e->stop){wchar_t value[100];swprintf(value,100,L"%ls %u of %u…",applying?L"Renaming":L"Checking",completed,e->count);text(e,ID_STATUS,value);}return 0;
    }
    case BM_RESULT:{
        request *r=(request*)l;e->busy=0;memcpy(e->items,r->items,e->count*sizeof *e->items);
        uint32_t ready=0,renamed=0,errors=0,stopped=0,unchanged=0,unknown=0,blocked=0;
        for(uint32_t i=0;i<e->count;i++){
            fpe_rename_item *it=&e->items[i];ready+=it->state==FPE_RENAME_READY;renamed+=it->state==FPE_RENAME_RENAMED;
            errors+=it->state==FPE_RENAME_ERROR;stopped+=it->state==FPE_RENAME_STOPPED;unchanged+=it->state==FPE_RENAME_UNCHANGED;unknown+=it->state==FPE_RENAME_UNCERTAIN;blocked+=it->state==FPE_RENAME_BLOCKED;
            if(it->state==FPE_RENAME_RENAMED)wcscpy(e->sources[i],it->to);
        }
        e->uncertain=unknown!=0;e->reviewed=r->kind==REQUEST_PREVIEW&&ready&&!r->error&&ready+unchanged==e->count;
        SetPropW(h,L"BulkRename.Renamed",(HANDLE)(UINT_PTR)renamed);SetPropW(h,L"BulkRename.Errors",(HANDLE)(UINT_PTR)errors);
        SetPropW(h,L"BulkRename.Stopped",(HANDLE)(UINT_PTR)stopped);SetPropW(h,L"BulkRename.Uncertain",(HANDLE)(UINT_PTR)unknown);
        wchar_t status[280];
        if(unknown)wcscpy(status,L"Some rename outcomes could not be confirmed. Refresh the pane and reopen the selection before continuing.");
        else if(r->kind==REQUEST_APPLY)swprintf(status,280,L"%u renamed; %u failed; %u unchanged; %u blocked; %u stopped. Preview again for another batch.",renamed,errors,unchanged,blocked,stopped);
        else if(stopped)wcscpy(status,L"Preview stopped. No files renamed. Preview again to check the entire draft.");
        else if(errors||r->error)swprintf(status,280,L"%u items have errors. Review the rows and fix names or collisions, then Preview again. No files renamed.",errors);
        else if(ready)swprintf(status,280,L"%u will be renamed; %u unchanged. Review before → after names, then Apply.",ready,unchanged);
        else wcscpy(status,L"No name changes. Set rules, then Preview. File extensions are preserved by default.");
        text(e,ID_STATUS,status);rows(e);controls(e);
        if(r->kind==REQUEST_APPLY){app_refresh(e->pane);Pane *other=app_other_pane(e->pane);if(other)app_refresh(other);}
        request_free(r);if(e->closing)DestroyWindow(h);return 0;
    }
    case WM_CLOSE:if(e->building){DestroyWindow(h);return 0;}if(e->busy){e->closing=1;stop(e);}else DestroyWindow(h);return 0;
    case WM_CTLCOLORSTATIC:case WM_CTLCOLORBTN:case WM_CTLCOLOREDIT:case WM_CTLCOLORLISTBOX:{int edit=m==WM_CTLCOLOREDIT||m==WM_CTLCOLORLISTBOX;
        SetTextColor((HDC)w,C_TEXT);SetBkColor((HDC)w,edit?C_EDIT:C_BG);return (LRESULT)(edit?e->edit:e->bg);}
    case WM_ERASEBKGND:{RECT rect;GetClientRect(h,&rect);FillRect((HDC)w,&rect,e->bg);return 1;}
    case WM_DESTROY:{
        InterlockedExchange(&e->stop,1);EnterCriticalSection(&e->lock);e->hwnd=NULL;LeaveCriticalSection(&e->lock);
        MSG msg;while(PeekMessageW(&msg,h,BM_RESULT,BM_SHOW,PM_REMOVE)){if(msg.message==WM_QUIT){PostQuitMessage((int)msg.wParam);break;}if(msg.message==BM_RESULT)request_free((request*)msg.lParam);}
        const wchar_t *props[]={L"BulkRename.ItemCount",L"BulkRename.Busy",L"BulkRename.Reviewed",L"BulkRename.Completed",L"BulkRename.Renamed",L"BulkRename.Errors",L"BulkRename.Stopped",L"BulkRename.Uncertain"};
        for(size_t i=0;i<sizeof props/sizeof *props;i++)RemovePropW(h,props[i]);
        if(window==h)window=NULL;SetWindowLongPtrW(h,GWLP_USERDATA,0);fn_release(e->owner);release(e);return 0;
    }
    }
    return DefWindowProcW(h,m,w,l);
}
int bulkrename_applies(const fn_ctx *c){return c->n&&c->n<=FPE_RENAME_MAX&&!(c->state&FN_S_ROOT);}
void bulkrename_run(fn_ctx *c)
{
    if(window){ShowWindow(window,SW_SHOW);SetForegroundWindow(window);return;}
    wchar_t **paths=fn_paths(c);if(!paths||!c->n||c->n>FPE_RENAME_MAX||!c->dir)return;
    editor *e=calloc(1,sizeof *e);if(!e)return;e->refs=1;e->pane=c->pane;e->owner=fn_self();e->count=c->n;
    e->folder=_wcsdup(c->dir);e->sources=calloc(e->count,sizeof *e->sources);e->items=calloc(e->count,sizeof *e->items);InitializeCriticalSection(&e->lock);
    if(!e->folder||!e->sources||!e->items){release(e);return;}
    for(uint32_t i=0;i<e->count;i++){
        const wchar_t *name=wcsrchr(paths[i],L'\\');if(!name||!name[1]||wcslen(name+1)>255){release(e);return;}
        wcscpy(e->sources[i],name+1);wcscpy(e->items[i].from,name+1);wcscpy(e->items[i].to,name+1);
    }
    wchar_t distro[64],linux[PATHENTRY_MAX];e->linux=pathentry_display(e->folder,distro,64,linux,PATHENTRY_MAX)||fpe_wsl_native(e->folder);
    e->root=!_stricmp(fn_option(fn_find("wsl.helper"),"user"),"root");e->bg=CreateSolidBrush(C_BG);e->edit=CreateSolidBrush(C_EDIT);
    if(fn_hold(e->owner)){release(e);return;}
    WNDCLASSW cls={.lpfnWndProc=proc,.hInstance=GetModuleHandleW(NULL),.hCursor=LoadCursorW(NULL,IDC_ARROW),.lpszClassName=L"OpenPilotBulkRename"};RegisterClassW(&cls);
    RECT rect={0,0,app_px(900),app_px(734)},parent;AdjustWindowRectEx(&rect,WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN,0,WS_EX_CONTROLPARENT);GetWindowRect(app_window(),&parent);
    HWND opened=CreateWindowExW(WS_EX_CONTROLPARENT,cls.lpszClassName,L"Bulk rename",WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN,
        parent.left+(parent.right-parent.left-(rect.right-rect.left))/2,parent.top+(parent.bottom-parent.top-(rect.bottom-rect.top))/2,
        rect.right-rect.left,rect.bottom-rect.top,app_window(),NULL,cls.hInstance,e);
    if(!opened){fn_release(e->owner);release(e);return;}window=opened;PostMessageW(opened,BM_SHOW,0,0);
}
int bulkrename_message(MSG *msg){return window&&(msg->hwnd==window||IsChild(window,msg->hwnd))&&IsDialogMessageW(window,msg);}
void bulkrename_shutdown(void){if(window)DestroyWindow(window);}
