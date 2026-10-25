/* On-demand, one-root logical sizes. No timers, cache or browsing hooks. */
#include "foldersize.h"
#include "app.h"
#include "theme.h"
#include <stdlib.h>
#include <stdio.h>
#include <uxtheme.h>
enum { ID_PATH=101,ID_STATE,ID_TOTAL,ID_COUNTS,ID_EXCLUDED,ID_ERROR,ID_ROLE,ID_CANCEL,ID_AGAIN };
#define SM_UPDATE (WM_APP+1)
typedef struct size_window {
    HWND hwnd;HANDLE thread;const fn_def *owner;wchar_t *path;HBRUSH bg;
    CRITICAL_SECTION lock;LONG refs;volatile LONG stop;
    int root,busy,queued,closing;fpe_size_result result;
} size_window;
static HWND window;
static void release(size_window *s)
{
    if(!InterlockedDecrement(&s->refs)){DeleteCriticalSection(&s->lock);DeleteObject(s->bg);free(s->path);free(s);}
}
static int update(void *user,const fpe_size_result *r)
{
    size_window *s=user;EnterCriticalSection(&s->lock);s->result=*r;
    if(s->hwnd&&!s->queued)s->queued=PostMessageW(s->hwnd,SM_UPDATE,0,0)!=0;
    LeaveCriticalSection(&s->lock);return !InterlockedCompareExchange(&s->stop,0,0);
}
static DWORD WINAPI worker(void *user)
{
    size_window *s=user;fpe_size_result result;fpe_folder_size(s->path,s->root,update,s,&result);release(s);return 0;
}
static void text(size_window *s,int id,const wchar_t *value){SetDlgItemTextW(s->hwnd,id,value);}
static void show(size_window *s,const fpe_size_result *r)
{
    wchar_t value[400];
    text(s,ID_STATE,r->state==FPE_SIZE_COMPLETE?L"Complete":r->state==FPE_SIZE_PARTIAL?L"Partial — some items could not be measured":r->state==FPE_SIZE_CANCELLED?L"Cancelled — totals are incomplete":s->stop?L"Cancelling…":L"Calculating…");
    double amount=(double)r->bytes;const wchar_t *unit=L"bytes";
    if(r->bytes>=1099511627776ULL){amount/=1099511627776.0;unit=L"TiB";}
    else if(r->bytes>=1073741824){amount/=1073741824.0;unit=L"GiB";}
    else if(r->bytes>=1048576){amount/=1048576.0;unit=L"MiB";}
    else if(r->bytes>=1024){amount/=1024.0;unit=L"KiB";}
    swprintf(value,400,L"Logical size: %.2f %ls (%llu bytes)%ls",amount,unit,(unsigned long long)r->bytes,r->state==FPE_SIZE_COMPLETE?L"":L" so far");text(s,ID_TOTAL,value);
    swprintf(value,400,L"Files: %llu     Folders: %llu (including the selected folder)",(unsigned long long)r->files,(unsigned long long)r->folders);text(s,ID_COUNTS,value);
    swprintf(value,400,L"Excluded links / special items / mount boundaries: %llu     Errors: %llu",(unsigned long long)r->skipped,(unsigned long long)r->errors);text(s,ID_EXCLUDED,value);
    if(r->first_error){
        wchar_t reason[220]=L"";FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,NULL,r->first_error,0,reason,220,NULL);
        swprintf(value,400,L"First error (%u): %ls",r->first_error,reason);text(s,ID_ERROR,value);
    }else text(s,ID_ERROR,r->state==FPE_SIZE_COMPLETE?L"Measured from live metadata. Recalculate after files change.":L"Files can change during a scan; totals reflect only the items measured.");
    SetPropW(s->hwnd,L"FolderSize.State",(HANDLE)(UINT_PTR)(r->state+1));
    SetPropW(s->hwnd,L"FolderSize.Bytes",(HANDLE)(UINT_PTR)r->bytes);
    SetPropW(s->hwnd,L"FolderSize.Files",(HANDLE)(UINT_PTR)r->files);
    SetPropW(s->hwnd,L"FolderSize.Folders",(HANDLE)(UINT_PTR)r->folders);
    SetPropW(s->hwnd,L"FolderSize.Errors",(HANDLE)(UINT_PTR)r->errors);
    SetPropW(s->hwnd,L"FolderSize.Skipped",(HANDLE)(UINT_PTR)r->skipped);
}
static void start(size_window *s)
{
    if(s->busy)return;
    s->result=(fpe_size_result){0};s->stop=0;s->busy=1;
    SetPropW(s->hwnd,L"FolderSize.Busy",(HANDLE)1);
    EnableWindow(GetDlgItem(s->hwnd,ID_AGAIN),FALSE);EnableWindow(GetDlgItem(s->hwnd,ID_CANCEL),TRUE);show(s,&s->result);
    InterlockedIncrement(&s->refs);s->thread=CreateThread(NULL,0,worker,s,CREATE_SUSPENDED,NULL);
    if(s->thread)ResumeThread(s->thread);
    else{
        DWORD error=GetLastError();release(s);s->busy=0;s->result.state=FPE_SIZE_PARTIAL;s->result.errors=1;s->result.first_error=error;
        SetPropW(s->hwnd,L"FolderSize.Busy",NULL);EnableWindow(GetDlgItem(s->hwnd,ID_AGAIN),TRUE);EnableWindow(GetDlgItem(s->hwnd,ID_CANCEL),FALSE);show(s,&s->result);
    }
}
static void cancel(size_window *s)
{
    if(!s->busy)return;InterlockedExchange(&s->stop,1);
    if(s->thread)CancelSynchronousIo(s->thread);
    EnableWindow(GetDlgItem(s->hwnd,ID_CANCEL),FALSE);text(s,ID_STATE,L"Cancelling… finishing the current filesystem request");
}
static HWND child(size_window *s,const wchar_t *cls,const wchar_t *title,DWORD style,int id,int x,int y,int w,int h)
{
    HWND c=CreateWindowExW(0,cls,title,WS_CHILD|WS_VISIBLE|style,app_px(x),app_px(y),app_px(w),app_px(h),s->hwnd,(HMENU)(INT_PTR)id,GetModuleHandleW(NULL),NULL);
    SendMessageW(c,WM_SETFONT,(WPARAM)app_font(),FALSE);SetWindowTheme(c,L"DarkMode_Explorer",NULL);return c;
}
static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l)
{
    size_window *s=(void*)GetWindowLongPtrW(h,GWLP_USERDATA);
    if(m==WM_NCCREATE){s=((CREATESTRUCTW*)l)->lpCreateParams;s->hwnd=h;SetWindowLongPtrW(h,GWLP_USERDATA,(LONG_PTR)s);}
    if(!s)return DefWindowProcW(h,m,w,l);
    switch(m){
    case WM_CREATE:
        child(s,L"STATIC",s->path,SS_PATHELLIPSIS,ID_PATH,18,16,650,28);
        child(s,L"STATIC",fpe_wsl_native(s->path)?s->root?L"WSL user: root":L"WSL user: distro default":L"Windows filesystem",0,ID_ROLE,18,50,650,24);
        child(s,L"STATIC",L"",0,ID_STATE,18,84,650,26);
        child(s,L"STATIC",L"",0,ID_TOTAL,18,120,650,28);
        child(s,L"STATIC",L"",0,ID_COUNTS,18,156,650,28);
        child(s,L"STATIC",L"",0,ID_EXCLUDED,18,192,650,40);
        child(s,L"STATIC",L"",0,ID_ERROR,18,238,650,58);
        child(s,L"STATIC",L"Logical bytes, not space on disk. No file contents are read. Links and reparse points are excluded; hard links count once per name. Linux scans stay on the root filesystem.",0,0,18,304,650,60);
        child(s,L"BUTTON",L"Recalculate",WS_TABSTOP,ID_AGAIN,18,380,130,32);
        child(s,L"BUTTON",L"Cancel scan",WS_TABSTOP,ID_CANCEL,164,380,130,32);
        child(s,L"BUTTON",L"Close",WS_TABSTOP,IDCANCEL,558,380,110,32);
        start(s);return 0;
    case SM_UPDATE:{
        EnterCriticalSection(&s->lock);fpe_size_result r=s->result;s->queued=0;LeaveCriticalSection(&s->lock);
        show(s,&r);
        if(r.state!=FPE_SIZE_RUNNING){
            s->busy=0;if(s->thread){CloseHandle(s->thread);s->thread=NULL;}
            SetPropW(h,L"FolderSize.Busy",NULL);EnableWindow(GetDlgItem(h,ID_AGAIN),TRUE);EnableWindow(GetDlgItem(h,ID_CANCEL),FALSE);
            if(s->closing)DestroyWindow(h);
        }
        return 0;
    }
    case WM_COMMAND:
        if(LOWORD(w)==ID_CANCEL)cancel(s);
        else if(LOWORD(w)==ID_AGAIN)start(s);
        else if(LOWORD(w)==IDCANCEL)SendMessageW(h,WM_CLOSE,0,0);
        return 0;
    case WM_CLOSE:
        if(s->busy){s->closing=1;cancel(s);}else DestroyWindow(h);return 0;
    case WM_CTLCOLORSTATIC:case WM_CTLCOLORBTN:SetTextColor((HDC)w,C_TEXT);SetBkColor((HDC)w,C_BG);return (LRESULT)s->bg;
    case WM_ERASEBKGND:{RECT r;GetClientRect(h,&r);FillRect((HDC)w,&r,s->bg);return 1;}
    case WM_DESTROY:{
        cancel(s);EnterCriticalSection(&s->lock);s->hwnd=NULL;LeaveCriticalSection(&s->lock);
        if(s->thread){CloseHandle(s->thread);s->thread=NULL;}
        const wchar_t *props[]={L"FolderSize.Busy",L"FolderSize.State",L"FolderSize.Bytes",L"FolderSize.Files",L"FolderSize.Folders",L"FolderSize.Errors",L"FolderSize.Skipped"};
        for(size_t i=0;i<sizeof props/sizeof *props;i++)RemovePropW(h,props[i]);
        if(window==h)window=NULL;SetWindowLongPtrW(h,GWLP_USERDATA,0);fn_release(s->owner);release(s);return 0;
    }
    }
    return DefWindowProcW(h,m,w,l);
}
int foldersize_applies(const fn_ctx *c){return c->n?(c->state&FN_S_DIR)!=0:!!(c->state&FN_S_FOLDER);}
void foldersize_run(fn_ctx *c)
{
    if(window){ShowWindow(window,SW_SHOW);SetForegroundWindow(window);return;}
    const wchar_t *path=c->dir;
    if(c->n){wchar_t **paths=fn_paths(c);if(!paths||c->n!=1||c->dirs!=1)return;path=paths[0];}
    if(!path)return;size_window *s=calloc(1,sizeof *s);if(!s)return;
    s->refs=1;s->owner=fn_self();s->path=_wcsdup(path);InitializeCriticalSection(&s->lock);s->bg=CreateSolidBrush(C_BG);
    s->root=!_stricmp(fn_option(fn_find("wsl.helper"),"user"),"root");
    if(!s->path||fn_hold(s->owner)){release(s);return;}
    WNDCLASSW cls={.lpfnWndProc=proc,.hInstance=GetModuleHandleW(NULL),.hCursor=LoadCursorW(NULL,IDC_ARROW),.lpszClassName=L"OpenPilotFolderSize"};RegisterClassW(&cls);
    RECT r={0,0,app_px(686),app_px(430)},parent;AdjustWindowRectEx(&r,WS_CAPTION|WS_SYSMENU,0,WS_EX_CONTROLPARENT);GetWindowRect(app_window(),&parent);
    HWND opened=CreateWindowExW(WS_EX_CONTROLPARENT,cls.lpszClassName,L"Folder size",WS_CAPTION|WS_SYSMENU,
        parent.left+(parent.right-parent.left-(r.right-r.left))/2,parent.top+(parent.bottom-parent.top-(r.bottom-r.top))/2,
        r.right-r.left,r.bottom-r.top,app_window(),NULL,cls.hInstance,s);
    if(!opened){fn_release(s->owner);release(s);return;}
    window=opened;ShowWindow(opened,SW_SHOW);SetForegroundWindow(opened);app_focus_window(GetDlgItem(opened,ID_CANCEL));
}
int foldersize_message(MSG *msg){return window&&(msg->hwnd==window||IsChild(window,msg->hwnd))&&IsDialogMessageW(window,msg);}
void foldersize_shutdown(void){if(window)DestroyWindow(window);}
