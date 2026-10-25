#include "../src/namecodec.h"
/* Cold permission editor: bounded selection, immutable worker requests,
 * explicit bit patches, guarded nonrecursive chmod and per-item results. */
#include "permissions.h"
#include "permissionutil.h"
#include "app.h"
#include "theme.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <uxtheme.h>

enum { ID_MODE=101,ID_OWNER,ID_GROUP,ID_PREVIEW,ID_STATUS,ID_APPLY,ID_RELOAD,ID_ROLE,
       ID_STOP,ID_RESULTS,ID_RESET,ID_PATH,ID_BIT=120,ID_OWNERSHIP=132,
       ID_UID=140,ID_GID,ID_REVIEW,ID_BACK,ID_RESOLVED,ID_FIELDS };
#define PM_RESULT (WM_APP+1)
#define PM_PROGRESS (WM_APP+2)
enum { ITEM_PENDING,ITEM_READY,ITEM_APPLIED,ITEM_UNCHANGED,ITEM_FAILED,ITEM_STOPPED };
typedef struct item {fpe_permissions current;DWORD error;int valid,state,changed;} item;
typedef struct editor {
    HWND hwnd,fields,controls[ID_FIELDS+1];int building_fields;Pane *pane;const fn_def *owner;wchar_t **paths;uint32_t count;
    item *items;permission_patch patch;fpe_identity identity;int ownership,reviewed,root,busy,sync;
    volatile LONG stop;LONG refs;CRITICAL_SECTION lock;HBRUSH bg,edit;HFONT list_font;
} editor;
enum { REQUEST_READ,REQUEST_MODE,REQUEST_REVIEW,REQUEST_OWNER };
typedef struct request {
    editor *e;item *items;permission_patch patch;fpe_identity identity;
    wchar_t owner[128],group[128];uint32_t completed,bad_field;DWORD error;int apply,stopped;
} request;
static HWND open_editor(Pane *pane,const fn_def *owner,wchar_t **paths,uint32_t count,int root,int ownership);
static HWND window;
static WNDPROC edit_proc;
static const uint32_t bits[]={0400,0200,0100,0040,0020,0010,0004,0002,0001,04000,02000,01000};
static LRESULT CALLBACK octal_proc(HWND h,UINT m,WPARAM w,LPARAM l)
{
    if((m==WM_KEYDOWN&&w=='A'&&GetKeyState(VK_CONTROL)<0)||(m==WM_CHAR&&w==1)){
        SendMessageW(h,EM_SETSEL,0,-1);return 0;
    }
    return CallWindowProcW(edit_proc,h,m,w,l);
}
static void release(editor *e)
{
    if(!InterlockedDecrement(&e->refs)){
        DeleteCriticalSection(&e->lock);DeleteObject(e->bg);DeleteObject(e->edit);if(e->list_font)DeleteObject(e->list_font);
        for(uint32_t i=0;i<e->count;i++)free(e->paths[i]);free(e->paths);free(e->items);free(e);
    }
}
static void request_free(request *r){if(r){free(r->items);free(r);}}
static HWND control(editor *e,int id){return id>0&&id<=ID_FIELDS?e->controls[id]:NULL;}
static void enable_control(HWND h,int enable)
{
    if(h && ((GetWindowLongPtrW(h,GWL_STYLE)&WS_DISABLED)==0)!=(enable!=0))EnableWindow(h,enable);
}
static void text(editor *e,int id,const wchar_t *s)
{
    HWND h=control(e,id);if(!h)return;
    wchar_t current[512]={0};size_t n=wcslen(s);
    if(n<512&&GetWindowTextW(h,current,512)==(int)n&&!wcscmp(current,s))return;
    SetWindowTextW(h,s);
}
static uint32_t ready_count(editor *e){uint32_t n=0;for(uint32_t i=0;i<e->count;i++)n+=e->items[i].valid!=0;return n;}
static int parse(editor *e,uint32_t *mode)
{
    wchar_t s[16];GetWindowTextW(control(e,ID_MODE),s,16);return permission_octal(s,mode);
}
static int input_valid(editor *e)
{
    if(e->ownership)return e->reviewed;
    uint32_t mode;if(e->count==1)return parse(e,&mode);
    wchar_t s[16];GetWindowTextW(control(e,ID_MODE),s,16);return !s[0]||permission_octal(s,&mode);
}
static int original_bit(editor *e,uint32_t bit)
{
    uint32_t modes[PERMISSION_MAX_ITEMS],n=0;for(uint32_t i=0;i<e->count;i++)if(e->items[i].valid)modes[n++]=e->items[i].current.mode;
    return permission_bit_state(modes,n,bit);
}
static void checks(editor *e)
{
    for(int i=0;i<12;i++){
        int action=permission_patch_action(&e->patch,bits[i]),state=action;
        if(state==PERMISSION_KEEP)state=original_bit(e,bits[i]);
        HWND h=control(e,ID_BIT+i);int check=state==PERMISSION_KEEP?BST_INDETERMINATE:state?BST_CHECKED:BST_UNCHECKED;
        if(SendMessageW(h,BM_GETCHECK,0,0)!=check)SendMessageW(h,BM_SETCHECK,check,0);
        if(e->count>1){
            const wchar_t *label=action==PERMISSION_KEEP?(state==PERMISSION_KEEP?L"Mixed":L"Keep"):action==PERMISSION_SET?L"Set":L"Clear";
            wchar_t caption[40];if(i<9)wcscpy(caption,label);
            else swprintf(caption,40,L"%ls (%ls)",i==9?L"Setuid":i==10?L"Setgid":L"Sticky",label);
            text(e,ID_BIT+i,caption);
        }
    }
}
static const wchar_t *failure(DWORD error,int changed,int ownership)
{
    if(error==ERROR_FILE_INVALID)return L"Changed/replaced; Reload";
    if(changed)return ownership?L"Changed; verification failed; Reload":L"Changed; mode not retained; Reload";
    if(error==ERROR_ACCESS_DENIED)return L"Permission denied";
    if(error==ERROR_NOT_SUPPORTED)return L"Unsupported item/helper";
    if(error==ERROR_PATH_NOT_FOUND||error==ERROR_FILE_NOT_FOUND)return L"Missing item";
    return L"WSL reply failed; Reload";
}
static void name_display(const wchar_t *path,wchar_t out[40])
{
    const wchar_t *base=wcsrchr(path,L'\\');base=base?base+1:path;
    fpe_lx_display(base,out,40,0);
}
static void details(editor *e)
{
    int index=e->count==1?0:(int)SendMessageW(control(e,ID_RESULTS),LB_GETCURSEL,0,0);
    if(index<0||(uint32_t)index>=e->count)index=0;item *it=&e->items[index];wchar_t s[256];
    if(it->valid){
        swprintf(s,256,L"Owner: %ls  (uid %u)",it->current.owner[0]?it->current.owner:L"unknown",it->current.uid);text(e,ID_OWNER,s);
        swprintf(s,256,L"Group: %ls  (gid %u)",it->current.group[0]?it->current.group:L"unknown",it->current.gid);text(e,ID_GROUP,s);
    }else{ text(e,ID_OWNER,L"Owner: unavailable until this item is read");text(e,ID_GROUP,L"Group: unavailable until this item is read");}
    if(e->count>1||e->ownership)text(e,ID_PATH,e->paths[index]);
}
static int owner_diff(const fpe_identity *id,const fpe_permissions *current)
{
    return ((id->fields&FPE_ID_USER)&&id->uid!=current->uid)||((id->fields&FPE_ID_GROUP)&&id->gid!=current->gid);
}
static uint32_t change_count(editor *e)
{
    uint32_t changes=0;
    for(uint32_t i=0;i<e->count;i++)if(e->items[i].valid)
        changes+=e->ownership?(e->reviewed&&owner_diff(&e->identity,&e->items[i].current)):
            permission_patch_mode(&e->patch,e->items[i].current.mode)!=e->items[i].current.mode;
    return changes;
}
static void rows(editor *e)
{
    if(e->count==1&&!e->ownership)return;HWND list=control(e,ID_RESULTS);
    int selected=(int)SendMessageW(list,LB_GETCURSEL,0,0);SendMessageW(list,WM_SETREDRAW,FALSE,0);SendMessageW(list,LB_RESETCONTENT,0,0);
    int valid=input_valid(e);
    for(uint32_t i=0;i<e->count;i++){
        item *it=&e->items[i];wchar_t name[40],before[12]=L"----",after[12]=L"----",row[192];name_display(e->paths[i],name);
        const wchar_t *status=L"Waiting to read";
        if(it->valid){swprintf(before,12,L"%04o",it->current.mode&07777);
            if(valid)swprintf(after,12,L"%04o",permission_patch_mode(&e->patch,it->current.mode)&07777);
        }
        if(it->state==ITEM_APPLIED)status=L"Applied and verified";
        else if(it->state==ITEM_FAILED)status=failure(it->error,it->changed,e->ownership);
        else if(it->state==ITEM_STOPPED)status=L"Not attempted; stopped";
        else if(it->valid)status=(permission_patch_mode(&e->patch,it->current.mode)&07777)!=(it->current.mode&07777)?L"Will change":L"No change requested";
        if(it->valid&&(permission_patch_mode(&e->patch,it->current.mode)&07777)!=(it->current.mode&07777)&&it->state!=ITEM_FAILED)status=L"Will change";
        if(e->ownership){
            wchar_t uid[32]=L"----",gid[32]=L"----",mode[32]=L"----";
            int diff=it->valid&&e->reviewed&&owner_diff(&e->identity,&it->current);
            if(it->valid){
                swprintf(uid,32,L"%u -> %u",it->current.uid,e->reviewed&&(e->identity.fields&FPE_ID_USER)?e->identity.uid:it->current.uid);
                swprintf(gid,32,L"%u -> %u",it->current.gid,e->reviewed&&(e->identity.fields&FPE_ID_GROUP)?e->identity.gid:it->current.gid);
                swprintf(mode,32,L"%04o%ls",it->current.mode&07777,diff&&(it->current.mode&0170000)==0100000&&(it->current.mode&06000)?L"; may clear setid":L"");
                if(it->state!=ITEM_FAILED&&it->state!=ITEM_STOPPED&&it->state!=ITEM_APPLIED)status=diff?L"Will change":L"No change requested";
                if(diff&&it->state!=ITEM_FAILED)status=L"Will change";
            }
            if(wcslen(name)>18){size_t cut=15;if(name[cut-1]>=0xd800&&name[cut-1]<=0xdbff)cut--;wcscpy(name+cut,L"...");}
            swprintf(row,192,L"%-18ls | %-12ls | %-12ls | %-22ls | %ls",name,uid,gid,mode,status);
        }else
        swprintf(row,192,L"%-31ls | %ls | %ls | %ls",name,before,after,status);SendMessageW(list,LB_ADDSTRING,0,(LPARAM)row);
    }
    SendMessageW(list,LB_SETCURSEL,selected>=0?selected:0,0);SendMessageW(list,WM_SETREDRAW,TRUE,0);InvalidateRect(list,NULL,TRUE);details(e);
}
static void preview(editor *e)
{
    int valid=input_valid(e);uint32_t ready=ready_count(e),changes=change_count(e);wchar_t a[11],b[11],s[256];
    if(e->ownership){
        wchar_t owner[160]=L"Keep each item's owner",group[160]=L"Keep each item's group";
        if(e->reviewed){
            if(e->identity.fields&FPE_ID_USER)swprintf(owner,160,L"%ls (uid %u)",e->identity.owner[0]?e->identity.owner:L"Unmapped",e->identity.uid);
            if(e->identity.fields&FPE_ID_GROUP)swprintf(group,160,L"%ls (gid %u)",e->identity.group[0]?e->identity.group:L"Unmapped",e->identity.gid);
            wchar_t resolved[400];swprintf(resolved,400,L"Owner: %ls\nGroup: %ls",owner,group);text(e,ID_RESOLVED,resolved);
            swprintf(s,256,L"%u of %u items available; %u will change.",ready,e->count,changes);
        }else{wcscpy(s,L"Resolve and review names/IDs before Apply.");text(e,ID_RESOLVED,L"Blank fields keep each item's current value.");}
        text(e,ID_PREVIEW,s);rows(e);
        wchar_t u[128],g[128];GetWindowTextW(control(e,ID_UID),u,128);GetWindowTextW(control(e,ID_GID),g,128);
        enable_control(control(e,ID_REVIEW),ready&&!e->busy&&(u[0]||g[0])&&
            GetWindowTextLengthW(control(e,ID_UID))<=127&&GetWindowTextLengthW(control(e,ID_GID))<=127);
        SetPropW(e->hwnd,L"Permissions.Reviewed",(HANDLE)(INT_PTR)e->reviewed);
    }else if(e->count==1){
        uint32_t mode=0;
        if(ready&&parse(e,&mode)){
            fpe_unix_mode_string(e->items[0].current.mode,a);fpe_unix_mode_string((e->items[0].current.mode&~07777u)|mode,b);
            swprintf(s,256,L"Current: %04o  %ls\nAfter Apply: %04o  %ls",e->items[0].current.mode&07777,a,mode,b);text(e,ID_PREVIEW,s);
            changes=mode!=(e->items[0].current.mode&07777);
        }else text(e,ID_PREVIEW,ready?L"Enter three or four octal digits (0-7).":L"Read permissions before editing.");
    }else{
        if(!valid)wcscpy(s,L"Enter three or four octal digits (0-7), or leave blank to use the checkboxes.");
        else swprintf(s,256,L"%u of %u items available. %u will change.\nReview each item's last read mode and result below.",ready,e->count,changes);
        text(e,ID_PREVIEW,s);rows(e);
    }
    if(!e->ownership)enable_control(control(e,ID_OWNERSHIP),ready&&!e->busy&&valid&&!changes);
    enable_control(control(e,ID_APPLY),ready&&!e->busy&&valid&&changes);
}
static void show_state(editor *e)
{
    wchar_t s[16];e->sync=1;
    if(e->ownership){text(e,ID_UID,L"");text(e,ID_GID,L"");e->reviewed=0;}
    else if(e->count==1&&e->items[0].valid){swprintf(s,16,L"%04o",e->items[0].current.mode&07777);text(e,ID_MODE,s);}
    else text(e,ID_MODE,L"");if(!e->ownership)checks(e);e->sync=0;details(e);
}
static void controls(editor *e)
{
    uint32_t ready=ready_count(e);
    if(!e->ownership)enable_control(e->fields,ready&&!e->busy);
    enable_control(control(e,ID_RELOAD),!e->busy);enable_control(control(e,IDCANCEL),!e->busy);
    if(e->count>1||e->ownership){enable_control(control(e,ID_RESET),!e->busy&&ready);enable_control(control(e,ID_STOP),e->busy&&!e->stop);}
    if(e->ownership){
        enable_control(control(e,ID_UID),ready&&!e->busy);enable_control(control(e,ID_GID),ready&&!e->busy);
        enable_control(control(e,ID_BACK),!e->busy);
    }else enable_control(control(e,ID_OWNERSHIP),ready&&!e->busy&&input_valid(e)&&!change_count(e));
    SetPropW(e->hwnd,L"Permissions.Loaded",(HANDLE)(INT_PTR)(ready!=0));
    SetPropW(e->hwnd,L"Permissions.ReadyCount",(HANDLE)(UINT_PTR)ready);
    if(e->busy){
        /* Keep the last displayed snapshot while the worker reads. Rebuilding
         * every checkbox/row here repeats completion work on the input path. */
        enable_control(control(e,ID_APPLY),FALSE);
        if(e->ownership){enable_control(control(e,ID_REVIEW),FALSE);SetPropW(e->hwnd,L"Permissions.Reviewed",(HANDLE)(INT_PTR)e->reviewed);}
    }else preview(e);
    /* Completion becomes observable only after the controls and preview agree. */
    SetPropW(e->hwnd,L"Permissions.Busy",(HANDLE)(INT_PTR)e->busy);
}
static DWORD WINAPI work(void *arg)
{
    request *r=arg;editor *e=r->e;
    if(r->apply==REQUEST_REVIEW){
        r->error=fpe_wsl_identity(e->paths[0],e->root,r->owner,r->group,&r->identity,&r->bad_field);
        r->stopped=InterlockedCompareExchange(&e->stop,0,0)!=0;
        goto reply;
    }
    fpe_wsl_metadata *session=fpe_wsl_metadata_begin(e->root);
    for(uint32_t i=0;i<e->count;i++){
        item *it=&r->items[i];
        if(InterlockedCompareExchange(&e->stop,0,0)){
            r->stopped=1;for(uint32_t j=i;j<e->count;j++)r->items[j].state=ITEM_STOPPED;break;
        }
        if(r->apply&&!it->valid){r->completed++;continue;}
        uint32_t mode=permission_patch_mode(&r->patch,it->current.mode)&07777;
        if(r->apply&&(r->apply==REQUEST_OWNER?!owner_diff(&r->identity,&it->current):mode==(it->current.mode&07777))){it->state=ITEM_UNCHANGED;it->error=0;it->changed=0;r->completed++;continue;}
        fpe_permissions actual={0};int changed=0;
        DWORD error=!session?ERROR_NOT_ENOUGH_MEMORY:r->apply==REQUEST_OWNER?
            fpe_wsl_metadata_ownership(session,e->paths[i],&it->current,&r->identity,&actual,&changed):
            fpe_wsl_metadata_permissions(session,e->paths[i],r->apply?&it->current:NULL,mode,&actual,&changed);
        it->error=error;it->changed=changed;
        if(!error){it->current=actual;it->valid=1;it->state=r->apply?ITEM_APPLIED:ITEM_READY;}
        else{it->state=ITEM_FAILED;if(!r->apply||error!=ERROR_ACCESS_DENIED)it->valid=0;}
        r->completed++;
        EnterCriticalSection(&e->lock);if(e->hwnd)PostMessageW(e->hwnd,PM_PROGRESS,r->completed,r->apply);LeaveCriticalSection(&e->lock);
    }
    fpe_wsl_metadata_end(session);
reply:
    EnterCriticalSection(&e->lock);
    if(!e->hwnd||!PostMessageW(e->hwnd,PM_RESULT,0,(LPARAM)r))request_free(r);
    LeaveCriticalSection(&e->lock);release(e);return 0;
}
static void start(editor *e,int apply)
{
    int64_t prepare=fpe_prof_begin();
    if(e->busy)return;uint32_t mode=0;permission_patch patch=e->patch;
    if(apply==REQUEST_REVIEW&&(GetWindowTextLengthW(control(e,ID_UID))>127||GetWindowTextLengthW(control(e,ID_GID))>127)){
        text(e,ID_STATUS,L"Owner/group input is too long. No name or ID will be truncated.");return;
    }
    if(apply==REQUEST_MODE||apply==REQUEST_OWNER){
        if(!ready_count(e)||!input_valid(e))return;
        if(apply==REQUEST_MODE&&e->count==1){if(!parse(e,&mode)||mode==(e->items[0].current.mode&07777))return;patch=(permission_patch){07777,mode};}
        if(!change_count(e))return;
    }
    request *r=calloc(1,sizeof *r);if(!r){text(e,ID_STATUS,L"Not enough memory.");return;}
    r->items=calloc(e->count,sizeof *r->items);if(!r->items){request_free(r);text(e,ID_STATUS,L"Not enough memory.");return;}
    r->e=e;r->apply=apply;r->patch=patch;r->identity=e->identity;
    if(apply==REQUEST_REVIEW){
        GetWindowTextW(control(e,ID_UID),r->owner,128);GetWindowTextW(control(e,ID_GID),r->group,128);
        if(!r->owner[0]&&!r->group[0]){request_free(r);return;}e->reviewed=0;
    }else if(apply)memcpy(r->items,e->items,e->count*sizeof *r->items);
    else{memset(e->items,0,e->count*sizeof *e->items);e->patch=(permission_patch){0};e->reviewed=0;}
    FPE_PROF_END("permissions.request.prepare",prepare);
    int64_t update=fpe_prof_begin();
    e->busy=1;InterlockedExchange(&e->stop,0);SetPropW(e->hwnd,L"Permissions.Error",0);SetPropW(e->hwnd,L"Permissions.Completed",0);controls(e);
    text(e,ID_STATUS,apply==REQUEST_REVIEW?L"Resolving owner/group names and IDs...":apply==REQUEST_OWNER?L"Applying ownership...":apply?L"Applying permissions...":L"Reading current permissions...");
    FPE_PROF_END("permissions.request.controls",update);
    int64_t launch=fpe_prof_begin();
    InterlockedIncrement(&e->refs);HANDLE thread=CreateThread(NULL,0,work,r,0,NULL);
    if(thread)CloseHandle(thread);
    else{release(e);request_free(r);e->busy=0;text(e,ID_STATUS,L"Could not start the metadata worker.");controls(e);}
    FPE_PROF_END("permissions.request.worker_start",launch);
}
/* One input group lets a batch pause editing with one native enable change.
 * Native dialog navigation still traverses its children; notifications and
 * control colours keep using the editor's handlers. */
