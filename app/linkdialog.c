#include "../src/namecodec.h"
#include "linkdialog.h"
#include "pathentry.h"
#include "theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int linkdialog_name(const wchar_t *raw,wchar_t mapped[260])
{
    if(!raw||!*raw||!wcscmp(raw,L".")||!wcscmp(raw,L"..")||wcschr(raw,L'/'))return 0;
    char bytes[256];uint32_t length=0;
    if(!pathentry_bytes(raw,bytes,255,&length)||!length)return 0;
    return fpe_lx_from_bytes((const uint8_t*)bytes,length,mapped,260,1,0,NULL);
}
typedef struct draft {
    HWND hwnd,owner;HFONT font;HBRUSH bg,edit;WNDPROC edit_proc;
    const wchar_t *source,*dest;uint32_t count;int dpi,done,ok,relative;wchar_t name[260];
} draft;
static int px(draft *d,int n){return MulDiv(n,d->dpi,96);}
static HWND child(draft *d,const wchar_t *class,const wchar_t *text,DWORD style,int id,int x,int y,int w,int h)
{
    HWND c=CreateWindowExW(!wcscmp(class,L"EDIT")?WS_EX_CLIENTEDGE:0,class,text,WS_CHILD|WS_VISIBLE|style,
        px(d,x),px(d,y),px(d,w),px(d,h),d->hwnd,(HMENU)(INT_PTR)id,GetModuleHandleW(NULL),NULL);
    SendMessageW(c,WM_SETFONT,(WPARAM)d->font,TRUE);return c;
}
static LRESULT CALLBACK edit_proc(HWND h,UINT m,WPARAM w,LPARAM l)
{
    draft *d=(draft*)GetWindowLongPtrW(GetParent(h),GWLP_USERDATA);
    if((m==WM_KEYDOWN&&w=='A'&&GetKeyState(VK_CONTROL)<0)||(m==WM_CHAR&&w==1)){SendMessageW(h,EM_SETSEL,0,-1);return 0;}
    return CallWindowProcW(d->edit_proc,h,m,w,l);
}
static void valid(draft *d)
{
    wchar_t raw[260],mapped[260];GetDlgItemTextW(d->hwnd,103,raw,260);
    int ok=d->count!=1||linkdialog_name(raw,mapped);EnableWindow(GetDlgItem(d->hwnd,IDOK),ok);
    SetDlgItemTextW(d->hwnd,106,ok?L"Existing items are never replaced.":L"Use a Linux filename: no /, . or ..; at most 255 bytes.");
}
static void display(const wchar_t *path,wchar_t out[PATHENTRY_MAX+100])
{
    wchar_t distro[64],linux[PATHENTRY_MAX];
    if(pathentry_display(path,distro,64,linux,PATHENTRY_MAX))swprintf(out,PATHENTRY_MAX+100,L"WSL: %ls   %ls",distro,linux);
    else {wcsncpy(out,path,PATHENTRY_MAX+99);out[PATHENTRY_MAX+99]=0;}
}
static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l)
{
    draft *d=(draft*)GetWindowLongPtrW(h,GWLP_USERDATA);
    if(m==WM_NCCREATE){d=((CREATESTRUCTW*)l)->lpCreateParams;d->hwnd=h;SetWindowLongPtrW(h,GWLP_USERDATA,(LONG_PTR)d);}
    if(!d)return DefWindowProcW(h,m,w,l);
    switch(m){
    case WM_CREATE:{
        wchar_t source[PATHENTRY_MAX+100],dest[PATHENTRY_MAX+100],label[100];display(d->source,source);display(d->dest,dest);
        swprintf(label,100,d->count==1?L"Create a symbolic link to:":L"Create %u symbolic links. First target:",d->count);
        child(d,L"STATIC",label,0,0,18,16,554,22);
        child(d,L"STATIC",source,SS_PATHELLIPSIS,104,18,43,554,24);
        child(d,L"STATIC",L"In this folder:",0,0,18,76,554,22);
        child(d,L"STATIC",dest,SS_PATHELLIPSIS,105,18,103,554,24);
        child(d,L"BUTTON",L"Relative target",BS_AUTORADIOBUTTON|WS_GROUP|WS_TABSTOP,101,18,146,230,24);
        child(d,L"BUTTON",L"Absolute target",BS_AUTORADIOBUTTON|WS_TABSTOP,102,305,146,230,24);
        SendDlgItemMessageW(h,101,BM_SETCHECK,BST_CHECKED,0);
        child(d,L"STATIC",L"Relative: folders can move together. Absolute: keep the same Linux target path.",0,0,18,177,554,42);
        child(d,L"STATIC",d->count==1?L"Link name:":L"Link names: each item's current name",0,0,18,233,554,24);
        if(d->count==1){HWND edit=child(d,L"EDIT",d->name,ES_AUTOHSCROLL|WS_TABSTOP,103,110,228,462,28);
            SendMessageW(edit,EM_LIMITTEXT,258,0);d->edit_proc=(WNDPROC)SetWindowLongPtrW(edit,GWLP_WNDPROC,(LONG_PTR)edit_proc);}
        child(d,L"STATIC",L"Existing items are never replaced.",0,106,18,276,554,42);
        child(d,L"BUTTON",L"Create",BS_DEFPUSHBUTTON|WS_TABSTOP|WS_GROUP,IDOK,365,331,95,30);
        child(d,L"BUTTON",L"Cancel",WS_TABSTOP,IDCANCEL,477,331,95,30);valid(d);return 0;
    }
    case WM_COMMAND:
        if(LOWORD(w)==103&&HIWORD(w)==EN_CHANGE){valid(d);return 0;}
        if(LOWORD(w)==IDOK){
            wchar_t raw[260];GetDlgItemTextW(h,103,raw,260);
            if(d->count==1&&!linkdialog_name(raw,d->name))return 0;
            d->relative=SendDlgItemMessageW(h,101,BM_GETCHECK,0,0)==BST_CHECKED;d->ok=1;DestroyWindow(h);return 0;
        }
        if(LOWORD(w)==IDCANCEL){DestroyWindow(h);return 0;}break;
    case WM_CLOSE:DestroyWindow(h);return 0;
    case WM_DESTROY:d->done=1;d->hwnd=NULL;return 0;
    case WM_CTLCOLORSTATIC:case WM_CTLCOLORBTN:case WM_CTLCOLOREDIT:
        SetTextColor((HDC)w,C_TEXT);SetBkColor((HDC)w,m==WM_CTLCOLOREDIT?C_EDIT:C_BG);return (LRESULT)(m==WM_CTLCOLOREDIT?d->edit:d->bg);
    case WM_ERASEBKGND:{RECT r;GetClientRect(h,&r);FillRect((HDC)w,&r,d->bg);return 1;}
    }
    return DefWindowProcW(h,m,w,l);
}
int linkdialog_choose(HWND owner,HFONT font,const wchar_t *source,const wchar_t *dest,uint32_t count,int *relative,wchar_t name[260])
{
    if(!owner||!source||!dest||!count||count>256||!relative||!name)return 0;
    draft d={.owner=owner,.font=font,.source=source,.dest=dest,.count=count,.dpi=(int)GetDpiForWindow(owner)};
    if(!d.dpi)d.dpi=96;wcsncpy(d.name,name,259);d.bg=CreateSolidBrush(C_BG);d.edit=CreateSolidBrush(C_EDIT);
    WNDCLASSW cls={.lpfnWndProc=proc,.hInstance=GetModuleHandleW(NULL),.hCursor=LoadCursorW(NULL,IDC_ARROW),.lpszClassName=L"OpenPilotSymlink"};RegisterClassW(&cls);
    RECT r={0,0,px(&d,590),px(&d,379)},parent;AdjustWindowRectEx(&r,WS_CAPTION|WS_SYSMENU,0,WS_EX_CONTROLPARENT);GetWindowRect(owner,&parent);
    HWND h=CreateWindowExW(WS_EX_CONTROLPARENT,cls.lpszClassName,L"Create symbolic links",WS_CAPTION|WS_SYSMENU,
        parent.left+(parent.right-parent.left-(r.right-r.left))/2,parent.top+(parent.bottom-parent.top-(r.bottom-r.top))/2,
        r.right-r.left,r.bottom-r.top,owner,NULL,cls.hInstance,&d);
    if(h){
        int enabled=IsWindowEnabled(owner);EnableWindow(owner,FALSE);ShowWindow(h,SW_SHOW);SetForegroundWindow(h);
        SetFocus(GetDlgItem(h,count==1?103:101));if(count==1)SendDlgItemMessageW(h,103,EM_SETSEL,0,-1);
        MSG msg;
        while(!d.done){
            int result=GetMessageW(&msg,NULL,0,0);
            if(result<=0){if(result==0)PostQuitMessage((int)msg.wParam);break;}
            if(!(d.hwnd&&(msg.hwnd==d.hwnd||IsChild(d.hwnd,msg.hwnd))&&IsDialogMessageW(d.hwnd,&msg))){TranslateMessage(&msg);DispatchMessageW(&msg);}
        }
        if(d.hwnd)DestroyWindow(d.hwnd);
        if(IsWindow(owner)&&enabled){EnableWindow(owner,TRUE);SetForegroundWindow(owner);}
    }
    DeleteObject(d.bg);DeleteObject(d.edit);
    if(d.ok){*relative=d.relative;wcscpy(name,d.name);}return d.ok;
}