static LRESULT CALLBACK fields_proc(HWND h,UINT m,WPARAM w,LPARAM l)
{
    HWND parent=GetParent(h);
    if(m==WM_COMMAND||m==WM_CTLCOLORSTATIC||m==WM_CTLCOLOREDIT||m==WM_CTLCOLORBTN)
        return SendMessageW(parent,m,w,l);
    if(m==WM_ENABLE){InvalidateRect(h,NULL,TRUE);RedrawWindow(h,NULL,NULL,RDW_INVALIDATE|RDW_ALLCHILDREN);return 0;}
    if(m==WM_ERASEBKGND){RECT r;GetClientRect(h,&r);HBRUSH brush=(HBRUSH)SendMessageW(parent,WM_CTLCOLORSTATIC,w,(LPARAM)h);FillRect((HDC)w,&r,brush);return 1;}
    return DefWindowProcW(h,m,w,l);
}
static HWND child(editor *e,const wchar_t *kind,const wchar_t *caption,DWORD style,int id,int x,int y,int w,int h)
{
    HWND c=CreateWindowExW(!wcscmp(kind,L"EDIT")?WS_EX_CLIENTEDGE:0,kind,caption,WS_CHILD|WS_VISIBLE|style,
        app_px(x),app_px(y)-(e->building_fields?app_px(124):0),app_px(w),app_px(h),e->building_fields?e->fields:e->hwnd,(HMENU)(INT_PTR)id,GetModuleHandleW(NULL),NULL);
    if(id>0&&id<=ID_FIELDS)e->controls[id]=c;
    SendMessageW(c,WM_SETFONT,(WPARAM)app_font(),TRUE);if(!wcscmp(kind,L"BUTTON"))SetWindowTheme(c,L"DarkMode_Explorer",NULL);return c;
}
static void ownership_controls(editor *e)
{
    wchar_t title[100];swprintf(title,100,L"%u selected item%ls. Owner/group details follow the selected row.",e->count,e->count==1?L"":L"s");
    child(e,L"STATIC",title,0,0,18,16,724,24);
    child(e,L"STATIC",e->root?L"WSL user: root":L"WSL user: distro default",0,ID_ROLE,18,45,724,22);
    child(e,L"STATIC",L"Owner: reading...",0,ID_OWNER,18,72,724,22);child(e,L"STATIC",L"Group: reading...",0,ID_GROUP,18,98,724,22);
    child(e,L"STATIC",L"New owner",0,0,18,136,95,24);child(e,L"STATIC",L"New group",0,0,390,136,95,24);
    HWND u=child(e,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,ID_UID,122,130,235,28);
    HWND g=child(e,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,ID_GID,500,130,235,28);
    SendMessageW(u,EM_LIMITTEXT,512,0);SendMessageW(g,EM_LIMITTEXT,512,0);
    edit_proc=(WNDPROC)SetWindowLongPtrW(u,GWLP_WNDPROC,(LONG_PTR)octal_proc);SetWindowLongPtrW(g,GWLP_WNDPROC,(LONG_PTR)octal_proc);
    child(e,L"STATIC",L"Enter a Linux name or decimal ID. Leave either field blank to keep each item's current value.",0,0,18,172,724,30);
    child(e,L"BUTTON",L"Review changes",WS_TABSTOP,ID_REVIEW,18,211,140,30);
    child(e,L"STATIC",L"",0,ID_RESOLVED,178,208,564,44);child(e,L"STATIC",L"",0,ID_PREVIEW,18,256,724,23);
    child(e,L"STATIC",L"Ownership changes may clear setuid/setgid and file capabilities. Permissions are never restored automatically. Only selected items change; folder contents keep their ownership.",0,0,18,287,724,35);
    LOGFONTW font={0};GetObjectW(app_font(),sizeof font,&font);wcscpy(font.lfFaceName,L"Consolas");font.lfHeight=-app_px(12);e->list_font=CreateFontIndirectW(&font);
    HWND header=child(e,L"STATIC",L"Item               | Owner UID    | Group GID    | Mode / side effects    | Result",0,0,18,332,724,22);
    HWND list=child(e,L"LISTBOX",L"",LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|WS_VSCROLL|WS_HSCROLL|WS_TABSTOP,ID_RESULTS,18,357,724,142);
    if(e->list_font){SendMessageW(list,WM_SETFONT,(WPARAM)e->list_font,TRUE);SendMessageW(header,WM_SETFONT,(WPARAM)e->list_font,TRUE);}
    SendMessageW(list,LB_SETHORIZONTALEXTENT,app_px(1500),0);
    child(e,L"STATIC",L"",SS_PATHELLIPSIS,ID_PATH,18,507,724,23);child(e,L"STATIC",L"",0,ID_STATUS,18,536,724,45);
    child(e,L"BUTTON",L"Reload",WS_TABSTOP,ID_RELOAD,18,594,95,30);child(e,L"BUTTON",L"Reset changes",WS_TABSTOP,ID_RESET,125,594,125,30);
    child(e,L"BUTTON",L"Permissions...",WS_TABSTOP,ID_BACK,262,594,140,30);
    child(e,L"BUTTON",L"Stop",WS_TABSTOP,ID_STOP,415,594,95,30);child(e,L"BUTTON",L"Apply",WS_TABSTOP,ID_APPLY,530,594,95,30);child(e,L"BUTTON",L"Close",WS_TABSTOP,IDCANCEL,647,594,95,30);
    SetPropW(e->hwnd,L"Permissions.ItemCount",(HANDLE)(UINT_PTR)e->count);SetPropW(e->hwnd,L"Permissions.Ownership",(HANDLE)1);start(e,REQUEST_READ);
}
static LRESULT CALLBACK editor_proc(HWND h,UINT m,WPARAM w,LPARAM l)
{
    editor *e=(editor*)GetWindowLongPtrW(h,GWLP_USERDATA);
    if(m==WM_NCCREATE){e=((CREATESTRUCTW*)l)->lpCreateParams;e->hwnd=h;SetWindowLongPtrW(h,GWLP_USERDATA,(LONG_PTR)e);}
    if(!e)return DefWindowProcW(h,m,w,l);
    switch(m){
    case WM_CREATE:{
        if(e->ownership){ownership_controls(e);return 0;}
        int multi=e->count>1;wchar_t title[80];swprintf(title,80,L"%u selected items. Owner/group details follow the selected row.",e->count);
        child(e,L"STATIC",multi?title:e->paths[0],SS_PATHELLIPSIS,0,18,16,multi?724:510,24);
        child(e,L"STATIC",e->root?L"WSL user: root":L"WSL user: distro default",0,ID_ROLE,18,45,510,22);
        child(e,L"STATIC",L"Owner: reading...",0,ID_OWNER,18,72,multi?724:510,22);
        child(e,L"STATIC",L"Group: reading...",0,ID_GROUP,18,98,multi?724:510,22);
        e->fields=CreateWindowExW(WS_EX_CONTROLPARENT,L"OpenPilotPermissionFields",L"Permission choices",
            WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN,0,app_px(124),app_px(multi?760:546),app_px(204),h,(HMENU)(INT_PTR)ID_FIELDS,GetModuleHandleW(NULL),NULL);
        if(!e->fields)return -1;
        e->controls[ID_FIELDS]=e->fields;e->building_fields=1;
        child(e,L"STATIC",L"Read",0,0,158,132,80,22);child(e,L"STATIC",L"Write",0,0,273,132,80,22);child(e,L"STATIC",L"Execute",0,0,388,132,90,22);
        for(int row=0;row<3;row++){
            child(e,L"STATIC",row==0?L"Owner":row==1?L"Group":L"Others",0,0,18,160+row*29,100,23);
            for(int col=0;col<3;col++)child(e,L"BUTTON",L"",(multi?BS_3STATE:BS_AUTOCHECKBOX)|WS_TABSTOP,ID_BIT+row*3+col,158+col*115,158+row*29,80,24);
        }
        DWORD check=(multi?BS_3STATE:BS_AUTOCHECKBOX)|WS_TABSTOP;
        child(e,L"BUTTON",L"Setuid",check,ID_BIT+9,18,250,150,24);child(e,L"BUTTON",L"Setgid",check,ID_BIT+10,195,250,150,24);child(e,L"BUTTON",L"Sticky",check,ID_BIT+11,372,250,150,24);
        child(e,L"STATIC",multi?L"Exact octal":L"Octal",0,0,18,287,multi?98:60,24);
        HWND edit=child(e,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,ID_MODE,multi?122:80,282,95,28);
        SendMessageW(edit,EM_LIMITTEXT,12,0);edit_proc=(WNDPROC)SetWindowLongPtrW(edit,GWLP_WNDPROC,(LONG_PTR)octal_proc);
        child(e,L"STATIC",L"Read permissions before editing.",0,ID_PREVIEW,multi?240:195,280,multi?502:330,44);
        e->building_fields=0;
        child(e,L"STATIC",multi?L"Changes affect selected items only; folder contents keep their modes.":L"Changes affect this item only; folder contents keep their modes.",0,0,18,335,multi?724:510,22);
        if(multi){
            child(e,L"STATIC",L"Mixed keeps each item's original value. Click to set, clear or keep. Exact octal sets all bits.",0,0,18,364,724,35);
            LOGFONTW font={0};GetObjectW(app_font(),sizeof font,&font);wcscpy(font.lfFaceName,L"Consolas");font.lfHeight=-app_px(12);e->list_font=CreateFontIndirectW(&font);
            HWND header=child(e,L"STATIC",L"Item                            | Read | After | Result",0,0,18,405,724,22);
            HWND list=child(e,L"LISTBOX",L"",LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|WS_VSCROLL|WS_HSCROLL|WS_TABSTOP,ID_RESULTS,18,432,724,153);
            if(e->list_font){SendMessageW(list,WM_SETFONT,(WPARAM)e->list_font,TRUE);SendMessageW(header,WM_SETFONT,(WPARAM)e->list_font,TRUE);}
            SendMessageW(list,LB_SETHORIZONTALEXTENT,app_px(950),0);
            child(e,L"STATIC",L"",SS_PATHELLIPSIS,ID_PATH,18,593,724,23);
            child(e,L"STATIC",L"",0,ID_STATUS,18,623,724,45);
            child(e,L"BUTTON",L"Reload",WS_TABSTOP,ID_RELOAD,18,680,95,30);child(e,L"BUTTON",L"Reset changes",WS_TABSTOP,ID_RESET,125,680,125,30);
            child(e,L"BUTTON",L"Owner / group...",WS_TABSTOP,ID_OWNERSHIP,262,680,140,30);
            child(e,L"BUTTON",L"Stop",WS_TABSTOP,ID_STOP,415,680,95,30);child(e,L"BUTTON",L"Apply",WS_TABSTOP,ID_APPLY,530,680,95,30);child(e,L"BUTTON",L"Close",WS_TABSTOP,IDCANCEL,647,680,95,30);
        }else{
            child(e,L"STATIC",L"",0,ID_STATUS,18,365,510,45);child(e,L"BUTTON",L"Reload",WS_TABSTOP,ID_RELOAD,18,420,95,30);
            child(e,L"BUTTON",L"Owner / group...",WS_TABSTOP,ID_OWNERSHIP,125,420,180,30);
            child(e,L"BUTTON",L"Apply",WS_TABSTOP,ID_APPLY,325,420,95,30);child(e,L"BUTTON",L"Close",WS_TABSTOP,IDCANCEL,433,420,95,30);
        }
        SetPropW(h,L"Permissions.ItemCount",(HANDLE)(UINT_PTR)e->count);start(e,0);return 0;
    }
    case WM_COMMAND:{
        int id=LOWORD(w),code=HIWORD(w);
        if(id==IDCANCEL){SendMessageW(h,WM_CLOSE,0,0);return 0;}
        if(id==ID_STOP&&e->busy){InterlockedExchange(&e->stop,1);enable_control(control(e,ID_STOP),FALSE);text(e,ID_STATUS,L"Finishing the current item, then stopping. Completed changes remain applied.");return 0;}
        if(id==ID_RESULTS&&code==LBN_SELCHANGE){details(e);return 0;}
        if(id==ID_APPLY&&code==BN_CLICKED){start(e,e->ownership?REQUEST_OWNER:REQUEST_MODE);return 0;}
        if(id==ID_RELOAD&&code==BN_CLICKED){start(e,0);return 0;}
        if(e->busy||e->sync)return 0;
        if(id==ID_OWNERSHIP||id==ID_BACK){
            if(id==ID_OWNERSHIP&&!ready_count(e))return 0;
            if(!e->ownership&&(!input_valid(e)||change_count(e))){text(e,ID_STATUS,L"Apply or reset permission changes before editing ownership.");return 0;}
            if(e->ownership&&change_count(e)){text(e,ID_STATUS,L"Apply or reset ownership changes before returning to permissions.");return 0;}
            HWND next=open_editor(e->pane,e->owner,e->paths,e->count,e->root,!e->ownership);
            if(next){DestroyWindow(h);SetForegroundWindow(next);}return 0;
        }
        if(!ready_count(e))return 0;
        if(e->ownership){
            if(id==ID_REVIEW&&code==BN_CLICKED){start(e,REQUEST_REVIEW);return 0;}
            if((id==ID_UID||id==ID_GID)&&code==EN_CHANGE){e->reviewed=0;text(e,ID_STATUS,L"Draft changed. Review names and IDs before Apply.");preview(e);return 0;}
            if(id==ID_RESET){e->identity=(fpe_identity){0};show_state(e);text(e,ID_STATUS,L"Ownership draft cleared.");preview(e);return 0;}
            return 0;
        }
        if(id==ID_RESET){e->patch=(permission_patch){0};show_state(e);preview(e);return 0;}
        if(id==ID_MODE&&code==EN_CHANGE){
            uint32_t mode=0;wchar_t value[16];GetWindowTextW(control(e,ID_MODE),value,16);
            if(permission_octal(value,&mode)){e->patch=(permission_patch){07777,mode};checks(e);}
            else if(e->count>1&&!value[0]){e->patch=(permission_patch){0};checks(e);}
            preview(e);return 0;
        }
        if(id>=ID_BIT&&id<ID_BIT+12&&code==BN_CLICKED){
            if(e->count>1){
                permission_patch_cycle(&e->patch,bits[id-ID_BIT],original_bit(e,bits[id-ID_BIT]));
                e->sync=1;text(e,ID_MODE,L"");e->sync=0;checks(e);preview(e);
            }else{
                uint32_t mode=0;for(int i=0;i<12;i++)if(SendMessageW(control(e,ID_BIT+i),BM_GETCHECK,0,0)==BST_CHECKED)mode|=bits[i];
                wchar_t s[16];swprintf(s,16,L"%04o",mode);text(e,ID_MODE,s);
            }
            return 0;
        }
        return 0;
    }
    case PM_PROGRESS:{
        SetPropW(h,L"Permissions.Completed",(HANDLE)(UINT_PTR)w);
        if(!e->stop&&(e->count>1||e->ownership)){wchar_t s[120];swprintf(s,120,L"%ls %u of %u...",l?(e->ownership?L"Applying ownership to":L"Applying permissions to"):L"Reading",(unsigned)w,e->count);text(e,ID_STATUS,s);}return 0;
    }
    case PM_RESULT:{
        request *r=(request*)l;e->busy=0;
        if(r->apply==REQUEST_REVIEW){
            e->reviewed=!r->error&&!r->stopped;e->identity=r->identity;
            wchar_t message[220];
            if(r->stopped)wcscpy(message,L"Review stopped. No ownership changes applied.");
            else if(!r->error)wcscpy(message,L"Names and IDs resolved. Review each item's proposed ownership, then Apply.");
            else if(r->bad_field)swprintf(message,220,L"%ls could not be resolved. Use an existing name or decimal ID from 0 to 4294967294.",r->bad_field==FPE_ID_USER?L"Owner":L"Group");
            else wcscpy(message,L"Could not resolve ownership through the native WSL helper. Review again before Apply.");
            SetPropW(h,L"Permissions.Error",(HANDLE)(UINT_PTR)r->error);text(e,ID_STATUS,message);controls(e);request_free(r);return 0;
        }
        memcpy(e->items,r->items,e->count*sizeof *e->items);e->patch=(permission_patch){0};
        uint32_t applied=0,failed=0,unchanged=0,stopped=0,unverified=0;DWORD first_error=0;
        for(uint32_t i=0;i<e->count;i++){
            item *it=&e->items[i];applied+=it->state==ITEM_APPLIED;failed+=it->state==ITEM_FAILED;
            unchanged+=it->state==ITEM_UNCHANGED;stopped+=it->state==ITEM_STOPPED;if(!first_error&&it->error)first_error=it->error;
            unverified+=it->state==ITEM_FAILED&&it->changed;
        }
        SetPropW(h,L"Permissions.Error",(HANDLE)(UINT_PTR)first_error);SetPropW(h,L"Permissions.Completed",(HANDLE)(UINT_PTR)r->completed);
        SetPropW(h,L"Permissions.Applied",(HANDLE)(UINT_PTR)applied);SetPropW(h,L"Permissions.Failed",(HANDLE)(UINT_PTR)failed);SetPropW(h,L"Permissions.Stopped",(HANDLE)(UINT_PTR)stopped);
        SetPropW(h,L"Permissions.Unverified",(HANDLE)(UINT_PTR)unverified);
        show_state(e);
        wchar_t message[256];
        if(e->count==1&&!e->ownership){
            item *it=&e->items[0];
            if(!it->error)wcscpy(message,r->apply?L"Permissions applied and verified.":L"Choose permissions, review the preview, then Apply.");
            else if(it->error==ERROR_ACCESS_DENIED)wcscpy(message,L"Permission denied. The selected WSL user cannot access or change this item.");
            else if(it->error==ERROR_FILE_INVALID)wcscpy(message,L"This item changed or was replaced. Reload before applying.");
            else if(it->changed)wcscpy(message,L"The filesystem did not retain the requested permissions. Reload to inspect the current mode.");
            else if(it->error==ERROR_NOT_SUPPORTED)wcscpy(message,L"Unavailable: requires a Linux file or folder and the native WSL helper. Symlinks and special files are excluded.");
            else if(it->error==ERROR_PATH_NOT_FOUND||it->error==ERROR_FILE_NOT_FOUND)wcscpy(message,L"This file or folder is no longer available. Reload to check again.");
            else wcscpy(message,L"Could not read a complete reply from WSL. Reload to check the current permissions before retrying.");
        }else if(r->apply)swprintf(message,256,L"%u applied and verified; %u failed; %u unchanged; %u not attempted.\n%u reported changes need Reload.%ls",applied,failed,unchanged,stopped,unverified,r->stopped?L" Stopped after the current item.":L"");
        else swprintf(message,256,L"%u of %u items available; %u unavailable; %u not read.%ls",ready_count(e),e->count,failed,stopped,r->stopped?L" Read stopped. Reload to read all items.":L" Review the preview, then Apply.");
        text(e,ID_STATUS,message);
        if(r->apply){app_refresh(e->pane);Pane *other=app_other_pane(e->pane);if(other)app_refresh(other);app_status(e->pane,first_error,e->count==1&&!e->ownership?(first_error?L"Permissions: check the editor for details":L"Permissions applied and verified"):(e->ownership?L"Ownership: %u applied and verified, %u failed, %u not attempted":L"Permissions: %u applied and verified, %u failed, %u not attempted"),applied,failed,stopped);}
        controls(e);request_free(r);return 0;
    }
    case WM_CLOSE:
        if(e->busy){text(e,ID_STATUS,(e->count>1||e->ownership)?L"Use Stop to finish the current item, then close when the batch returns.":L"Wait for the permission request to finish before closing.");return 0;}
        DestroyWindow(h);return 0;
    case WM_CTLCOLORSTATIC:case WM_CTLCOLORBTN:case WM_CTLCOLOREDIT:case WM_CTLCOLORLISTBOX:{
        int dark=m==WM_CTLCOLOREDIT||m==WM_CTLCOLORLISTBOX;SetTextColor((HDC)w,C_TEXT);SetBkColor((HDC)w,dark?C_EDIT:C_BG);return (LRESULT)(dark?e->edit:e->bg);
    }
    case WM_ERASEBKGND:{RECT r;GetClientRect(h,&r);FillRect((HDC)w,&r,e->bg);return 1;}
    case WM_DESTROY:{
        InterlockedExchange(&e->stop,1);EnterCriticalSection(&e->lock);e->hwnd=NULL;LeaveCriticalSection(&e->lock);
        MSG msg;while(PeekMessageW(&msg,h,PM_RESULT,PM_PROGRESS,PM_REMOVE)){
            if(msg.message==WM_QUIT){PostQuitMessage((int)msg.wParam);break;}if(msg.message==PM_RESULT)request_free((request*)msg.lParam);
        }
        const wchar_t *props[]={L"Permissions.Busy",L"Permissions.Loaded",L"Permissions.Error",L"Permissions.ReadyCount",L"Permissions.ItemCount",L"Permissions.Completed",L"Permissions.Applied",L"Permissions.Failed",L"Permissions.Stopped",L"Permissions.Unverified",L"Permissions.Reviewed",L"Permissions.Ownership"};
        for(size_t i=0;i<sizeof props/sizeof *props;i++)RemovePropW(h,props[i]);
        if(window==h)window=NULL;SetWindowLongPtrW(h,GWLP_USERDATA,0);fn_release(e->owner);release(e);return 0;
    }
    }
    return DefWindowProcW(h,m,w,l);
}
/* Sent WM_COMMAND messages bypass the explorer's dispatch profiler. Keep
 * their native handler spans separately from the caller's round trip. Close
 * destroys this window, so retain its last/worst identity on the owner. */
static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l)
{
    if(m!=WM_COMMAND)return editor_proc(h,m,w,l);
    int64_t began=fpe_prof_begin();
    HWND owner=began?GetWindow(h,GW_OWNER):NULL;
    LRESULT result=editor_proc(h,m,w,l);
    if(!began)return result;
    LARGE_INTEGER now,freq;QueryPerformanceCounter(&now);QueryPerformanceFrequency(&freq);
    uintptr_t us=(uintptr_t)((now.QuadPart-began)*1000000/freq.QuadPart);
    switch(LOWORD(w)){
    case ID_APPLY:FPE_PROF_END("permissions.command.apply",began);break;
    case ID_RELOAD:FPE_PROF_END("permissions.command.reload",began);break;
    case IDCANCEL:FPE_PROF_END("permissions.command.close",began);break;
    default:FPE_PROF_END("permissions.command.other",began);break;
    }
    if(owner){
        SetPropW(owner,L"Permissions.Command.LastId",(HANDLE)(uintptr_t)LOWORD(w));
        SetPropW(owner,L"Permissions.Command.LastUs",(HANDLE)us);
        if(us>=(uintptr_t)GetPropW(owner,L"Permissions.Command.MaxUs")){
            SetPropW(owner,L"Permissions.Command.MaxId",(HANDLE)(uintptr_t)LOWORD(w));
            SetPropW(owner,L"Permissions.Command.MaxUs",(HANDLE)us);
        }
    }
    return result;
}
int permissions_applies(const fn_ctx *c){return c->n&&c->n<=PERMISSION_MAX_ITEMS&&c->dir&&fpe_wsl_native(c->dir);}
static HWND open_editor(Pane *pane,const fn_def *owner,wchar_t **paths,uint32_t count,int root,int ownership)
{
    editor *e=calloc(1,sizeof *e);if(!e)return NULL;
    e->pane=pane;e->owner=owner;e->refs=1;e->count=count;e->root=root;e->ownership=ownership;InitializeCriticalSection(&e->lock);
    e->paths=calloc(e->count,sizeof *e->paths);e->items=calloc(e->count,sizeof *e->items);
    if(!e->paths||!e->items){e->count=0;release(e);return NULL;}
    for(uint32_t i=0;i<e->count;i++)if(!(e->paths[i]=_wcsdup(paths[i]))){release(e);return NULL;}
    e->bg=CreateSolidBrush(C_BG);e->edit=CreateSolidBrush(C_EDIT);
    if(fn_hold(e->owner)){release(e);return NULL;}
    WNDCLASSW cls={.lpfnWndProc=proc,.hInstance=GetModuleHandleW(NULL),.hCursor=LoadCursorW(NULL,IDC_ARROW),.lpszClassName=L"OpenPilotPermissions"};RegisterClassW(&cls);
    WNDCLASSW fields={.lpfnWndProc=fields_proc,.hInstance=cls.hInstance,.hCursor=cls.hCursor,.lpszClassName=L"OpenPilotPermissionFields"};RegisterClassW(&fields);
    RECT r={0,0,app_px(ownership||count>1?760:546),app_px(ownership?640:count>1?728:468)},parent;
    AdjustWindowRectEx(&r,WS_CAPTION|WS_SYSMENU,0,WS_EX_CONTROLPARENT);GetWindowRect(app_window(),&parent);
    HWND opened=CreateWindowExW(WS_EX_CONTROLPARENT,cls.lpszClassName,ownership?L"Linux owner/group":L"Linux permissions",WS_CAPTION|WS_SYSMENU,
        parent.left+(parent.right-parent.left-(r.right-r.left))/2,parent.top+(parent.bottom-parent.top-(r.bottom-r.top))/2,
        r.right-r.left,r.bottom-r.top,app_window(),NULL,cls.hInstance,e);
    if(!opened){fn_release(e->owner);release(e);return NULL;}
    window=opened;ShowWindow(opened,SW_SHOW);SetForegroundWindow(opened);SetFocus(control(e,ownership?ID_UID:ID_MODE));return opened;
}
void permissions_run(fn_ctx *c)
{
    if(window){SetForegroundWindow(window);return;}
    wchar_t **paths=fn_paths(c);if(!paths||!c->n||c->n>PERMISSION_MAX_ITEMS)return;
    int root=!_stricmp(fn_option(fn_find("wsl.helper"),"user"),"root");
    open_editor(c->pane,fn_self(),paths,c->n,root,0);
}
int permissions_message(MSG *msg){return window&&(msg->hwnd==window||IsChild(window,msg->hwnd))&&IsDialogMessageW(window,msg);}
void permissions_shutdown(void){if(window)DestroyWindow(window);}
