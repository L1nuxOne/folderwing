/* Native desktop integration using the production window handlers and OLE
 * drop target. Including the host gives the test visibility into ownership;
 * the shipped app contains no test messages, endpoints or input hooks. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include "../app/sidepanel.h"
#include "../app/panelproto.h"
#include "../app/shell.h"
/* A bounded fixture observes dispatch without entering the modal OLE loop.
 * Normal/full drag acceptance still calls the real shell outside these cases. */
static int gesture_observe,gesture_calls,gesture_right;
static int gesture_modal_trace;
static uint32_t gesture_count;
static wchar_t gesture_paths[4][MAX_PATH+100];
static DWORD gesture_drag_out(HWND hwnd,const wchar_t *const *paths,uint32_t n,int right)
{
    if(!gesture_observe)return shell_drag_out(hwnd,paths,n,right);
    gesture_calls++;gesture_count=n;gesture_right=right;
    for(uint32_t i=0;i<n&&i<4;i++){wcsncpy(gesture_paths[i],paths[i],MAX_PATH+99);gesture_paths[i][MAX_PATH+99]=0;}
    return 0; /* DROPEFFECT_NONE; OLE constants are included by the host below. */
}
/* Registration faults and call counts stay in this native harness. */
static int drop_observe,drop_calls,drop_revokes,drop_fail_at;
static HWND drop_order[8];
static int drag_register_drop(HWND hwnd,const shell_drop_handler *handler,void *context)
{
    if(drop_observe){
        if(drop_calls<8)drop_order[drop_calls]=hwnd;
        drop_calls++;
        if(drop_calls==drop_fail_at)return -1;
    }
    return shell_drop_register(hwnd,handler,context);
}
static void drag_revoke_drop(HWND hwnd)
{
    if(drop_observe)drop_revokes++;
    shell_drop_revoke(hwnd);
}
/* Only this harness holds/fails a real durable write. Production has no hooks. */
static HANDLE save_flush_entered, save_flush_release;
static volatile LONG save_flush_action;
static DWORD save_flush_thread;
static HANDLE race_entered, race_release;
static BOOL drag_replace_file(LPCWSTR old, LPCWSTR replacement, LPCWSTR backup,
                              DWORD flags, LPVOID exclude, LPVOID reserved)
{
    if (race_entered) {
        SetEvent(race_entered);
        if (WaitForSingleObject(race_release, 10000) != WAIT_OBJECT_0) {
            SetLastError(ERROR_TIMEOUT);
            return FALSE;
        }
    }
    return ReplaceFileW(old, replacement, backup, flags, exclude, reserved);
}
static int save_observe_replies, save_replies;
static int save_fail_start;
static int clipboard_fail_empty;
static UINT clipboard_fail_format;
static HGLOBAL clipboard_track[2];
static int clipboard_freed[2];
static HGLOBAL drag_global_free(HGLOBAL h)
{
    for (int i=0;i<2;i++) if (h && h==clipboard_track[i]) clipboard_freed[i]++;
    return GlobalFree(h);
}
static BOOL drag_empty_clipboard(void)
{
    if (clipboard_fail_empty) { clipboard_fail_empty=0; SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return EmptyClipboard();
}
static HANDLE drag_set_clipboard(UINT format, HANDLE h)
{
    if (format==clipboard_fail_format) { clipboard_fail_format=0; SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    return SetClipboardData(format,h);
}
static DWORD WINAPI settings_commit(void *arg);
static HANDLE drag_save_thread(LPSECURITY_ATTRIBUTES security, SIZE_T stack, LPTHREAD_START_ROUTINE start,
                               LPVOID arg, DWORD flags, LPDWORD id)
{
    if (save_fail_start && start == settings_commit) {
        save_fail_start=0;SetLastError(ERROR_NOT_ENOUGH_MEMORY);return NULL;
    }
    return CreateThread(security,stack,start,arg,flags,id);
}
static BOOL drag_flush(HANDLE file)
{
    LONG action = InterlockedExchange(&save_flush_action, 0);
    if (action) save_flush_thread = GetCurrentThreadId();
    if (action == 1) {
        SetEvent(save_flush_entered);
        if (WaitForSingleObject(save_flush_release, 5000) != WAIT_OBJECT_0) {
            SetLastError(ERROR_TIMEOUT); return FALSE;
        }
    }
    if (action == 2) { SetLastError(ERROR_WRITE_FAULT); return FALSE; }
    return FlushFileBuffers(file);
}
static int drag_panel_send(const wchar_t *app, uint32_t type, const void *data, uint32_t size)
{
    if (save_observe_replies && type == PANELP_SAVED) save_replies++;
    return sidepanel_send(app, type, data, size);
}
#define FlushFileBuffers drag_flush
#define ReplaceFileW drag_replace_file
#define sidepanel_send drag_panel_send
#define CreateThread drag_save_thread
#define GlobalFree drag_global_free
#define EmptyClipboard drag_empty_clipboard
#define SetClipboardData drag_set_clipboard
#define shell_drop_register drag_register_drop
#define shell_drop_revoke drag_revoke_drop
#define shell_drag_out gesture_drag_out
#define wWinMain embedded_openpilot_main
#include "../app/fpdemo.c"
#include "../app/everything.h"
#undef wWinMain
#undef shell_drag_out
#undef sidepanel_send
#undef FlushFileBuffers
#undef ReplaceFileW
#undef CreateThread
#undef GlobalFree
#undef EmptyClipboard
#undef SetClipboardData
#undef shell_drop_register
#undef shell_drop_revoke

/* Wrap only the source-included shell clipboard boundary. All other shell
 * integration stays real; no aliases from the host include remain active. */
static struct {
    int mode,opened,busy,fail_open,fail_empty,fail_get;
    unsigned allocs,locks,sets,fail_alloc,fail_lock,fail_sets,count,bad_free,early_success;
    HANDLE unicode,handles[8];int owned[8];
} path_clip;
static HGLOBAL WINAPI path_clip_alloc(UINT flags,SIZE_T size)
{
    if(path_clip.mode&&++path_clip.allocs==path_clip.fail_alloc)return NULL;
    HGLOBAL h=GlobalAlloc(flags,size);if(path_clip.mode&&h&&path_clip.count<8){unsigned i=path_clip.count++;path_clip.handles[i]=h;path_clip.owned[i]=1;}return h;
}
static LPVOID WINAPI path_clip_lock(HGLOBAL h){if(path_clip.mode&&++path_clip.locks==path_clip.fail_lock)return NULL;return GlobalLock(h);}
static HGLOBAL WINAPI path_clip_free(HGLOBAL h)
{
    if(path_clip.mode)for(unsigned i=0;i<path_clip.count;i++)if(path_clip.handles[i]==h&&!path_clip.owned[i])path_clip.bad_free++;
    HGLOBAL left=GlobalFree(h);if(path_clip.mode&&!left)for(unsigned i=0;i<path_clip.count;i++)if(path_clip.handles[i]==h)path_clip.owned[i]=0;return left;
}
static BOOL WINAPI path_clip_open(HWND h){if(path_clip.mode&&path_clip.fail_open)return FALSE;if(path_clip.mode==1){if(path_clip.busy||path_clip.opened)return FALSE;path_clip.opened=1;return TRUE;}return OpenClipboard(h);}
static BOOL WINAPI path_clip_close(void){if(path_clip.mode==1){int was=path_clip.opened;path_clip.opened=0;return was;}return CloseClipboard();}
static BOOL WINAPI path_clip_available(UINT format){return path_clip.mode==1?(format==CF_UNICODETEXT&&path_clip.unicode!=NULL):IsClipboardFormatAvailable(format);}
static HANDLE WINAPI path_clip_get(UINT format){if(path_clip.mode&&path_clip.fail_get)return NULL;return path_clip.mode==1?(format==CF_UNICODETEXT?path_clip.unicode:NULL):GetClipboardData(format);}
static BOOL WINAPI path_clip_empty(void)
{
    if(path_clip.mode&&path_clip.fail_empty)return FALSE;
    if(path_clip.mode==1){if(!path_clip.opened)return FALSE;if(path_clip.unicode)GlobalFree(path_clip.unicode);path_clip.unicode=NULL;return TRUE;}return EmptyClipboard();
}
static HANDLE WINAPI path_clip_set(UINT format,HANDLE h)
{
    if(path_clip.mode){unsigned call=++path_clip.sets;if(wcsstr(g.panes[0].status,L"copied"))path_clip.early_success++;if(path_clip.fail_sets&(1u<<(call-1)))return NULL;}
    HANDLE accepted;if(path_clip.mode==1){accepted=path_clip.opened&&format==CF_UNICODETEXT?h:NULL;if(accepted)path_clip.unicode=h;}else accepted=SetClipboardData(format,h);
    if(path_clip.mode&&accepted)for(unsigned i=0;i<path_clip.count;i++)if(path_clip.handles[i]==h)path_clip.owned[i]=0;return accepted;
}
#define GlobalAlloc path_clip_alloc
#define GlobalLock path_clip_lock
#define GlobalFree path_clip_free
#define OpenClipboard path_clip_open
#define CloseClipboard path_clip_close
#define IsClipboardFormatAvailable path_clip_available
#define GetClipboardData path_clip_get
#define EmptyClipboard path_clip_empty
#define SetClipboardData path_clip_set
#include "../app/shell.c"
#undef GlobalAlloc
#undef GlobalLock
#undef GlobalFree
#undef OpenClipboard
#undef CloseClipboard
#undef IsClipboardFormatAvailable
#undef GetClipboardData
#undef EmptyClipboard
#undef SetClipboardData

static int drag_passed,drag_failed;
static fpe_job_progress gesture_modal_job;
static int menu_context_calls;
static void menu_context_probe(fn_ctx *c) { (void)c; menu_context_calls++; }
static const fn_def menu_context_function={.id="test.menu_context",.title=L"Menu context probe",
    .life=FN_COLD,.where=FN_PANE,.requires=FN_S_FOLDER,.menus=FN_MENU_FOLDER,.run=menu_context_probe};
#define VERIFY(label, condition) do { if(condition){drag_passed++;printf("  ok    %s\n",label);} \
    else{drag_failed++;printf("  FAIL  %s (line %d)\n",label,__LINE__);} fflush(stdout); } while(0)
static wchar_t drag_root[MAX_PATH+80],drag_left[MAX_PATH+100],drag_right[MAX_PATH+100];
static const char drag_settings[]="[functions]\npreview.image = off\npreview.pdf = off\npreview.text = off\noperations.show = off\nsvc.icons = off\n[panel.side]\nslide = off\n";
/* Observe the real owned message pump; never call clipboard_publish here. */
static int clipboard_expiry_trace;
static unsigned clipboard_timer_seen;
static uint64_t clipboard_timer_last;
static void clipboard_expiry_fact(const char *phase)
{
    printf("CLIPBOARD_EXPIRY phase=%s tick=%llu request=%llu data=%p extra=%p owner=%p owner_live=%d until=%llu request_sequence=%lu actual_sequence=%lu holder=%p actual_owner=%p timer_dispatches=%u last_timer=%llu queue=%lu\n",
        phase,(unsigned long long)GetTickCount64(),(unsigned long long)g_clipboard.request,g_clipboard.data,g_clipboard.extra,g_clipboard.owner,IsWindow(g_clipboard.owner),
        (unsigned long long)g_clipboard.until,g_clipboard.sequence,GetClipboardSequenceNumber(),GetOpenClipboardWindow(),GetClipboardOwner(),clipboard_timer_seen,(unsigned long long)clipboard_timer_last,(unsigned long)GetInputState());fflush(stdout);
}
static void drag_pump(int ms)
{
    uint64_t until=GetTickCount64()+ms;MSG m;
    do{
        HANDLE warm_wait=dropwarm_wait_handle();
        if(warm_wait&&WaitForSingleObject(warm_wait,0)==WAIT_OBJECT_0){MSG done={.message=WM_APP_DROP_WARM};dispatch_timed(&done);}
        while(PeekMessageW(&m,NULL,0,0,PM_REMOVE))if(m.message!=WM_QUIT){
            int observed=clipboard_expiry_trace&&m.hwnd==g.hwnd&&m.message==WM_TIMER&&m.wParam==TIMER_CLIPBOARD;
            if(observed){clipboard_timer_seen++;clipboard_timer_last=GetTickCount64();clipboard_expiry_fact("before-real-WM_TIMER");}
            if(gesture_modal_trace&&m.hwnd==g.hwnd&&m.message==WM_APP_JOB){
                const job_msg *j=(const job_msg *)m.lParam;
                if(j&&!j->diagnostic){gesture_modal_job=j->p;gesture_modal_job.current=NULL;}
                if(j&&!j->diagnostic)printf("N11_JOB state=%u errors=%u last_error=%u items_done=%u items_total=%u current=%ls\n",j->p.state,j->p.errors,j->p.last_error,j->p.items_done,j->p.items_total,j->p.current?j->p.current:L"<none>");
            }
            TranslateMessage(&m);dispatch_timed(&m);if(observed)clipboard_expiry_fact("after-real-WM_TIMER");
        }
        Sleep(2);
    }while(GetTickCount64()<until);
}
static void drag_settle(Pane *p)
{
    uint64_t until=GetTickCount64()+5000;
    while(p->nav_ticket && GetTickCount64()<until)drag_pump(5);
    VERIFY("the shown tab finishes loading",p->nav_ticket==0);
}
static wchar_t *drag_path(const wchar_t *dir,const wchar_t *name){return join(dir,name);}
static void drag_write(const wchar_t *path,const char *value)
{
    HANDLE f=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);DWORD n;
    VERIFY("the private source fixture is created",f!=INVALID_HANDLE_VALUE);
    if(f!=INVALID_HANDLE_VALUE){WriteFile(f,value,(DWORD)strlen(value),&n,NULL);CloseHandle(f);}
}
static int drag_exists(const wchar_t *path){return GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES;}
static int drag_wait_file(const wchar_t *path,int exists)
{
    uint64_t until=GetTickCount64()+5000;
    while(drag_exists(path)!=exists && GetTickCount64()<until)drag_pump(10);
    return drag_exists(path)==exists;
}
static void drag_mouse(HWND h,UINT message,WPARAM keys,int x,int y)
{
    SendMessageW(h,message,keys,MAKELPARAM((short)x,(short)y));
}
static void drag_tab(Pane *src,int index,Pane *dst,int slot,int escape)
{
    int width=tab_width(src);int x=g.pad+index*width+(width<S(48)?width/3:S(24)),y=S(12);
    drag_mouse(src->hwnd,WM_LBUTTONDOWN,MK_LBUTTON,x,y);
    POINT pt={g.pad+slot*tab_width(dst),dst->ntabs>1?S(12):p_list_top(dst)+S(60)};
    ClientToScreen(dst->hwnd,&pt);ScreenToClient(src->hwnd,&pt);
    drag_mouse(src->hwnd,WM_MOUSEMOVE,MK_LBUTTON,pt.x,pt.y);
    if(escape)SendMessageW(src->hwnd,WM_KEYDOWN,VK_ESCAPE,0);
    drag_mouse(src->hwnd,WM_LBUTTONUP,0,pt.x,pt.y);
    drag_settle(src);if(dst!=src)drag_settle(dst);
}
static int drag_address_fits(Pane *p)
{
    RECT r;GetWindowRect(p->edit,&r);MapWindowPoints(NULL,p->hwnd,(POINT*)&r,2);
    return r.right==p->cw-g.pad-S(6)&&r.left>=0;
}
static int64_t drag_zone_calls(const char *name)
{
    fpe_prof_row rows[512];uint32_t n=fpe_prof_rows(rows,512);
    for(uint32_t i=0;i<n;i++)if(!strcmp(rows[i].name,name))return rows[i].calls;
    return 0;
}
static HANDLE clipboard_held, clipboard_release;
static DWORD WINAPI clipboard_holder(void *arg)
{
    uint64_t until=GetTickCount64()+1000;
    while (!OpenClipboard(NULL)) { if(GetTickCount64()>=until)return 1; Sleep(2); }
    SetEvent(clipboard_held);
    DWORD wait=WaitForSingleObject(clipboard_release,5000);
    CloseClipboard(); return wait==WAIT_OBJECT_0?0:2;
}
static HGLOBAL clipboard_string(const wchar_t *s)
{
    size_t bytes=(wcslen(s)+1)*sizeof *s;
    HGLOBAL h=GlobalAlloc(GMEM_MOVEABLE,bytes);void *p=h?GlobalLock(h):NULL;
    if(!p){if(h)GlobalFree(h);return NULL;}memcpy(p,s,bytes);GlobalUnlock(h);return h;
}
/* A busy read is not evidence of changed bytes. A completed mismatch is final. */
enum { CLIP_READ_BUSY=-1, CLIP_READ_CHANGED=0, CLIP_READ_MATCH=1 };
static int clipboard_probe_text(const wchar_t *s,DWORD *error)
{
    *error=0;
    if(!OpenClipboard(g.hwnd)){*error=GetLastError();return CLIP_READ_BUSY;}
    HGLOBAL h=GetClipboardData(CF_UNICODETEXT);const wchar_t *p=h?GlobalLock(h):NULL;
    int ok=p&&!wcscmp(p,s);if(!p)*error=GetLastError();
    if(p)GlobalUnlock(h);CloseClipboard();return ok?CLIP_READ_MATCH:CLIP_READ_CHANGED;
}
static int clipboard_preserved(const wchar_t *s,DWORD sequence)
{
    uint64_t until=GetTickCount64()+1500;DWORD first_error=0,error=0;unsigned reads=0;
    for(;;){
        if(GetClipboardSequenceNumber()!=sequence)break;
        int state=clipboard_probe_text(s,&error);reads++;
        if(reads==1)first_error=error;
        if(GetClipboardSequenceNumber()!=sequence)break;
        if(state==CLIP_READ_MATCH)return 1;
        if(state!=CLIP_READ_BUSY||GetTickCount64()>=until)break;
        Sleep(2); /* only retry read access; never pump or republish */
    }
    fprintf(stderr,"CLIPBOARD expected_sequence=%lu actual_sequence=%lu first_error=%lu last_error=%lu reads=%u holder=%p owner=%p\n",
        (unsigned long)sequence,(unsigned long)GetClipboardSequenceNumber(),(unsigned long)first_error,
        (unsigned long)error,reads,GetOpenClipboardWindow(),GetClipboardOwner());
    return 0;
}
static int clipboard_is_text(const wchar_t *s)
{
    return clipboard_preserved(s,GetClipboardSequenceNumber());
}
static int clipboard_text_ready(const wchar_t *s)
{
    uint64_t until=GetTickCount64()+1500;
    do { DWORD error;if(clipboard_probe_text(s,&error)==CLIP_READ_MATCH)return 1; drag_pump(5); } while(GetTickCount64()<until);
    return 0;
}
static void clipboard_seed(const wchar_t *s)
{
    uint64_t request=app_clipboard_begin(g.hwnd);
    app_clipboard_submit(request,CF_UNICODETEXT,clipboard_string(s),0,NULL,L"Seeded");
    uint64_t until=GetTickCount64()+1500;
    while(g_clipboard.data&&GetTickCount64()<until)drag_pump(5);
    VERIFY("private clipboard fixture is ready",clipboard_text_ready(s));
}
static HANDLE clipboard_lock(void)
{
    ResetEvent(clipboard_held);ResetEvent(clipboard_release);
    HANDLE thread=CreateThread(NULL,0,clipboard_holder,NULL,0,NULL);
    VERIFY("another thread holds the real clipboard",thread&&WaitForSingleObject(clipboard_held,1500)==WAIT_OBJECT_0);
    return thread;
}
static void clipboard_unlock(HANDLE thread)
{
    SetEvent(clipboard_release);
    VERIFY("clipboard holder closes and exits",thread&&WaitForSingleObject(thread,1500)==WAIT_OBJECT_0);
    if(thread)CloseHandle(thread);
}
static DWORD WINAPI clipboard_release_after(void *arg)
{(void)arg;Sleep(40);SetEvent(clipboard_release);return 0;}
static void clipboard_wait(void)
{
    uint64_t until=GetTickCount64()+1800;
    while(g_clipboard.data&&GetTickCount64()<until)drag_pump(5);
}
static void test_clipboard_publication_ui(void)
{
    puts("clipboard: bounded, nonblocking publication and immutable request ownership");
    clipboard_held=CreateEventW(NULL,TRUE,FALSE,NULL);clipboard_release=CreateEventW(NULL,TRUE,FALSE,NULL);
    clipboard_seed(L"before");HANDLE holder=clipboard_lock();
    wchar_t original[MAX_PATH+160],mutable[MAX_PATH+160];
    _snwprintf(original,MAX_PATH+160,L"%ls\\original.pdf",drag_left);wcscpy(mutable,original);
    wchar_t *paths[]={mutable};
    LARGE_INTEGER start,end,frequency;QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);
    clip_set(&g.panes[0],paths,1,1);QueryPerformanceCounter(&end);
    double elapsed=1000.0*(end.QuadPart-start.QuadPart)/frequency.QuadPart;
    VERIFY("contended file copy returns inside the16ms dispatch budget",elapsed<16&&g_clipboard.data!=NULL);
    wcscpy(mutable,L"C:\\changed-selection.txt");
    clipboard_unlock(holder);clipboard_wait();
    int exact=0,move=0;
    if(OpenClipboard(g.hwnd)){
        HDROP drop=GetClipboardData(CF_HDROP);wchar_t path[MAX_PATH+160];
        exact=drop&&DragQueryFileW(drop,0xffffffff,NULL,0)==1&&DragQueryFileW(drop,0,path,MAX_PATH+160)&&!wcscmp(path,original);
        HGLOBAL effect=GetClipboardData(g.cf_effect);DWORD *p=effect?GlobalLock(effect):NULL;
        move=p&&*p==DROPEFFECT_MOVE;if(p)GlobalUnlock(effect);CloseClipboard();
    }
    VERIFY("retry publishes the original file selection and cut effect",exact&&move&&!g_clipboard.data);
    clipboard_seed(L"deadline");holder=clipboard_lock();clipboard_timer_seen=0;clipboard_timer_last=0;clipboard_expiry_trace=1;
    uint64_t ticket=app_clipboard_begin(g.hwnd);
    app_clipboard_submit(ticket,CF_UNICODETEXT,clipboard_string(L"late"),0,NULL,L"Copied");
    clipboard_expiry_fact("submitted-held-request");clipboard_wait();clipboard_expiry_fact("existing1800ms-observation");
    VERIFY("a continuously held clipboard expires without publishing",!g_clipboard.data&&!g_clipboard.owner);
    clipboard_unlock(holder);drag_pump(60);clipboard_expiry_fact("after-owned-holder-release");clipboard_expiry_trace=0;
    VERIFY("expiry leaves the prior clipboard intact after unlock",clipboard_is_text(L"deadline"));
    holder=clipboard_lock();ticket=app_clipboard_begin(g.hwnd);
    app_clipboard_submit(ticket,CF_UNICODETEXT,clipboard_string(L"superseded"),0,NULL,L"Copied");
    uint64_t next=app_clipboard_begin(g.hwnd);
    app_clipboard_submit(next,CF_UNICODETEXT,clipboard_string(L"latest"),0,NULL,L"Copied");
    app_clipboard_submit(ticket,CF_UNICODETEXT,clipboard_string(L"obsolete preparation"),0,NULL,L"Copied");
    clipboard_unlock(holder);clipboard_wait();
    VERIFY("the newest request wins over pending and late prepared copies",clipboard_is_text(L"latest"));
    holder=clipboard_lock();ticket=app_clipboard_begin(g.hwnd);
    app_clipboard_submit(ticket,CF_UNICODETEXT,clipboard_string(L"obsolete"),0,NULL,L"Copied");
    clipboard_unlock(holder);
    VERIFY("another publisher obtains the clipboard",OpenClipboard(g.hwnd));
    EmptyClipboard();SetClipboardData(CF_UNICODETEXT,clipboard_string(L"external change"));CloseClipboard();
    clipboard_wait();
    VERIFY("a clipboard change cancels pending publication",clipboard_is_text(L"external change")&&!g_clipboard.data);
    HWND owner=CreateWindowExW(0,L"STATIC",L"clipboard owner",WS_CHILD,0,0,1,1,g.hwnd,NULL,GetModuleHandleW(NULL),NULL);
    holder=clipboard_lock();ticket=app_clipboard_begin(owner);
    app_clipboard_submit(ticket,CF_UNICODETEXT,clipboard_string(L"closed viewer"),0,NULL,L"Copied");
    DestroyWindow(owner);clipboard_unlock(holder);clipboard_wait();
    VERIFY("a destroyed source cannot publish later",clipboard_is_text(L"external change")&&!g_clipboard.data);
    for(int fault=0;fault<3;fault++){
        clipboard_seed(L"retained");
        DWORD retained_sequence=GetClipboardSequenceNumber();
        ticket=app_clipboard_begin(g.hwnd);
        clipboard_track[0]=clipboard_string(L"first");clipboard_track[1]=GlobalAlloc(GMEM_MOVEABLE,sizeof(DWORD));
        DWORD *p=GlobalLock(clipboard_track[1]);*p=1;GlobalUnlock(clipboard_track[1]);
        clipboard_freed[0]=clipboard_freed[1]=0;
        if(fault==0)clipboard_fail_empty=1;
        else clipboard_fail_format=fault==1?CF_UNICODETEXT:g.cf_effect;
        app_clipboard_submit(ticket,CF_UNICODETEXT,clipboard_track[0],g.cf_effect,clipboard_track[1],L"Copied");
        clipboard_wait();
        VERIFY("clipboard failures release only handles still owned by the app",!g_clipboard.data&&clipboard_freed[0]==(fault==2?0:1)&&clipboard_freed[1]==1);
        if(fault==0){
            VERIFY("EmptyClipboard failure preserves existing data",clipboard_preserved(L"retained",retained_sequence));
            holder=clipboard_lock();DWORD read_error=0;
            VERIFY("clipboard oracle distinguishes unavailable reads from changed contents",clipboard_probe_text(L"retained",&read_error)==CLIP_READ_BUSY&&read_error!=0);
            HANDLE release=CreateThread(NULL,0,clipboard_release_after,NULL,0,NULL);
            VERIFY("clipboard read oracle recovers from contention without republishing",release&&clipboard_preserved(L"retained",retained_sequence));
            clipboard_unlock(holder);
            VERIFY("clipboard oracle release worker exits",release&&WaitForSingleObject(release,1500)==WAIT_OBJECT_0);
            if(release)CloseHandle(release);
            VERIFY("clipboard oracle reports a verified mismatch immediately",clipboard_probe_text(L"different",&read_error)==CLIP_READ_CHANGED);
        }
        else VERIFY("SetClipboardData failure reports failure without a partial copy",!IsClipboardFormatAvailable(CF_UNICODETEXT));
        clipboard_track[0]=clipboard_track[1]=NULL;
    }
    holder=clipboard_lock();ticket=app_clipboard_begin(g.hwnd);
    app_clipboard_submit(ticket,CF_UNICODETEXT,clipboard_string(L"cancelled"),0,NULL,L"Copied");
    app_clipboard_cancel(NULL);clipboard_unlock(holder);drag_pump(60);
    app_clipboard_submit(ticket,CF_UNICODETEXT,clipboard_string(L"late after shutdown"),0,NULL,L"Copied");
    VERIFY("shutdown cancellation rejects pending timers and late results",!g_clipboard.data&&!g_clipboard.owner&&!IsClipboardFormatAvailable(CF_UNICODETEXT));
    CloseHandle(clipboard_held);CloseHandle(clipboard_release);
}
static void test_drop_startup_ui(void)
{
    puts("drop startup: one native registration per bounded step, failure and cleanup");
    const fn_def *left=fn_find("svc.drop"),*right=fn_find("svc.drop.right"),*tree=fn_find("svc.drop.tree");
    drop_observe=1;drop_calls=drop_revokes=drop_fail_at=0;
    fn_stop(left);
    VERIFY("stopping the master releases all three owned registrations",drop_revokes==3&&!fn_running(left)&&!fn_running(right)&&!fn_running(tree));
    uint32_t cursor=0;int steps=0,more=1;
    while(more&&steps++<64){int before=drop_calls;more=fn_start_hot_step(1,&cursor);VERIFY("a bounded startup step registers at most one window",drop_calls-before<=1);}
    VERIFY("startup registers left, right and tree in dependency order",!more&&drop_calls==3&&drop_order[0]==g.panes[0].hwnd&&drop_order[1]==g.panes[1].hwnd&&drop_order[2]==T.hwnd);
    VERIFY("every successful registration has a running lifetime owner",fn_running(left)&&fn_running(right)&&fn_running(tree));
    char disabled[512];snprintf(disabled,sizeof disabled,"%s\n[functions]\nsvc.drop = off\n",drag_settings);
    drop_revokes=0;fn_reload(disabled,strlen(disabled));
    VERIFY("disabling the master also disables and releases both dependent targets",drop_revokes==3&&!fn_active(right)&&!fn_active(tree)&&!fn_running(left)&&!fn_running(right)&&!fn_running(tree));
    /* Load the on settings without fn_reload's hot restart: exercise the same
     * bounded primitive used by first-frame initialization. */
    fn_settings(drag_settings,strlen(drag_settings));fn_configure_all();
    drop_calls=drop_revokes=0;drop_fail_at=3;cursor=0;steps=0;more=1;
    while(more&&steps++<64){more=fn_start_hot_step(1,&cursor);if(more==FN_PENDING)drag_pump(5);}
    char report[65536];fn_report_csv(report,sizeof report);
    const char *tree_row=strstr(report,"fn,svc.drop.tree,");
    const char *failed_state=tree_row?strstr(tree_row,",failed,"):NULL;
    VERIFY("a native registration failure is reported instead of claiming success",!more&&drop_calls==3&&fn_running(left)&&fn_running(right)&&!fn_running(tree)&&failed_state&&failed_state<strchr(tree_row,'\n'));
    fn_reload(disabled,strlen(disabled));
    VERIFY("failure cleanup revokes only the two successful registrations",drop_revokes==2&&!fn_running(left)&&!fn_running(right)&&!fn_running(tree));
    drop_fail_at=0;fn_reload(drag_settings,strlen(drag_settings));
    uint64_t reenabled_until=GetTickCount64()+5000;
    while(!fn_running(tree)&&GetTickCount64()<reenabled_until)drag_pump(5);
    VERIFY("explicit settings reenable recovers the failed registration",fn_running(left)&&fn_running(right)&&fn_running(tree));
    drop_observe=0;
}
static void test_drop_settings_lifecycle_ui(void)
{
    puts("drop lifecycle: disable and reenable while the prior apartment stops");
    const fn_def *owner=fn_find("svc.drop.warmup"),*shell=fn_find("svc.shell");
    char no_drop[512];snprintf(no_drop,sizeof no_drop,"%s\n[functions]\nsvc.drop = off\n",drag_settings);
    fn_reload(no_drop,strlen(no_drop));
    uint64_t drained_until=GetTickCount64()+5000;while(dropwarm_wait_handle()&&GetTickCount64()<drained_until)drag_pump(5);
    fn_reload(drag_settings,strlen(drag_settings)); /* start a fresh pending generation */
    char off[512];snprintf(off,sizeof off,"%s\n[functions]\nsvc.shell = off\n",drag_settings);
    fn_reload(off,strlen(off));
    VERIFY("disabling shell immediately withdraws all real drop targets",!GetPropW(g.panes[0].hwnd,L"OleDropTargetInterface")&&!GetPropW(g.panes[1].hwnd,L"OleDropTargetInterface")&&!GetPropW(T.hwnd,L"OleDropTargetInterface"));
    VERIFY("parent apartment remains initialized until worker collection",!fn_running(shell)&&shell_ready()&&dropwarm_wait_handle());
    fn_reload(drag_settings,strlen(drag_settings));
    VERIFY("reenable cannot replace an apartment while it is stopping",!fn_running(owner));
    uint64_t until=GetTickCount64()+5000;
    while(!fn_running(fn_find("svc.drop.tree"))&&GetTickCount64()<until)drag_pump(5);
    VERIFY("reenable completes worker cleanup before publishing a new generation",fn_running(owner)&&fn_running(shell)&&fn_running(fn_find("svc.drop.tree"))&&shell_ready());
}

static uint8_t *paint_snapshot(Pane *p,int *bytes)
{
    GdiFlush();DIBSECTION dib={0};
    if(GetObjectW(p->bb.bmp,sizeof dib,&dib)!=sizeof dib||!dib.dsBm.bmBits)return NULL;
    *bytes=p->cw*p->ch*4;uint8_t *copy=malloc((size_t)*bytes);if(!copy)return NULL;
    for(int y=0;y<p->ch;y++)memcpy(copy+(size_t)y*p->cw*4,(uint8_t *)dib.dsBm.bmBits+(size_t)y*dib.dsBm.bmWidthBytes,(size_t)p->cw*4);
    return copy;
}
static void test_partial_painting_ui(void)
{
    puts("painting: footer damage isolation and partial/full pixel equivalence");
    Pane *p=&g.panes[0];
    for(int i=0;i<4;i++){wchar_t name[40];_snwprintf(name,40,L"paint-%d.txt",i);wchar_t *file=drag_path(drag_left,name);drag_write(file,"pixels");free(file);}
    p_refresh(p);drag_settle(p);drag_pump(100);
    InvalidateRect(p->hwnd,NULL,FALSE);UpdateWindow(p->hwnd);
    int size=0;uint8_t *before=paint_snapshot(p,&size);
    VERIFY("pane back buffer exposes a complete CPU pixel surface",before&&size==p->cw*p->ch*4);
    set_status(p,0,L"Footer-only update");RECT dirty={0};GetUpdateRect(p->hwnd,&dirty,FALSE);
    VERIFY("status changes invalidate only the footer",dirty.top==p->ch-g.status_h&&dirty.bottom==p->ch);
    UpdateWindow(p->hwnd);int after_size=0;uint8_t *after=paint_snapshot(p,&after_size);
    VERIFY("footer repaint preserves all pixels above it",before&&after&&size==after_size&&!memcmp(before,after,(size_t)p->cw*(p->ch-g.status_h)*4));
    free(before);free(after);
    uint32_t row=1;int y=p_list_top(p)+g.row_h;
    p->drop_row=(int)row;RECT one={0,y,p->cw,y+g.row_h};
    InvalidateRect(p->hwnd,&one,FALSE);UpdateWindow(p->hwnd);before=paint_snapshot(p,&size);
    InvalidateRect(p->hwnd,NULL,FALSE);UpdateWindow(p->hwnd);after=paint_snapshot(p,&after_size);
    if(before&&after&&size==after_size){for(int i=0;i<size;i++)if(before[i]!=after[i]){printf("Partial paint differs at x=%d y=%d channel=%d partial=%u full=%u\n",(i/4)%p->cw,(i/4)/p->cw,i%4,before[i],after[i]);break;}}
    VERIFY("one-row partial repaint matches a full repaint pixel for pixel",before&&after&&size==after_size&&!memcmp(before,after,(size_t)size));
    free(before);free(after);
    DWORD objects=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    for(int i=0;i<30;i++){set_status(p,0,L"Status %d",i);UpdateWindow(p->hwnd);}
    VERIFY("repeated dirty repaint releases every temporary GDI object",GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==objects);
    p->drop_row=-1;set_status(p,0,L"");p_clear_sel(p);p_redraw(p);
    for(int i=0;i<4;i++){wchar_t name[40];_snwprintf(name,40,L"paint-%d.txt",i);wchar_t *file=drag_path(drag_left,name);DeleteFileW(file);free(file);}
    p_refresh(p);drag_settle(p);drag_pump(50);
    VERIFY("idle UI turns release temporary pane and main pixel buffers",!p->bb.bmp&&!g.bb.bmp);
}

static void test_deferred_pane_geometry(void)
{
    puts("pane layout: deferred child geometry and complete live frames");
    Pane *a=&g.panes[0],*b=&g.panes[1];double ratio=g.ratio;
    g.ratio=0.43;layout();
    VERIFY("a native batch queues child geometry for each changed pane",a->geometry_pending&&b->geometry_pending);
    drag_pump(10);
    VERIFY("queued geometry completes before painting the panes",!a->geometry_pending&&!b->geometry_pending);
    VERIFY("both address controls fit the final pane geometry",drag_address_fits(a)&&drag_address_fits(b));
    g.ratio=ratio;layout_now();
    VERIFY("live layout flushes child geometry before its complete frame",!a->geometry_pending&&!b->geometry_pending&&drag_address_fits(a)&&drag_address_fits(b));
    /* Windows clamps nPage to the range for an empty/small view. Repainting
     * an unchanged view must not resubmit its larger physical viewport. */
    p_redraw(a);int64_t pane_calls=drag_zone_calls("pane.scrollbar");
    for(int i=0;i<10;i++)p_redraw(a);
    VERIFY("unchanged small folders do not repeat native scrollbar updates",p_count(a)==0&&p_page(a)>1&&drag_zone_calls("pane.scrollbar")==pane_calls);
    int show_tree=g.show_tree;g.show_tree=1;layout_now();tree_redraw();
    int64_t tree_calls=drag_zone_calls("tree.scrollbar");
    for(int i=0;i<10;i++)tree_redraw();
    VERIFY("unchanged small folder trees do not repeat native scrollbar updates",T.nrows==0&&tree_page()>1&&drag_zone_calls("tree.scrollbar")==tree_calls);
    g.show_tree=show_tree;layout_now();
}

static void test_tab_drag_ui(void)
{
    puts("tab dragging: real mouse handlers, ownership, history and persistence");
    Pane *left=&g.panes[0],*right=&g.panes[1];
    wchar_t *b=drag_path(drag_root,L"B"),*c=drag_path(drag_root,L"C"),*child=drag_path(drag_left,L"child");
    CreateDirectoryW(b,NULL);CreateDirectoryW(c,NULL);CreateDirectoryW(child,NULL);
    tab_new(left,c,0);tab_new(left,b,0);
    VERIFY("the private pane has three independently owned tabs",left->ntabs==3 && left->tab==0);
    drag_tab(left,2,left,0,0);
    VERIFY("dragging a background tab reorders it without changing the shown folder",
        same_path(left->tabs[0].path,c) && same_path(left->path,drag_left) && left->tab==1);
    drag_tab(left,0,left,3,0);
    VERIFY("a tab can move to the last insertion slot",same_path(left->tabs[2].path,c) && left->tab==0);
    drag_tab(left,1,right,0,1);
    VERIFY("Escape cancels a cross-pane tab drag",left->ntabs==3 && right->ntabs==1 && !g_tab_drag.source);
    drag_tab(left,1,right,0,0);
    VERIFY("an inactive tab moves to a pane with no visible strip",left->ntabs==2 && right->ntabs==2 && same_path(right->path,b));
    VERIFY("moving an inactive tab leaves the source folder shown",same_path(left->path,drag_left));
    p_navigate(left,child,NULL,1);drag_settle(left);p_set_sort(left,FPE_SORT_SIZE);
    VERIFY("the source tab has navigation history",left->nback==1);
    drag_tab(left,left->tab,right,0,0);
    VERIFY("an active tab moves with its sort and back history",left->ntabs==1 && right->ntabs==3 &&
        same_path(right->path,child) && right->opts.key==FPE_SORT_SIZE && right->nback==1);
    VERIFY("the source pane still contains a usable tab",same_path(left->path,c));
    p_go_back(right);drag_settle(right);
    VERIFY("Back in the moved tab returns to its original folder",same_path(right->path,drag_left) && right->nfwd==1);
    tab_new(left,b,0);
    int left_count=left->ntabs,right_count=right->ntabs;
    app_toggle_dual();drag_tab(left,0,right,0,0);
    VERIFY("a hidden pane cannot receive a tab",left->ntabs==left_count && right->ntabs==right_count);
    app_toggle_dual();
    while(right->ntabs<TABS_MAX)tab_new(right,drag_right,0);
    drag_tab(left,0,right,0,0);
    VERIFY("a full pane rejects a tab without losing it",left->ntabs==left_count && right->ntabs==TABS_MAX);
    wchar_t *narrow_path=wdup(right->tabs[1].path);
    drag_tab(right,1,right,TABS_MAX,0);
    VERIFY("narrow tabs at the tab limit can still be dragged",right->ntabs==TABS_MAX && same_path(right->tabs[TABS_MAX-1].path,narrow_path));
    free(narrow_path);
    while(right->ntabs>3)tab_close(right,right->ntabs-1);
    wchar_t *session_file=drag_path(drag_root,L"session.ini");wcscpy(g.session,session_file);session_save();
    HANDLE f=CreateFileW(session_file,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);char data[16384];DWORD n=0;
    if(f!=INVALID_HANDLE_VALUE){ReadFile(f,data,sizeof data-1,&n,NULL);CloseHandle(f);}
    session saved;session_parse(data,n,&saved);
    VERIFY("the reordered/moved tab list is saved with the session",saved.ntabs[0]==left->ntabs && saved.ntabs[1]==right->ntabs &&
        saved.cur[1]==right->tab && same_path(saved.tabs[1][right->tab].path,right->path));
    session_free(&saved);g.session[0]=0;free(session_file);free(b);free(c);free(child);
}

/* One CF_HDROP object, with no Explorer extension activation needed. */
static IDataObject *drag_object_paths(const wchar_t *const *sources,uint32_t count)
{
    IDataObject *obj=NULL;
    HRESULT hr=SHCreateDataObject(NULL,0,NULL,NULL,&IID_IDataObject,(void **)&obj);
    if(FAILED(hr))return NULL;
    size_t bytes=sizeof(DROPFILES)+sizeof(wchar_t);for(uint32_t i=0;i<count;i++)bytes+=(wcslen(sources[i])+1)*sizeof(wchar_t);
    HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT,bytes);
    DROPFILES *drop=memory?GlobalLock(memory):NULL;
    if(!drop){if(memory)GlobalFree(memory);IDataObject_Release(obj);return NULL;}
    drop->pFiles=sizeof(DROPFILES);drop->fWide=TRUE;wchar_t *p=(wchar_t *)((BYTE *)drop+sizeof *drop);
    for(uint32_t i=0;i<count;i++){wcscpy(p,sources[i]);p+=wcslen(p)+1;}GlobalUnlock(memory);
    FORMATETC fe={CF_HDROP,NULL,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};STGMEDIUM sm={TYMED_HGLOBAL,{.hGlobal=memory},NULL};
    if(FAILED(IDataObject_SetData(obj,&fe,&sm,TRUE))){GlobalFree(memory);IDataObject_Release(obj);return NULL;}
    return obj;
}
static void drag_snapshot(HWND hwnd)
{
    RECT r;GetWindowRect(hwnd,&r);int width=r.right-r.left,height=r.bottom-r.top;
    HDC screen=GetDC(hwnd),dc=CreateCompatibleDC(screen);BITMAPINFO info={.bmiHeader={sizeof(BITMAPINFOHEADER),width,-height,1,32,BI_RGB}};
    void *pixels=NULL;HBITMAP bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&pixels,NULL,0);
    if(bitmap&&dc){HGDIOBJ old=SelectObject(dc,bitmap);PrintWindow(hwnd,dc,0);
        wchar_t owned_snapshot[4096];DWORD own=GetEnvironmentVariableW(L"OPENPILOT_DRAG_SNAPSHOT",owned_snapshot,4096);
        HANDLE file=CreateFileW(own&&own<4096?owned_snapshot:L"build\\symlinks\\link-dialog.bmp",GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
        if(file!=INVALID_HANDLE_VALUE){DWORD wrote;DWORD size=(DWORD)(width*height*4);
            BITMAPFILEHEADER head={.bfType=0x4d42,.bfSize=sizeof head+sizeof info.bmiHeader+size,.bfOffBits=sizeof head+sizeof info.bmiHeader};
            WriteFile(file,&head,sizeof head,&wrote,NULL);WriteFile(file,&info.bmiHeader,sizeof info.bmiHeader,&wrote,NULL);WriteFile(file,pixels,size,&wrote,NULL);CloseHandle(file);}
        SelectObject(dc,old);
    }
    if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);ReleaseDC(hwnd,screen);
}
static HWND drag_menu_window(void)
{
    HWND h=NULL;DWORD pid;
    while((h=FindWindowExW(NULL,h,L"OpenPilotMenu",NULL))){GetWindowThreadProcessId(h,&pid);
        if(pid==GetCurrentProcessId() && IsWindowVisible(h))return h;}
    return NULL;
}
static HWND drag_link_dialog(void)
{
    HWND h=NULL;DWORD pid;
    while((h=FindWindowExW(NULL,h,L"OpenPilotSymlink",L"Create symbolic links"))){
        GetWindowThreadProcessId(h,&pid);
        if(pid==GetCurrentProcessId()&&IsWindowVisible(h)&&!IsWindowEnabled(g.hwnd)&&
           GetDlgItem(h,IDOK)&&GetDlgItem(h,101)&&GetDlgItem(h,102))return h;
    }return NULL;
}
typedef struct drag_menu_pick {
    int choice,seen,correct;DWORD enabled;
    int linux,configure_link,relative,cancel_link,dialog_seen,owner_disabled,invalid_refused,no_mutation;
    const wchar_t *name,*output;
} drag_menu_pick;
static DWORD WINAPI drag_pick_worker(void *arg)
{
    drag_menu_pick *pick=arg;HWND menu=NULL;uint64_t until=GetTickCount64()+5000;
    while(!menu && GetTickCount64()<until){menu=drag_menu_window();Sleep(5);}
    if(!menu)return 0;
    pick->seen=1;
    HMENU m=(HMENU)SendMessageW(menu,MN_GETHMENU,0,0);
    wchar_t text[100];
    if(pick->choice==-2){
        int properties=0,release_menu=0;
        for(int i=0;i<GetMenuItemCount(m);++i){GetMenuStringW(m,i,text,100,MF_BYPOSITION);
            wchar_t *tab=wcschr(text,L'\t');if(tab)*tab=0;
            if(!wcscmp(text,L"Properties"))properties=1;if(!wcscmp(text,L"Copy here"))release_menu=1;}
        pick->correct=properties && !release_menu;PostMessageW(menu,WM_KEYDOWN,VK_ESCAPE,0);return 0;
    }
    GetMenuStringW(m,0,text,100,MF_BYPOSITION);pick->correct=!wcscmp(text,L"Copy here");
    GetMenuStringW(m,1,text,100,MF_BYPOSITION);pick->correct&=!wcscmp(text,L"Move here");
    GetMenuStringW(m,2,text,100,MF_BYPOSITION);pick->correct&=!wcscmp(text,pick->linux?L"Create symbolic link here":L"Create shortcut here");
    for(int i=0;i<3;++i)if(!(GetMenuState(m,i,MF_BYPOSITION)&(MF_DISABLED|MF_GRAYED)))pick->enabled|=1u<<i;
    if(pick->choice<0 || (pick->choice<3 && !(pick->enabled&(1u<<pick->choice)))){PostMessageW(menu,WM_KEYDOWN,VK_ESCAPE,0);return 0;}
    /* OpenPilot's menu highlights disabled rows too; Down skips separators. */
    PostMessageW(menu,WM_KEYDOWN,VK_HOME,0);Sleep(20);
    int steps=pick->choice<3?pick->choice:3;
    for(int i=0;i<steps;++i){PostMessageW(menu,WM_KEYDOWN,VK_DOWN,0);Sleep(20);}
    PostMessageW(menu,WM_KEYDOWN,VK_RETURN,0);
    if(pick->configure_link){
        HWND dialog=NULL;until=GetTickCount64()+5000;
        /* A top-level HWND exists during WM_CREATE, before the edit/radios or
         * modal owner are ready. Never drive a partially constructed dialog or
         * a dialog belonging to another OpenPilot process. */
        while(!dialog&&GetTickCount64()<until){dialog=drag_link_dialog();Sleep(5);}
        if(!dialog)return 0;pick->dialog_seen=1;pick->owner_disabled=!IsWindowEnabled(g.hwnd);
        pick->no_mutation=!pick->output||!drag_exists(pick->output);
        HWND edit=GetDlgItem(dialog,103);
        if(edit){
            SetWindowTextW(edit,L"../invalid");
            int disabled=!IsWindowEnabled(GetDlgItem(dialog,IDOK));
            SendMessageW(dialog,WM_COMMAND,IDOK,0);pick->invalid_refused=disabled&&IsWindow(dialog);
            if(pick->name)SetWindowTextW(edit,pick->name);
            if(pick->output)drag_snapshot(dialog);
        }
        SendMessageW(GetDlgItem(dialog,pick->relative?101:102),BM_CLICK,0,0);
        if(pick->cancel_link)PostMessageW(dialog,WM_KEYDOWN,VK_ESCAPE,0);
        else PostMessageW(dialog,WM_COMMAND,IDOK,0);
    }
    return 0;
}
static DWORD drag_drop_pick_paths(Pane *target,const wchar_t *const *sources,uint32_t count,int right_button,drag_menu_pick *pick)
{
    IDataObject *obj=drag_object_paths(sources,count);
    IDropTarget *dt=(IDropTarget *)GetPropW(target->hwnd,L"OleDropTargetInterface");
    VERIFY("the real OLE target and CF_HDROP fixture exist",obj && dt);
    if(!obj||!dt){if(obj)IDataObject_Release(obj);return 0;}
    POINT pt={S(70),target->ch-g.status_h-S(20)};ClientToScreen(target->hwnd,&pt);POINTL point={pt.x,pt.y};
    DWORD effect=DROPEFFECT_COPY|DROPEFFECT_MOVE|DROPEFFECT_LINK;
    IDropTarget_DragEnter(dt,obj,right_button?MK_RBUTTON:MK_LBUTTON,point,&effect);
    VERIFY("the target advertises a legal effect while hovering",effect!=DROPEFFECT_NONE);
    pick->linux=fpe_wsl_native(target->path);
    HANDLE thread=right_button?CreateThread(NULL,0,drag_pick_worker,pick,0,NULL):NULL;
    effect=DROPEFFECT_COPY|DROPEFFECT_MOVE|DROPEFFECT_LINK;
    IDropTarget_Drop(dt,obj,0,point,&effect); /* button already released: target must remember right-drag */
    if(thread){WaitForSingleObject(thread,6000);CloseHandle(thread);}
    if(right_button){
        VERIFY("release shows the dedicated menu with the destination's link action",pick->seen && pick->correct);
        VERIFY("the menu enables only legal source/destination effects",pick->enabled==drop_choices(sources,count,target->path,DROPEFFECT_COPY|DROPEFFECT_MOVE|DROPEFFECT_LINK));
    }
    IDataObject_Release(obj);return effect;
}
static DWORD drag_drop_pick(Pane *target,const wchar_t *source,int right_button,drag_menu_pick *pick)
{
    return drag_drop_pick_paths(target,&source,1,right_button,pick);
}
static DWORD drag_drop(Pane *target,const wchar_t *source,int right_button,int choice)
{
    drag_menu_pick pick={.choice=choice};return drag_drop_pick(target,source,right_button,&pick);
}
static int admission_child(DWORD pid)
{
    HANDLE h=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(h==INVALID_HANDLE_VALUE)return 0;PROCESSENTRY32W p={.dwSize=sizeof p};int found=0;
    if(Process32FirstW(h,&p))do{if(p.th32ProcessID==pid){found=p.th32ParentProcessID==GetCurrentProcessId();break;}}while(Process32NextW(h,&p));CloseHandle(h);return found;
}
static BOOL CALLBACK admission_window(HWND h,LPARAM ignored)
{
    (void)ignored;wchar_t cls[80];DWORD pid=0;GetClassNameW(h,cls,80);if(wcscmp(cls,L"FpOperationsHidden"))return TRUE;GetWindowThreadProcessId(h,&pid);if(!admission_child(pid))return TRUE;
    HANDLE proc=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);FILETIME create={0},exit,kernel,user;if(proc){GetProcessTimes(proc,&create,&exit,&kernel,&user);CloseHandle(proc);}
    printf("OP_ADMISSION helper_pid=%lu helper_hwnd=%p helper_birth=%lu:%lu visible=%d ready_property=%lld ready_contract=not-public-property\n",pid,h,create.dwHighDateTime,create.dwLowDateTime,IsWindowVisible(h),(long long)(INT_PTR)GetPropW(h,L"Operations.Ready"));return TRUE;
}
static void operation_admission_fact(const char *phase,Pane *p,DWORD effect,const drag_menu_pick *pick)
{
    int row=-99;wchar_t *dest=p_drop_dest(p,S(70),p->ch-g.status_h-S(20),&row);const fn_def *fn=fn_find("operations.show");
    printf("OP_ADMISSION phase=%s tick=%llu effect=%lu menu_seen=%d menu_correct=%d choice=%d hit_row=%d actual_destination=%ls pane_path=%ls pane_count=%u cursor=%u selected=%u loading=%d navigation=%llu view_revision=%llu status_error=%d status=%ls fn_active=%d fn_running=%d requests=%p model_n=%u panel_open=%d panel_running=%d panel_generation=%u panel_hwnd=%p\n",
        phase,(unsigned long long)GetTickCount64(),effect,pick?pick->seen:0,pick?pick->correct:0,pick?pick->choice:-99,row,dest?dest:L"<null>",p->path?p->path:L"<null>",p_count(p),p->cursor,p->nsel,p->loading,(unsigned long long)p->nav_ticket,(unsigned long long)fpe_view_revision(p->view),p->status_err,p->status,fn_active(fn),fn_running(fn),(void*)g_op_requests,g_operations.n,sidepanel_open(),sidepanel_running(operations_app()),sidepanel_generation(operations_app()),sidepanel_window());free(dest);
    for(operation_request *r=g_op_requests;r;r=r->next)printf("OP_ADMISSION request_token=%llu count=%u consume_cut=%d destination=%ls\n",(unsigned long long)r->token,r->count,r->consume_cut,r->dest?r->dest:L"<null>");
    for(uint32_t i=0;i<g_operations.n;i++){const ops_entry *e=&g_operations.entries[i];printf("OP_ADMISSION model_token=%llu job=%llu state=%u op=%u count=%u errors=%u last_error=%u source=%ls destination=%ls\n",(unsigned long long)e->h.token,(unsigned long long)e->h.job,e->h.state,e->h.op,e->h.count,e->h.errors,e->h.last_error,e->source?e->source:L"<null>",e->dest?e->dest:L"<null>");}
    EnumWindows(admission_window,0);fflush(stdout);
}
static void test_right_drop_ui(void)
{
    puts("right file drops: OLE release, dedicated menu, cancellation and queued operations");
    Pane *target=&g.panes[1];p_navigate(target,drag_right,NULL,0);drag_settle(target);
    wchar_t *source=drag_path(drag_left,L"copy.dat"),*output=drag_path(drag_right,L"copy.dat");drag_write(source,"copy");
    VERIFY("Cancel reports no drop effect",drag_drop(target,source,1,3)==DROPEFFECT_NONE);
    drag_pump(100);VERIFY("Cancel changes neither source nor destination",drag_exists(source) && !drag_exists(output));
    VERIFY("Escape cancels the release menu",drag_drop(target,source,1,-1)==DROPEFFECT_NONE && !drag_exists(output));
    VERIFY("Copy here reports COPY",drag_drop(target,source,1,0)==DROPEFFECT_COPY);
    VERIFY("Copy here copies through the job queue and keeps the source",drag_wait_file(output,1) && drag_exists(source));
    DeleteFileW(output);
    VERIFY("Move here never tells an external source to delete originals itself",drag_drop(target,source,1,1)==DROPEFFECT_NONE);
    VERIFY("Move here finishes in the app job queue",drag_wait_file(output,1) && drag_wait_file(source,0));
    free(source);free(output);
    source=drag_path(drag_left,L"ready.dat");output=drag_path(drag_right,L"ready.dat");drag_write(source,"ready");
    fn_reload("[functions]\noperations.show = on\npreview.image = off\npreview.pdf = off\npreview.text = off\nsvc.icons = off\n",strlen("[functions]\noperations.show = on\npreview.image = off\npreview.pdf = off\npreview.text = off\nsvc.icons = off\n"));
    drag_menu_pick admission={.choice=0};operation_admission_fact("before-right-drop",target,0,NULL);
    DWORD admission_effect=drag_drop_pick(target,source,1,&admission);drag_pump(150);
    if(!g_op_requests||drag_exists(output))operation_admission_fact("first-existing150ms-failure",target,admission_effect,&admission);
    else operation_admission_fact("existing150ms-ready",target,admission_effect,&admission);
    VERIFY("right-drop Copy opens Operations before any mutation",g_op_requests && !drag_exists(output));
    if(g_op_requests){ops_action start={.token=g_op_requests->token,.action=OPS_START,.policy=FPE_CONFLICT_RENAME};operations_action(&start);}
    VERIFY("the confirmed operation reaches its chosen destination",drag_wait_file(output,1) && drag_exists(source));
    sidepanel_close();fn_reload("[functions]\noperations.show = off\npreview.image = off\npreview.pdf = off\npreview.text = off\nsvc.icons = off\n",strlen("[functions]\noperations.show = off\npreview.image = off\npreview.pdf = off\npreview.text = off\nsvc.icons = off\n"));
    free(source);free(output);
    source=drag_path(drag_left,L"leftdrag.dat");output=drag_path(drag_right,L"leftdrag.dat");drag_write(source,"left");
    drag_drop(target,source,0,0);
    VERIFY("left-button drops still use the normal move rule without a release menu",drag_wait_file(output,1) && drag_wait_file(source,0) && drag_menu_window()==NULL);
    free(source);free(output);
    source=drag_path(drag_right,L"duplicate.dat");output=drag_path(drag_right,L"duplicate (2).dat");drag_write(source,"duplicate");
    drag_drop(target,source,1,0);
    VERIFY("an explicit right-drop Copy can duplicate in the same folder",drag_wait_file(output,1) && drag_exists(source));
    free(source);free(output);
}
static void test_shortcut_drop_ui(void)
{
    puts("shortcut drops: worker creation, exact target and collision refusal");
    Pane *target=&g.panes[1];p_navigate(target,drag_right,NULL,0);drag_settle(target);wchar_t *source=drag_path(drag_left,L"link.dat"),*output=drag_path(drag_right,L"link.dat - Shortcut.lnk");
    drag_write(source,"target");
    VERIFY("Create shortcut here reports LINK",drag_drop(target,source,1,2)==DROPEFFECT_LINK);
    VERIFY("a shortcut appears without moving or copying its target",drag_wait_file(output,1) && drag_exists(source));
    IShellLinkW *link=NULL;IPersistFile *persist=NULL;wchar_t actual[MAX_PATH+200]={0};
    HRESULT hr=CoCreateInstance(&CLSID_ShellLink,NULL,CLSCTX_INPROC_SERVER,&IID_IShellLinkW,(void **)&link);
    if(SUCCEEDED(hr))hr=IShellLinkW_QueryInterface(link,&IID_IPersistFile,(void **)&persist);
    if(SUCCEEDED(hr))hr=IPersistFile_Load(persist,output,STGM_READ);
    if(SUCCEEDED(hr))hr=IShellLinkW_GetPath(link,actual,MAX_PATH+200,NULL,SLGP_RAWPATH);
    if(FAILED(hr)||!same_path(actual,source))wprintf(L"Shortcut diagnostic: hr=%08lx actual=%ls expected=%ls\n",(unsigned long)hr,actual,source);
    VERIFY("the saved shortcut resolves to the exact original path",SUCCEEDED(hr) && same_path(actual,source));
    if(persist)IPersistFile_Release(persist);if(link)IShellLinkW_Release(link);
    drag_drop(target,source,1,2);
    wchar_t *second=drag_path(drag_right,L"link.dat - Shortcut (2).lnk");
    VERIFY("a second shortcut uses a new name without overwriting the first",drag_wait_file(second,1) && drag_exists(output));
    DWORD collision=shell_create_shortcut(source,output);
    VERIFY("direct shortcut creation refuses an existing destination",collision==ERROR_ALREADY_EXISTS || collision==ERROR_FILE_EXISTS);
    drag_pump(100);VERIFY("shortcuts do not start the inline rename editor",target->rename_edit==NULL);
    free(source);free(output);free(second);
}
static void test_right_drop_wsl_ui(void)
{
    puts("right drops into WSL: native transfer and native link exposure");
    wchar_t dest[4096];
    DWORD n=GetEnvironmentVariableW(L"OPENPILOT_DRAG_WSL_DEST",dest,4096);
    VERIFY("WSL acceptance provides its private destination",n && n<4096);
    if(!n||n>=4096)return;
    Pane *target=&g.panes[1];p_navigate(target,dest,NULL,0);drag_settle(target);
    wchar_t *source=drag_path(drag_left,L"linux-copy.dat"),*output=drag_path(dest,L"linux-copy.dat");drag_write(source,"linux-copy");
    VERIFY("WSL offers native symlinks alongside Copy and Move",drop_choices((const wchar_t *const *)&source,1,dest,7)==7);
    drag_drop(target,source,1,0);
    VERIFY("a right-drop Copy reaches Linux through the native job path",drag_wait_file(output,1) && drag_exists(source));
    free(source);free(output);
    source=drag_path(drag_left,L"linux-move.dat");output=drag_path(dest,L"linux-move.dat");drag_write(source,"linux-move");
    drag_drop(target,source,1,1);
    VERIFY("a right-drop Move to Linux transfers the file before removing its source",drag_wait_file(output,1) && drag_wait_file(source,0));
    free(source);free(output);
}

static int drag_wait_links(void)
{
    const fn_def *d=fn_find("file.symlink");uint64_t until=GetTickCount64()+5000;
    while(fn_running(d)&&GetTickCount64()<until)drag_pump(10);
    return !fn_running(d);
}
static int drag_link(const wchar_t *directory,const wchar_t *name,wchar_t out[4096],int *folder)
{
    fpe_dir *dir=NULL;if(fpe_wsl_read(directory,&dir))return 0;int found=0;
    for(uint32_t i=0;i<fpe_dir_count(dir);i++){
        const fpe_entry *entry=fpe_dir_entries(dir)+i;fpe_unix u;
        if(!wcscmp(fpe_dir_name(dir,entry),name)&&fpe_dir_unix(dir,entry,&u)&&u.link){
            wcsncpy(out,u.link,4095);out[4095]=0;if(folder)*folder=fpe_entry_is_dir(entry);found=1;break;
        }
    }
    fpe_dir_release(dir);return found;
}
/* Preserve independent facts before standing teardown; never infer which
 * object failed from a short-circuit conjunction or a later passing batch. */
static const char *drag_symlink_utf8(const wchar_t *text,char out[16384])
{
    if(!WideCharToMultiByte(CP_UTF8,0,text?text:L"<null>",-1,out,16384,NULL,NULL))strcpy(out,"<UTF8 conversion failed>");return out;
}
static void drag_symlink_object(const char *label,const wchar_t *directory,const wchar_t *name)
{
    fpe_dir *dir=NULL;fpe_err err=fpe_wsl_read(directory,&dir);int found=0;
    if(!err&&dir)for(uint32_t i=0;i<fpe_dir_count(dir);i++){
        const fpe_entry *entry=fpe_dir_entries(dir)+i;if(wcscmp(fpe_dir_name(dir,entry),name))continue;
        fpe_unix u={0};int native=fpe_dir_unix(dir,entry,&u);found=1;
        char value[16384];printf("SYMLINK_OBJECT label=%s read_error=%u found=1 native=%d mode=%o uid=%u gid=%u link=%s\n",label,(unsigned)err,native,u.mode,u.uid,u.gid,drag_symlink_utf8(u.link?u.link:L"<not-link>",value));break;
    }
    if(!found)printf("SYMLINK_OBJECT label=%s read_error=%u found=0\n",label,(unsigned)err);fpe_dir_release(dir);
}
static int drag_symlink_source(const char *label,const wchar_t *path,const char *expected)
{
    HANDLE f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    DWORD error=f==INVALID_HANDLE_VALUE?GetLastError():0,read=0;char bytes[32]={0};int ok=f!=INVALID_HANDLE_VALUE&&ReadFile(f,bytes,sizeof bytes,&read,NULL);
    if(f!=INVALID_HANDLE_VALUE){if(!ok)error=GetLastError();CloseHandle(f);}
    int exact=ok&&read==strlen(expected)&&!memcmp(bytes,expected,read);
    char value[16384];printf("SYMLINK_SOURCE label=%s open_error=%lu read_ok=%d bytes=%lu exact=%d path=%s\n",label,error,ok,read,exact,drag_symlink_utf8(path,value));return exact;
}

static void test_symlink_drop_ui(void)
{
    puts("Linux symlink drops: actual dialog, relative/absolute targets, cancellation and collision refusal");
    wchar_t root[4096],actual[4096];DWORD n=GetEnvironmentVariableW(L"OPENPILOT_DRAG_WSL_DEST",root,4096);
    VERIFY("symlink acceptance has its private Linux root",n&&n<4096);if(!n||n>=4096)return;
    wchar_t *from=join(root,L"source"),*to=join(root,L"links");CreateDirectoryW(from,NULL);CreateDirectoryW(to,NULL);
    wchar_t *source=join(from,L"file β\uf03a"),*relative=join(to,L"relative\uf03aback\uf05cslash\uf02a");drag_write(source,"keep");
    Pane *target=&g.panes[1];p_navigate(target,to,NULL,0);drag_settle(target);
    drag_menu_pick pick={.choice=2,.configure_link=1,.relative=1,.name=L"relative:back\\slash*",.output=relative};
    VERIFY("Create symbolic link here reports LINK",drag_drop_pick(target,source,1,&pick)==DROPEFFECT_LINK);
    VERIFY("the dialog keeps the destination owner modal",pick.dialog_seen&&pick.owner_disabled&&IsWindowEnabled(g.hwnd));
    VERIFY("an invalid link name cannot submit",pick.invalid_refused);
    VERIFY("opening and editing the dialog do not create a link",pick.no_mutation);
    VERIFY("the link worker finishes and releases its cold function",drag_wait_links());
    VERIFY("a relative link preserves the exact Unicode and reserved source name",drag_link(to,L"relative\uf03aback\uf05cslash\uf02a",actual,NULL)&&!wcscmp(actual,L"../source/file β:"));
    VERIFY("creating a Linux link leaves its source intact",drag_exists(source));
    pick=(drag_menu_pick){.choice=2,.configure_link=1,.name=L"absolute-link"};
    VERIFY("absolute target can be chosen through the same release action",drag_drop_pick(target,source,1,&pick)==DROPEFFECT_LINK&&drag_wait_links());
    wchar_t distro[64],linux[PATHENTRY_MAX];pathentry_display(source,distro,64,linux,PATHENTRY_MAX);
    int absolute_found=drag_link(to,L"absolute-link",actual,NULL);
    if(!absolute_found||wcscmp(actual,linux))printf("absolute link diagnostic: found=%d actual=%ls expected=%ls status_err=%d status=%ls\n",absolute_found,absolute_found?actual:L"<absent>",linux,target->status_err,target->status);
    VERIFY("absolute links store the literal Linux path",absolute_found&&!wcscmp(actual,linux));
    pick=(drag_menu_pick){.choice=2,.configure_link=1,.relative=0,.name=L"relative:back\\slash*"};
    drag_drop_pick(target,source,1,&pick);
    VERIFY("a collision is reported without overwriting an existing relative link",drag_wait_links()&&target->status_err&&drag_link(to,L"relative\uf03aback\uf05cslash\uf02a",actual,NULL)&&!wcscmp(actual,L"../source/file β:"));
    pick=(drag_menu_pick){.choice=2,.configure_link=1,.relative=1,.cancel_link=1,.name=L"cancelled"};
    VERIFY("Escape cancels the link dialog and the OLE drop",drag_drop_pick(target,source,1,&pick)==DROPEFFECT_NONE);
    VERIFY("cancelling leaves no link, worker or modal owner",!fn_running(fn_find("file.symlink"))&&IsWindowEnabled(g.hwnd)&&!drag_link(to,L"cancelled",actual,NULL));
    wchar_t *tree=join(from,L"tree"),*child=join(tree,L"nested");CreateDirectoryW(tree,NULL);drag_write(child,"nested");
    pick=(drag_menu_pick){.choice=2,.configure_link=1,.relative=1,.name=L"tree-link"};
    drag_drop_pick(target,tree,1,&pick);int folder=0;
    int folder_done=drag_wait_links(),folder_found=drag_link(to,L"tree-link",actual,&folder);
    if(!folder_done||!folder_found||!folder||wcscmp(actual,L"../source/tree"))printf("folder link diagnostic: done=%d found=%d folder=%d actual=%ls status_err=%d status=%ls\n",folder_done,folder_found,folder,folder_found?actual:L"<absent>",target->status_err,target->status);
    VERIFY("folder links remain symlinks and navigate as folders",folder_done&&folder_found&&folder&&!wcscmp(actual,L"../source/tree"));
    VERIFY("folder linking leaves descendants intact",drag_exists(child));
    wchar_t *windows=join(drag_left,L"windows-link.dat");drag_write(windows,"windows");
    pick=(drag_menu_pick){.choice=2,.configure_link=1,.relative=0,.name=L"windows-link"};drag_drop_pick(target,windows,1,&pick);
    VERIFY("a Windows file can be linked through its Linux-visible mount",drag_wait_links()&&drag_link(to,L"windows-link",actual,NULL)&&actual[0]==L'/'&&wcsstr(actual,L"windows-link.dat")&&drag_exists(windows));
    const wchar_t *multiple[]={source,windows};pick=(drag_menu_pick){.choice=2,.configure_link=1,.relative=1};
    VERIFY("one release menu can create links for multiple selected sources",drag_drop_pick_paths(target,multiple,2,1,&pick)==DROPEFFECT_LINK&&drag_wait_links());
    wchar_t before_link[4096]={0};int before_found=drag_link(to,L"file β\uf03a",before_link,NULL);
    int multiple_linux=before_found&&!wcscmp(before_link,L"../source/file β:");
    int multiple_windows=drag_link(to,L"windows-link.dat",actual,NULL);
    VERIFY("multiple-item links retain their source names and targets",multiple_linux&&multiple_windows);
    wchar_t *fresh=join(drag_left,L"fresh-link.dat");drag_write(fresh,"fresh");
    const wchar_t *partial[]={fresh,source};pick=(drag_menu_pick){.choice=2,.configure_link=1};
    int point_row=-99;wchar_t *point_dest=p_drop_dest(target,S(70),target->ch-g.status_h-S(20),&point_row);
    char text_before[16384],text_dest[16384],text_point[16384],text_fresh[16384],text_old[16384],text_status[16384];
    printf("SYMLINK_PARTIAL_BEFORE old_found=%d old_target=%s pane_destination=%s actual_drop_destination=%s point_row=%d count=%u pane_height=%d list_top=%d top=%d tabs=%d sort=%d fresh=%s source=%s\n",before_found,drag_symlink_utf8(before_found?before_link:L"<absent>",text_before),drag_symlink_utf8(target->path,text_dest),drag_symlink_utf8(point_dest,text_point),point_row,p_count(target),target->ch,p_list_top(target),target->top,target->ntabs,target->opts.key,drag_symlink_utf8(fresh,text_fresh),drag_symlink_utf8(source,text_old));free(point_dest);
    DWORD partial_effect=drag_drop_pick_paths(target,partial,2,1,&pick);
    int partial_drained=drag_wait_links();int partial_error=target->status_err;wchar_t partial_status[512];wcsncpy(partial_status,target->status,511);partial_status[511]=0;
    wchar_t fresh_target[4096]={0},old_target[4096]={0};int fresh_found=drag_link(to,L"fresh-link.dat",fresh_target,NULL),old_found=drag_link(to,L"file β\uf03a",old_target,NULL);
    int fresh_original=drag_symlink_source("fresh-original",fresh,"fresh"),old_original=drag_symlink_source("linux-original",source,"keep");
    printf("SYMLINK_PARTIAL effect=%lu menu_seen=%d menu_correct=%d enabled=%lu dialog_seen=%d owner_disabled=%d owner_enabled=%d drained=%d running=%d status_err=%d status=%s fresh_found=%d fresh_target=%s old_found=%d old_target=%s\n",partial_effect,pick.seen,pick.correct,pick.enabled,pick.dialog_seen,pick.owner_disabled,IsWindowEnabled(g.hwnd),partial_drained,fn_running(fn_find("file.symlink")),partial_error,drag_symlink_utf8(partial_status,text_status),fresh_found,drag_symlink_utf8(fresh_found?fresh_target:L"<absent>",text_fresh),old_found,drag_symlink_utf8(old_found?old_target:L"<absent>",text_old));
    drag_symlink_object("fresh-link",to,L"fresh-link.dat");drag_symlink_object("existing-link",to,L"file β\uf03a");
    VERIFY("partial drop reports LINK independently",partial_effect==DROPEFFECT_LINK);
    VERIFY("partial dialog is accepted and modal owner restored",pick.dialog_seen&&pick.owner_disabled&&IsWindowEnabled(g.hwnd));
    VERIFY("partial worker drains inside the unchanged5s interval",partial_drained);
    VERIFY("partial collision reports an error",partial_error);
    VERIFY("partial failures report the actual created count",wcsstr(partial_status,L"Created 1 of 2 symbolic links")!=NULL);
    VERIFY("partial batch creates the fresh link",fresh_found);
    VERIFY("partial batch preserves the old link target",before_found&&old_found&&!wcscmp(old_target,before_link)&&!wcscmp(old_target,L"../source/file β:"));
    VERIFY("partial batch preserves the Windows original bytes",fresh_original);
    VERIFY("partial batch preserves the Linux original bytes",old_original);
    const wchar_t *many[257];for(int i=0;i<257;i++)many[i]=source;
    VERIFY("link drops are bounded to 256 sources",!(drop_choices(many,257,to,7)&DROPEFFECT_LINK));
    p_navigate(target,from,NULL,0);drag_settle(target);
    pick=(drag_menu_pick){.choice=2,.configure_link=1,.relative=1,.name=L"sibling-link"};drag_drop_pick(target,source,1,&pick);
    VERIFY("a renamed link can be created beside its source",drag_wait_links()&&drag_link(from,L"sibling-link",actual,NULL)&&!wcscmp(actual,L"file β:"));
    p_navigate(target,to,NULL,0);drag_settle(target);
    const wchar_t *foreign=L"\\\\wsl.localhost\\OtherDistro\\tmp\\file";
    VERIFY("cross-distro symbolic links are unavailable",!(drop_choices(&foreign,1,to,7)&DROPEFFECT_LINK));
    VERIFY("a missing drop destination offers no actions",drop_choices((const wchar_t *const *)&source,1,NULL,7)==0);
    const char *off="[functions]\nfile.symlink = off\noperations.show = off\npreview.image = off\npreview.text = off\npreview.pdf = off\nsvc.icons = off\n";
    fn_reload(off,strlen(off));
    VERIFY("the Linux link function can be disabled independently",!(drop_choices((const wchar_t *const *)&source,1,to,7)&DROPEFFECT_LINK));
    VERIFY("disabling Linux links keeps Windows shortcuts available",drop_choices((const wchar_t *const *)&windows,1,drag_right,7)&DROPEFFECT_LINK);
    const char *on="[functions]\noperations.show = off\npreview.image = off\npreview.text = off\npreview.pdf = off\nsvc.icons = off\n";fn_reload(on,strlen(on));
    free(from);free(to);free(source);free(relative);free(tree);free(child);free(windows);free(fresh);
}

static void test_plain_right_click_ui(void)
{
    puts("plain right click: selection and ordinary context menu remain available");
    Pane *p=&g.panes[1];p_navigate(p,drag_right,NULL,0);drag_settle(p);
    drag_menu_pick pick={-2,0,0,0};HANDLE thread=CreateThread(NULL,0,drag_pick_worker,&pick,0,NULL);
    int x=S(65),y=p_list_top(p)+g.row_h/2;
    drag_mouse(p->hwnd,WM_RBUTTONDOWN,MK_RBUTTON,x,y);
    drag_mouse(p->hwnd,WM_RBUTTONUP,0,x,y);
    if(thread){WaitForSingleObject(thread,6000);CloseHandle(thread);}
    VERIFY("a plain right click opens the regular file context menu",pick.seen && pick.correct);
    VERIFY("a plain right click leaves no armed file drag",!p->down_armed && !p->right_suppress);
}

static void test_menu_context_lifetime_ui(void)
{
    puts("menu context: folder and item targets survive navigation; stale choices do not run");
    Pane *p=&g.panes[0];p_navigate(p,drag_left,NULL,0);drag_settle(p);
    fn_ctx c;pane_ctx(&c,p,FN_FROM_MENU,0);
    HMENU menu=CreatePopupMenu();fn_menu(menu,FN_MENU_FOLDER,&c);
    UINT choice=0;wchar_t title[128];
    for(int i=0;i<GetMenuItemCount(menu);i++){
        UINT id=GetMenuItemID(menu,i);
        if(id==(UINT)-1)continue;
        if(GetMenuStringW(menu,id,title,128,MF_BYCOMMAND)&&!wcscmp(title,L"Menu context probe"))choice=id;
    }
    DestroyMenu(menu);
    VERIFY("the real folder menu includes the probe action",choice!=0);
    VERIFY("the initial menu context is current",app_menu_context_current(&c));
    p_navigate(p,drag_right,NULL,0);drag_settle(p);
    VERIFY("the open menu retains its original folder after navigation",c.dir&&!wcscmp(c.dir,drag_left)&&c.dest&&!wcscmp(c.dest,drag_left));
    VERIFY("navigation makes the pending menu choice stale",!app_menu_context_current(&c));
    if(choice)run_menu_choice(&c,choice,"");
    VERIFY("a stale folder choice cannot run against the new pane",menu_context_calls==0);
    ctx_free(&c);

    pane_ctx(&c,p,FN_FROM_MENU,0);
    if(choice)run_menu_choice(&c,choice,"");
    VERIFY("a current folder choice still runs",choice&&menu_context_calls==1);
    ctx_free(&c);

    wchar_t *a=drag_path(drag_left,L"menu-capture-a.txt");
    wchar_t *b=drag_path(drag_left,L"menu-capture-b.txt");
    drag_write(a,"a");drag_write(b,"b");
    fpe_dir *dir=read_start(drag_left);
    if(dir){p_show_dir(p,dir,NULL,0);fpe_dir_release(dir);}
    pane_ctx(&c,p,FN_FROM_MENU,1);
    VERIFY("item menu captures its selected file before opening",c.n&&c.paths&&c.paths[0]&&app_menu_context_current(&c));
    wchar_t *saved=c.n&&c.paths&&c.paths[0]?wdup(c.paths[0]):NULL;
    if(p_count(p)>1)p->cursor=(p->cursor+1)%p_count(p);
    VERIFY("changing the cursor invalidates the original item menu",!app_menu_context_current(&c));
    p_navigate(p,drag_right,NULL,0);drag_settle(p);
    VERIFY("the selected path remains owned after its pane navigates",saved&&c.paths&&c.paths[0]&&!wcscmp(c.paths[0],saved));
    ctx_free(&c);free(saved);free(a);free(b);
}

static void drag_remove_tree(const wchar_t *path)
{
    wchar_t *pattern=join(path,L"*");WIN32_FIND_DATAW data;HANDLE f=FindFirstFileW(pattern,&data);free(pattern);
    if(f!=INVALID_HANDLE_VALUE){do{if(!wcscmp(data.cFileName,L".")||!wcscmp(data.cFileName,L".."))continue;
        wchar_t *child=join(path,data.cFileName);if(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)drag_remove_tree(child);else DeleteFileW(child);free(child);
    }while(FindNextFileW(f,&data));FindClose(f);}RemoveDirectoryW(path);
}
/* An owned named Everything instance exercises the actual asynchronous IPC,
 * without touching a user's Everything window, settings or database. */
static volatile LONG search_server_mode,search_server_calls;
static wchar_t search_scope[1024],search_result[1024],search_server_class[160];
static HWND search_server_window;
typedef struct delayed_search_reply {HWND target,server;DWORD id,bytes;BYTE packet[8192];} delayed_search_reply;
static HANDLE delayed_search_thread;
static DWORD WINAPI delayed_search(void *user)
{
    delayed_search_reply *s=user;Sleep(350);COPYDATASTRUCT reply={s->id,s->bytes,s->packet};DWORD_PTR result;
    SendMessageTimeoutW(s->target,WM_COPYDATA,(WPARAM)s->server,(LPARAM)&reply,SMTO_ABORTIFHUNG,1000,&result);free(s);return 0;
}
static LRESULT CALLBACK search_server_proc(HWND h,UINT m,WPARAM w,LPARAM l)
{
    if(m==WM_COPYDATA){
        COPYDATASTRUCT *cd=(void*)l;if(cd->dwData!=2||cd->cbData<22)return 0;
        DWORD q[5];memcpy(q,cd->lpData,sizeof q);const wchar_t *text=(const wchar_t*)((BYTE*)cd->lpData+20);
        InterlockedIncrement(&search_server_calls);int probe=wcsstr(text,L"folder:path:wfn:")!=NULL;
        LONG mode=InterlockedCompareExchange(&search_server_mode,0,0);
        if(mode==2)return 1; /* accepted, never replies */
        BYTE packet[8192]={0};DWORD *head=(DWORD*)packet;int count=probe?mode!=1:!wcsstr(text,L"zero");
        if(mode==3&&!probe){COPYDATASTRUCT reply={q[1],1,packet};SendMessageW((HWND)(UINT_PTR)q[0],WM_COPYDATA,(WPARAM)h,(LPARAM)&reply);return 1;}
        head[0]=head[3]=probe?count:0;head[1]=head[4]=probe?0:count;head[2]=head[5]=count;
        DWORD used=28;
        if(count){
            DWORD *item=(DWORD*)(packet+28);item[0]=probe?1:0;used+=12;
            wchar_t path[1024];wcscpy(path,probe?search_scope:search_result);wchar_t *slash=wcsrchr(path,L'\\');const wchar_t *name=slash?slash+1:path;
            item[1]=used;size_t bytes=(wcslen(name)+1)*sizeof(wchar_t);memcpy(packet+used,name,bytes);used+=(DWORD)bytes;
            if(slash)*slash=0;else path[0]=0;item[2]=used;bytes=(wcslen(path)+1)*sizeof(wchar_t);memcpy(packet+used,path,bytes);used+=(DWORD)bytes;
        }
        if(mode==4&&!probe&&wcsstr(text,L"needle")){
            delayed_search_reply *s=calloc(1,sizeof *s);if(!s)return 0;s->target=(HWND)(UINT_PTR)q[0];s->server=h;s->id=q[1];s->bytes=used;memcpy(s->packet,packet,used);
            delayed_search_thread=CreateThread(NULL,0,delayed_search,s,0,NULL);if(!delayed_search_thread){free(s);return 0;}return 1;
        }
        COPYDATASTRUCT reply={q[1],used,packet};SendMessageW((HWND)(UINT_PTR)q[0],WM_COPYDATA,(WPARAM)h,(LPARAM)&reply);return 1;
    }
    if(m==WM_DESTROY){PostQuitMessage(0);return 0;}return DefWindowProcW(h,m,w,l);
}
static DWORD WINAPI search_server(void *user)
{
    WNDCLASSW cls={.lpfnWndProc=search_server_proc,.hInstance=GetModuleHandleW(NULL),.lpszClassName=search_server_class};RegisterClassW(&cls);
    search_server_window=CreateWindowExW(0,search_server_class,L"",0,0,0,0,0,NULL,NULL,cls.hInstance,NULL);SetEvent(user);
    MSG msg;while(GetMessageW(&msg,NULL,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}return 0;
}
static HWND search_palette(void){return FindWindowExW(g.hwnd,NULL,L"OpenPilotPalette",NULL);}
static int search_property(const wchar_t *name){return (int)(UINT_PTR)GetPropW(search_palette(),name);}
static void search_type(const wchar_t *text)
{
    HWND edit=FindWindowExW(search_palette(),NULL,L"EDIT",NULL);SetWindowTextW(edit,text);
}
static int search_wait(unsigned ms)
{
    uint64_t until=GetTickCount64()+ms;
    do{drag_pump(10);if(search_property(L"Search.State")==2)return 1;}while(GetTickCount64()<until);
    wchar_t status[512];GetWindowTextW(search_palette(),status,512);
    printf("search deadline: state=%d backend=%d count=%d errors=%d status=%ls\n",search_property(L"Search.State"),search_property(L"Search.Backend"),search_property(L"Search.Count"),search_property(L"Search.Errors"),status);
    return 0;
}
static void search_open(const wchar_t *root,int live)
{
    fn_ctx c={.dir=root,.pane=&g.panes[0],.where=FN_PANE,.state=FN_S_FOLDER};fn_run_id(live?"palette.search_live":"palette.search",&c);drag_pump(10);
}
static void search_close(void){palette_close();uint64_t until=GetTickCount64()+3000;while(search_palette()&&GetTickCount64()<until)drag_pump(10);}

static void test_palette_revision_ui(void)
{
    puts("command palette: a row offered before settings reload cannot dispatch afterward");
    Pane *p=&g.panes[0];p_navigate(p,drag_left,NULL,0);drag_settle(p);
    fn_ctx c;pane_ctx(&c,p,FN_FROM_CODE,0);menu_context_calls=0;
    VERIFY("the command palette opens",fn_run_id("palette.commands",&c)&&search_palette());
    ctx_free(&c);
    search_type(L"Menu context probe");drag_pump(30);
    fn_settings(drag_settings,strlen(drag_settings));fn_configure_all();
    HWND edit=FindWindowExW(search_palette(),NULL,L"EDIT",NULL);
    if(edit)SendMessageW(edit,WM_KEYDOWN,VK_RETURN,0);
    VERIFY("the stale palette row invokes no function",menu_context_calls==0&&!search_palette());
    pane_ctx(&c,p,FN_FROM_CODE,0);
    VERIFY("the refreshed command palette opens",fn_run_id("palette.commands",&c)&&search_palette());
    ctx_free(&c);
    search_type(L"Menu context probe");drag_pump(30);
    edit=FindWindowExW(search_palette(),NULL,L"EDIT",NULL);
    if(edit)SendMessageW(edit,WM_KEYDOWN,VK_RETURN,0);
    VERIFY("the refreshed palette row invokes exactly once",menu_context_calls==1&&!search_palette());
    search_close();
}
static WNDPROC search_paint_proc;
static int search_suppressed_paints;
static LRESULT CALLBACK search_without_paint(HWND h,UINT m,WPARAM w,LPARAM l)
{
    if(m==WM_PAINT){search_suppressed_paints++;ValidateRect(h,NULL);return 0;}
    return CallWindowProcW(search_paint_proc,h,m,w,l);
}
static void test_search_routing_ui(void)
{
    puts("folder search: real overlay, named Everything IPC, fallback and lifecycle");
    wchar_t *root=drag_path(drag_root,L"search"),*deep=drag_path(root,L"nested"),*file=drag_path(deep,L"needle.txt");CreateDirectoryW(root,NULL);CreateDirectoryW(deep,NULL);drag_write(file,"needle");
    wcscpy(search_scope,root);wcscpy(search_result,file);
    wchar_t instance[64];swprintf(instance,64,L"OpenPilotRoute-%lu",GetCurrentProcessId());swprintf(search_server_class,160,L"EVERYTHING_TASKBAR_NOTIFICATION_(%ls)",instance);
    HANDLE ready=CreateEventW(NULL,TRUE,FALSE,NULL),server=CreateThread(NULL,0,search_server,ready,0,NULL);
    VERIFY("owned Everything fixture starts",server&&WaitForSingleObject(ready,3000)==WAIT_OBJECT_0&&search_server_window);CloseHandle(ready);ev_set_instance(instance);
    search_server_mode=0;search_open(root,0);search_type(L"needle");
    VERIFY("indexed Windows folder uses Everything",search_wait(3000)&&search_property(L"Search.Backend")==1&&search_property(L"Search.Count")==1);
    VERIFY("coverage and user search are separate queries",search_server_calls==2);
    search_type(L"zero");VERIFY("valid zero matches keep the indexed backend",search_wait(3000)&&search_property(L"Search.Backend")==1&&!search_property(L"Search.Count"));
    VERIFY("coverage is reused within the same folder",search_server_calls==3);search_close();
    search_server_mode=4;search_open(root,0);search_type(L"needle");drag_pump(200);search_type(L"zero");
    VERIFY("a newer indexed query can finish before the old reply",search_wait(3000)&&!search_property(L"Search.Count"));
    drag_pump(400);VERIFY("a delayed stale Everything reply cannot restore old results",search_property(L"Search.Backend")==1&&!search_property(L"Search.Count"));
    if(delayed_search_thread){WaitForSingleObject(delayed_search_thread,1000);CloseHandle(delayed_search_thread);delayed_search_thread=NULL;}search_close();
    search_server_mode=1;search_open(root,0);search_type(L"needle");
    VERIFY("missing index coverage falls back to a recursive live search",search_wait(3000)&&search_property(L"Search.Backend")==2&&search_property(L"Search.Count")==1);search_close();
    search_server_mode=3;search_open(root,0);search_type(L"needle");
    VERIFY("malformed Everything response falls back",search_wait(3000)&&search_property(L"Search.Backend")==2&&search_property(L"Search.Count")==1);search_close();
    search_server_mode=2;search_open(root,0);
    search_suppressed_paints=0;search_paint_proc=(WNDPROC)SetWindowLongPtrW(search_palette(),GWLP_WNDPROC,(LONG_PTR)search_without_paint);
    search_type(L"needle");
    VERIFY("a new query publishes Waiting before the next paint",search_property(L"Search.State")==1);
    VERIFY("unanswered Everything query falls back without blocking UI",search_wait(4500)&&search_property(L"Search.Backend")==2&&search_property(L"Search.Count")==1);
    VERIFY("fallback publishes results even while actual paints are withheld",search_suppressed_paints>0&&search_property(L"Search.State")==2&&search_property(L"Search.Count")==1);
    SetWindowLongPtrW(search_palette(),GWLP_WNDPROC,(LONG_PTR)search_paint_proc);InvalidateRect(search_palette(),NULL,FALSE);search_close();
    ev_set_instance(L"OpenPilotMissingInstance");search_open(root,0);search_type(L"needle");
    VERIFY("Everything absent still searches nested Windows files",search_wait(3000)&&search_property(L"Search.Backend")==2&&search_property(L"Search.Count")==1);search_close();
    ev_set_instance(instance);search_server_mode=0;LONG before=search_server_calls;search_open(root,1);search_type(L"needle");
    VERIFY("explicit live search bypasses the index",search_wait(3000)&&search_property(L"Search.Backend")==2&&search_server_calls==before);
    search_type(L"regex:.*");drag_pump(50);VERIFY("unsupported syntax has a visible query error",search_property(L"Search.State")==4&&!search_property(L"Search.Count"));
    search_type(L"needle");search_type(L"no-such-name");VERIFY("typing replaces a query without stale matches",search_wait(3000)&&!search_property(L"Search.Count"));
    search_type(L"needle");VERIFY("cleared error can search again",search_wait(3000)&&search_property(L"Search.Count")==1);
    HWND edit=FindWindowExW(search_palette(),NULL,L"EDIT",NULL);SendMessageW(edit,WM_KEYDOWN,VK_RETURN,0);drag_settle(&g.panes[0]);
    VERIFY("Enter reveals the actual nested file",same_path(g.panes[0].path,deep)&&!search_palette());
    search_open(root,1);search_type(L"needle");SendMessageW(search_palette(),WM_TIMER,2,0);search_close();VERIFY("closing a running scan releases the overlay",!search_palette());
    wchar_t native_root[MAX_PATH];DWORD native_len=GetEnvironmentVariableW(L"OPENPILOT_DRAG_WSL_DEST",native_root,MAX_PATH);
    if(native_len&&native_len<MAX_PATH){
        wchar_t *native=drag_path(native_root,L"search-fixture");fpe_err create_error=fpe_wsl_create(native,1,NULL);
        if(create_error)printf("native create error=%u root=%ls\n",create_error,native);
        VERIFY("native search fixture is created",create_error==0);
        wchar_t *native_file=drag_path(native,L"needle.txt");create_error=fpe_wsl_create(native_file,0,NULL);
        if(create_error)printf("native file error=%u\n",create_error);VERIFY("native search file is created",create_error==0);
        search_open(native,0);search_type(L"needle");int completed=search_wait(5000);
        if(!completed||search_property(L"Search.Count")!=1){wchar_t status[256];GetWindowTextW(search_palette(),status,256);printf("native search backend=%d state=%d count=%d errors=%d status=%ls\n",search_property(L"Search.Backend"),search_property(L"Search.State"),search_property(L"Search.Count"),search_property(L"Search.Errors"),status);}
        VERIFY("WSL paths bypass Everything and search natively",completed&&search_property(L"Search.Backend")==2&&search_property(L"Search.Count")==1);search_close();free(native_file);free(native);
    }else VERIFY("native search fixture is provided",0);
    PostMessageW(search_server_window,WM_CLOSE,0,0);VERIFY("owned Everything fixture stops",WaitForSingleObject(server,3000)==WAIT_OBJECT_0);CloseHandle(server);ev_set_instance(NULL);
    free(root);free(deep);free(file);
}

static int index_wait(int wanted,unsigned ms)
{
    uint64_t until=GetTickCount64()+ms;MSG m;
    do{
        while(PeekMessageW(&m,NULL,0,0,PM_REMOVE))if(m.message!=WM_QUIT){
            TranslateMessage(&m);dispatch_timed(&m);
            /* Paint each published transition before the next worker reply.
             * Draining all replies first can hide freed result-row pointers. */
            HWND palette=search_palette();
            if(palette&&GetUpdateRect(palette,NULL,FALSE)){MSG paint={.hwnd=palette,.message=WM_PAINT};dispatch_timed(&paint);}
        }
        if(search_property(L"Index.State")==wanted)return 1;Sleep(2);
    }while(GetTickCount64()<until);return 0;
}
static DWORD WINAPI index_enable_menu(void *arg)
{
    int *picked=arg;HWND menu=NULL;uint64_t until=GetTickCount64()+5000;
    while(!menu&&GetTickCount64()<until){menu=drag_menu_window();Sleep(5);}
    if(!menu)return 0;
    HMENU handle=(HMENU)SendMessageW(menu,MN_GETHMENU,0,0);wchar_t text[100];
    GetMenuStringW(handle,0,text,100,MF_BYPOSITION);
    *picked=!wcscmp(text,L"Index this folder and subfolders");
    /* Exercise real user wait in the nested menu loop. */
    Sleep(100);PostMessageW(menu,WM_KEYDOWN,VK_HOME,0);PostMessageW(menu,WM_KEYDOWN,*picked?VK_RETURN:VK_ESCAPE,0);
    return 0;
}
static void test_wsl_index_ui(void)
{
    puts("optional WSL index: real overlay controls, current/live routing and pause lifecycle");
    wchar_t native_root[MAX_PATH];DWORD n=GetEnvironmentVariableW(L"OPENPILOT_DRAG_WSL_DEST",native_root,MAX_PATH);
    VERIFY("index UI receives an owned WSL fixture",n&&n<MAX_PATH);if(!n||n>=MAX_PATH)return;
    wchar_t *root=drag_path(native_root,L"index-fixture"),*file=drag_path(root,L"needle.txt");
    VERIFY("index UI fixture directory is created",!fpe_wsl_create(root,1,NULL));
    VERIFY("index UI fixture file is created",!fpe_wsl_create(file,0,NULL));
    search_open(root,0);drag_pump(300);
    VERIFY("WSL overlay exposes the index menu",IsWindowVisible(GetDlgItem(search_palette(),1000)));
    VERIFY("indexing begins off",!search_property(L"Index.Enabled"));
    int picked=0;HANDLE menu_worker=CreateThread(NULL,0,index_enable_menu,&picked,0,NULL);
    if(menu_worker){SendMessageW(search_palette(),WM_COMMAND,1000,(LPARAM)GetDlgItem(search_palette(),1000));WaitForSingleObject(menu_worker,5000);CloseHandle(menu_worker);}
    VERIFY("explicit enable through the real menu reaches current state",picked&&index_wait(FPE_INDEX_CURRENT,6000)&&search_property(L"Index.Entries")==1);
    search_type(L"needle");VERIFY("current selected folder uses the WSL index",search_wait(5000)&&search_property(L"Search.Backend")==3&&search_property(L"Search.Count")==1);
    search_type(L"ne");VERIFY("short queries retain live search",search_wait(5000)&&search_property(L"Search.Backend")==2&&search_property(L"Search.Count")==1);
    search_type(L"needle size:=0");VERIFY("metadata queries use live values",search_wait(5000)&&search_property(L"Search.Backend")==2&&search_property(L"Search.Count")==1);
    search_type(L"needle");VERIFY("safe name query returns to the index",search_wait(5000)&&search_property(L"Search.Backend")==3);
    SendMessageW(search_palette(),WM_COMMAND,1102,0);
    VERIFY("pause is visible and falls back to live",index_wait(FPE_INDEX_PAUSED,5000)&&search_wait(5000)&&search_property(L"Search.Backend")==2);
    SendMessageW(search_palette(),WM_COMMAND,1103,0);
    VERIFY("resume restores current coverage",index_wait(FPE_INDEX_CURRENT,5000)&&search_wait(5000)&&search_property(L"Search.Backend")==3);
    search_close();search_open(root,1);search_type(L"needle");
    VERIFY("explicit live bypasses an enabled WSL index",search_wait(5000)&&search_property(L"Search.Backend")==2);
    SendMessageW(search_palette(),WM_COMMAND,1105,0);
    VERIFY("remove turns indexing off",index_wait(FPE_INDEX_NONE,5000)&&!search_property(L"Index.Enabled"));
    search_close();fpe_search_index_shutdown();free(root);free(file);
}

static int watch_wait_visible(Pane *p,const wchar_t *name,int exists)
{
    uint64_t until=GetTickCount64()+5000;
    while(GetTickCount64()<until){
        if(p->view && (fpe_view_find(p->view,name)!=UINT32_MAX)==exists)return 1;
        drag_pump(10);
    }
    return 0;
}
static void save_fixture_write(const char *text)
{
    HANDLE f=CreateFileW(g.settings,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                         NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);DWORD n=0;
    VERIFY("settings fixture writes completely",f!=INVALID_HANDLE_VALUE&&WriteFile(f,text,(DWORD)strlen(text),&n,NULL)&&n==strlen(text));
    if(f!=INVALID_HANDLE_VALUE)CloseHandle(f);
}
static void save_hold(void)
{
    ResetEvent(save_flush_entered);ResetEvent(save_flush_release);
    InterlockedExchange(&save_flush_action,1);
}
static int save_finish(void)
{
    uint64_t until=GetTickCount64()+5000;
    while(g_settings_save && WaitForSingleObject(g_settings_save->thread,0)!=WAIT_OBJECT_0 && GetTickCount64()<until)drag_pump(5);
    MSG m={.hwnd=g.hwnd,.message=WM_APP_SETTINGS_SAVED};dispatch_timed(&m);
    return g_settings_save==NULL;
}
static int save_file_has(const char *part)
{
    char *text=NULL;size_t len=0;int read=settings_read(&text,&len);if(text)text[len]=0;
    int ok=!read&&text&&strstr(text,part)!=NULL;
    free(text);return ok;
}
static void test_settings_commit_ui(void)
{
    puts("custom command save: durable IO leaves the UI thread; conflicts and helper generations");
    const char *base="; retain this comment\n[functions]\npreview.image = off\npreview.pdf = off\npreview.text = off\noperations.show = off\nsvc.icons = off\n[panel.side]\nslide = off\n";
    wchar_t prior[MAX_PATH+32];wcscpy(prior,g.settings);uint64_t prior_hash=g_cfg_hash;
    _snwprintf(g.settings,MAX_PATH+32,L"%ls\\settings.ini",drag_root);
    save_fixture_write(base);settings_apply(base,strlen(base));configure_panel();
    save_flush_entered=CreateEventW(NULL,TRUE,FALSE,NULL);save_flush_release=CreateEventW(NULL,TRUE,FALSE,NULL);
    VERIFY("durable-write boundary events exist",save_flush_entered&&save_flush_release);
    sidepanel_show(g.hwnd,settings_app(),0,0);
    uint32_t original=sidepanel_generation(settings_app());
    VERIFY("settings helper has an instance identity",original!=0);
    relay_draft d;relay_draft_init(&d);d.request=1;d.revision=fn_revision();
    strcpy(d.name,"async_save_probe");strcpy(d.title,"Held save");strcpy(d.program,"cmd");strcpy(d.arguments,"/d /c exit 0");
    save_hold();save_observe_replies=1;save_replies=0;
    settings_relay_save(&d);
    int entered=WaitForSingleObject(save_flush_entered,5000)==WAIT_OBJECT_0;
    VERIFY("save returns while durable write remains held",entered&&g_settings_save&&WaitForSingleObject(save_flush_release,0)==WAIT_TIMEOUT);
    VERIFY("the held flush runs on a worker",entered&&save_flush_thread!=GetCurrentThreadId());
    VERIFY("uncommitted command is absent from disk and live registry",!save_file_has("[relay.async_save_probe]")&&!fn_find("relay.async_save_probe"));
    VERIFY("no success reply precedes durable completion",save_replies==0);
    settings_save *pending=g_settings_save;
    settings_relay_save(&d);
    VERIFY("a second command cannot replace the pending transaction",pending&&g_settings_save==pending&&save_replies==1);
    char toggle[]="2\t0\nfunctions\tpreview.text\tset\ton\n";settings_set(toggle);
    VERIFY("other settings writes are serialized behind the pending commit",g_settings_save==pending&&save_replies==2&&!save_file_has("preview.text = on"));
    save_replies=0;
    settings_check();
    VERIFY("watcher does not reload halfway through the pending transaction",g_settings_save==pending&&!fn_find("relay.async_save_probe"));
    MSG alive={.hwnd=g.hwnd,.message=WM_NULL};dispatch_timed(&alive);
    VERIFY("UI still dispatches messages while storage is held",WaitForSingleObject(save_flush_release,0)==WAIT_TIMEOUT);
    sidepanel_close();
    VERIFY("closing Settings drops its instance without waiting for storage",sidepanel_generation(settings_app())==0&&WaitForSingleObject(save_flush_release,0)==WAIT_TIMEOUT);
    sidepanel_show(g.hwnd,settings_app(),0,0);
    uint32_t fresh=sidepanel_generation(settings_app());
    VERIFY("reopened Settings has a different instance identity",fresh&&fresh!=original);
    SetEvent(save_flush_release);
    VERIFY("accepted save completes after its submitting helper closes",save_finish());
    VERIFY("durable command and original comments are preserved",save_file_has("[relay.async_save_probe]")&&save_file_has("; retain this comment")&&fn_find("relay.async_save_probe"));
    VERIFY("old completion cannot acknowledge a reused request in a new helper",save_replies==0);
    d.editing=1;d.revision=fn_revision();strcpy(d.title,"Failed save");
    save_fail_start=1;save_replies=0;settings_relay_save(&d);
    VERIFY("thread creation failure reports failure without retaining a transaction",!g_settings_save&&save_replies==1&&!save_fail_start);
    VERIFY("failed worker creation leaves committed settings intact",save_file_has("title = Held save")&&!save_file_has("Failed save"));
    InterlockedExchange(&save_flush_action,2);save_replies=0;settings_relay_save(&d);
    VERIFY("failed flush is reported after worker completion",save_finish()&&save_replies==1);
    const fn_def *saved=fn_find("relay.async_save_probe");
    VERIFY("failed durability leaves disk and live command unchanged",save_file_has("title = Held save")&&!save_file_has("Failed save")&&saved&&!wcscmp(saved->title,L"Held save"));
    d.revision=fn_revision();strcpy(d.title,"Conflicting save");save_hold();save_replies=0;settings_relay_save(&d);
    VERIFY("conflict test reaches its held durable-write boundary",WaitForSingleObject(save_flush_entered,5000)==WAIT_OBJECT_0);
    char *current=NULL;size_t len=0;settings_read(&current,&len);
    char *external=malloc(len+64);if(external){memcpy(external,current,len);strcpy(external+len,"\n; external edit wins\n");save_fixture_write(external);}free(current);
    SetEvent(save_flush_release);
    VERIFY("external edit causes a completed conflict reply",save_finish()&&save_replies==1);
    VERIFY("external edit is retained without writing the conflicting draft",external&&save_file_has("external edit wins")&&!save_file_has("Conflicting save"));
    VERIFY("conflict reload adopts the actual file hash",external&&g_cfg_hash==bytes_hash(external,strlen(external)));free(external);
    d.revision=fn_revision();strcpy(d.title,"Retried save");save_replies=0;settings_relay_save(&d);
    uint64_t modal_until=GetTickCount64()+350;
    while(g_settings_save&&GetTickCount64()<modal_until)drag_pump(5); /* nested loop, no outer handle wait */
    VERIFY("a nested UI loop receives completion without returning to the outer handle wait",g_settings_save==NULL&&save_replies==1);
    VERIFY("retained draft can be retried after conflict",save_finish()&&save_replies==1&&save_file_has("title = Retried save")&&save_file_has("external edit wins"));
    wchar_t temp[MAX_PATH+64];WIN32_FIND_DATAW found;
    _snwprintf(temp,MAX_PATH+64,L"%ls.*.tmp",g.settings);
    HANDLE leftover=FindFirstFileW(temp,&found);
    VERIFY("completed saves leave no temporary settings file",leftover==INVALID_HANDLE_VALUE);
    if(leftover!=INVALID_HANDLE_VALUE)FindClose(leftover);
    sidepanel_close();save_observe_replies=0;settings_save_join();
    CloseHandle(save_flush_entered);CloseHandle(save_flush_release);save_flush_entered=save_flush_release=NULL;
    settings_apply(base,strlen(base));settings_watch_stop();wcscpy(g.settings,prior);g_cfg_hash=prior_hash;
}

static int settings_race_child(int argc, wchar_t **argv)
{
    if (argc != 7) return 30;
    race_entered = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[5]);
    race_release = OpenEventW(SYNCHRONIZE, FALSE, argv[6]);
    if (!race_entered || !race_release) return 31;
    char text[64];
    int len = snprintf(text, sizeof text, "; baseline\n; writer %lc\n", argv[4][0]);
    uint64_t expect = _wcstoui64(argv[3], NULL, 10);
    int rc = settings_write(argv[2], text, (size_t)len, &expect);
    CloseHandle(race_entered); CloseHandle(race_release);
    return rc == 0 ? 10 : rc == -2 ? 20 : 30;
}

static int settings_race_spawn(const wchar_t *path, uint64_t expect, wchar_t marker,
                              const wchar_t *entered, const wchar_t *release, PROCESS_INFORMATION *pi)
{
    wchar_t exe[MAX_PATH], line[MAX_PATH * 4];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    _snwprintf(line, MAX_PATH * 4,
               L"\"%ls\" --settings-race-child \"%ls\" %llu %lc \"%ls\" \"%ls\"",
               exe, path, (unsigned long long)expect, marker, entered, release);
    STARTUPINFOW si = { .cb = sizeof si };
    memset(pi, 0, sizeof *pi);
    if (!CreateProcessW(exe, line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, pi)) return 0;
    CloseHandle(pi->hThread);
    return 1;
}

static DWORD settings_race_exit(PROCESS_INFORMATION *pi)
{
    if (!pi->hProcess) return 99;
    DWORD wait = WaitForSingleObject(pi->hProcess, 10000), code = 99;
    if (wait != WAIT_OBJECT_0) { TerminateProcess(pi->hProcess, 99); WaitForSingleObject(pi->hProcess, 2000); }
    GetExitCodeProcess(pi->hProcess, &code);
    CloseHandle(pi->hProcess);
    return code;
}

static void test_settings_race_processes(void)
{
    puts("settings saves: two processes and an external atomic editor");
    wchar_t *dir = drag_path(drag_root, L"settings-race");
    wchar_t *path = drag_path(dir, L"settings.ini");
    wchar_t alias[MAX_PATH + 64], enter_name[2][128], release_name[2][128];
    HANDLE entered[2] = { NULL }, released[2] = { NULL };
    PROCESS_INFORMATION proc[2] = {0};
    const char base[] = "; baseline\n";
    VERIFY("settings race fixture directory is created",CreateDirectoryW(dir,NULL));
    drag_write(path,base);
    _snwprintf(alias,MAX_PATH+64,L"%ls\\.\\settings.ini",dir);
    for (int i=0;i<2;i++) {
        _snwprintf(enter_name[i],128,L"Local\\OpenPilotSettingsRace-%lu-%d-enter",GetCurrentProcessId(),i);
        _snwprintf(release_name[i],128,L"Local\\OpenPilotSettingsRace-%lu-%d-release",GetCurrentProcessId(),i);
        entered[i]=CreateEventW(NULL,TRUE,FALSE,enter_name[i]);
        released[i]=CreateEventW(NULL,TRUE,FALSE,release_name[i]);
    }
    VERIFY("two-process save barriers exist",entered[0]&&entered[1]&&released[0]&&released[1]);
    uint64_t expect=bytes_hash(base,sizeof base-1);
    int first=settings_race_spawn(path,expect,L'A',enter_name[0],release_name[0],&proc[0]);
    VERIFY("first writer starts",first);
    int held=first&&WaitForSingleObject(entered[0],5000)==WAIT_OBJECT_0;
    VERIFY("first writer reaches the final replacement barrier",held);
    int second=held&&settings_race_spawn(alias,expect,L'B',enter_name[1],release_name[1],&proc[1]);
    VERIFY("second writer starts through a normalized path alias",second);
    int raced=second&&WaitForSingleObject(entered[1],350)==WAIT_OBJECT_0;
    VERIFY("second process cannot pass validation while first owns the file mutex",second&&!raced);
    SetEvent(released[0]); SetEvent(released[1]);
    DWORD a=settings_race_exit(&proc[0]),b=settings_race_exit(&proc[1]);
    VERIFY("one writer commits and the other reports conflict",a==10&&b==20);
    char *actual=NULL;size_t n=0;
    int read=settings_read_path(path,&actual,&n);
    VERIFY("competing edit never overwrites the committed one",!read&&actual&&n==strlen("; baseline\n; writer A\n")&&
           !memcmp(actual,"; baseline\n; writer A\n",n));
    free(actual);

    /* An editor that ignores our mutex cannot replace the file while the
     * guarded read is open. It can retry after the save has finished. */
    ResetEvent(entered[0]); ResetEvent(released[0]);
    settings_read_path(path,&actual,&n);expect=bytes_hash(actual?actual:"",n);free(actual);
    int third=settings_race_spawn(path,expect,L'C',enter_name[0],release_name[0],&proc[0]);
    VERIFY("writer reaches the external-edit replacement barrier",third&&WaitForSingleObject(entered[0],5000)==WAIT_OBJECT_0);
    wchar_t *external=drag_path(dir,L"external.tmp");
    drag_write(external,"; external atomic edit\n");
    int swapped=MoveFileExW(external,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
    VERIFY("noncooperating editor cannot replace the guarded file",!swapped);
    SetEvent(released[0]);
    DWORD c=settings_race_exit(&proc[0]);
    VERIFY("guarded writer commits without losing the blocked external edit",c==10&&drag_exists(external));
    int retried=MoveFileExW(external,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
    actual=NULL;n=0;read=settings_read_path(path,&actual,&n);
    VERIFY("external editor can retry after the save without losing its edit",retried&&!read&&actual&&
           n==strlen("; external atomic edit\n")&&!memcmp(actual,"; external atomic edit\n",n));
    free(actual);free(external);
    for(int i=0;i<2;i++){if(entered[i])CloseHandle(entered[i]);if(released[i])CloseHandle(released[i]);}
    DeleteFileW(path);RemoveDirectoryW(dir);free(path);free(dir);
}

static void test_folder_watch_ui(void)
{
    puts("folder notifications: real Windows and WSL changes refresh the shown pane");
    wchar_t linux_root[MAX_PATH];DWORD n=GetEnvironmentVariableW(L"OPENPILOT_DRAG_WSL_DEST",linux_root,MAX_PATH);
    VERIFY("folder watching receives an owned Linux fixture",n&&n<MAX_PATH);if(!n||n>=MAX_PATH)return;
    Pane *p=&g.panes[0];
    for(int linux=0;linux<2;linux++){
        wchar_t *root=drag_path(linux?linux_root:drag_root,L"watch-fixture");
        wchar_t *file=drag_path(root,L"external-change.txt");
        VERIFY("the watched fixture directory is created",CreateDirectoryW(root,NULL));
        if(linux){
            /* Warm the native listing/watch helper before this event fixture.
             * Empty snapshots have no per-entry Unix metadata, so check the
             * native metadata flag only after the external file appears. */
            fpe_dir *ready=NULL;fpe_err err=fpe_wsl_read(root,&ready);
            VERIFY("the native listing helper is ready for the inotify fixture",!err&&ready);
            fpe_dir_release(ready);
        }
        p_navigate(p,root,NULL,0);drag_settle(p);drag_pump(300);
        VERIFY("navigation installs the intended Windows or native Linux watcher path",same_path(p->path,root)&&fpe_wsl_available(p->path)==linux);
        drag_write(file,"external change");
        VERIFY(linux?"inotify refreshes a native entry created outside the pane":"ReadDirectoryChanges refreshes a file created outside the pane",watch_wait_visible(p,L"external-change.txt",1)&&p_linux(p)==linux);
        VERIFY("the external file is deleted",DeleteFileW(file));
        VERIFY(linux?"inotify removes the deleted row without manual refresh":"ReadDirectoryChanges removes the deleted row without manual refresh",watch_wait_visible(p,L"external-change.txt",0));
        free(root);free(file);
    }
    p_navigate(p,drag_left,NULL,0);drag_settle(p);
}

static unsigned gesture_serial;
static wchar_t *gesture_fixture(Pane *p,int with_a)
{
    wchar_t leaf[80];_snwprintf(leaf,80,L"gesture-%u",++gesture_serial);wchar_t *root=drag_path(drag_root,leaf);
    VERIFY("gesture fixture directory is newly owned",CreateDirectoryW(root,NULL));
    const wchar_t *names[]={L"a.txt",L"b.txt",L"c.txt"};
    for(int i=with_a?0:1;i<3;i++){wchar_t *path=drag_path(root,names[i]);drag_write(path,"owned gesture bytes");free(path);}
    p_navigate(p,root,NULL,0);drag_settle(p);
    p->filter_len=0;p->filter[0]=0;p_filter_changed(p);
    p->opts.key=FPE_SORT_NAME;p->opts.descending=0;p_keep_cursor_after(p,apply_opts);
    p_clear_sel(p);uint32_t b=fpe_view_find(p->view,L"b.txt"),c=fpe_view_find(p->view,L"c.txt");
    VERIFY("both selected names are visible",b!=UINT32_MAX&&c!=UINT32_MAX);
    if(b!=UINT32_MAX&&c!=UINT32_MAX){p_set_sel(p,b,1);p_set_sel(p,c,1);p->cursor=p->anchor=c;}
    p->down_armed=p->down_deferred=p->right_suppress=0;gesture_calls=0;gesture_count=0;gesture_observe=1;return root;
}
static void gesture_selected(Pane *p,int a,int b,int c)
{
    const wchar_t *names[]={L"a.txt",L"b.txt",L"c.txt"};int expected[]={a,b,c};
    VERIFY("selection count matches intended names",p->nsel==(uint32_t)(a+b+c));
    for(int i=0;i<3;i++){uint32_t row=fpe_view_find(p->view,names[i]);VERIFY("selection is preserved by name",(row!=UINT32_MAX&&p_is_sel(p,row))==expected[i]);}
    wchar_t **paths=NULL;uint32_t count=p_targets(p,&paths,NULL);VERIFY("operation targets match selected-name count",count==(uint32_t)(a+b+c));
    for(uint32_t i=0;i<count;i++){const wchar_t *name=wcsrchr(paths[i],L'\\');name=name?name+1:paths[i];int found=0;for(int k=0;k<3;k++)if(expected[k]&&!wcscmp(name,names[k]))found++;VERIFY("every target is an intended selected name",found==1);free(paths[i]);}free(paths);
}
static void gesture_press(Pane *p,const wchar_t *name,int right,WPARAM modifiers)
{
    uint32_t row=fpe_view_find(p->view,name);VERIFY("gesture press resolves current name",row!=UINT32_MAX);if(row==UINT32_MAX)return;
    int x=S(65),y=p_list_top(p)+((int)row-p->top)*g.row_h+g.row_h/2;
    VERIFY("gesture press hits intended current row",p_row_at(p,x,y)==(int)row);
    drag_mouse(p->hwnd,right?WM_RBUTTONDOWN:WM_LBUTTONDOWN,(right?MK_RBUTTON:MK_LBUTTON)|modifiers,x,y);
}
static void gesture_move(Pane *p,int right)
{
    drag_mouse(p->hwnd,WM_MOUSEMOVE,right?MK_RBUTTON:MK_LBUTTON,p->down_x+GetSystemMetrics(SM_CXDRAG)+S(16),p->down_y);
}
static void gesture_release(Pane *p,int right)
{
    drag_mouse(p->hwnd,right?WM_RBUTTONUP:WM_LBUTTONUP,0,p->down_x,p->down_y);
}
static void gesture_refresh(Pane *p,const wchar_t *root)
{
    fpe_dir *snapshot=NULL;fpe_err err=fpe_dir_read(root,&snapshot);VERIFY("synchronous owned-folder read succeeds without fallback",!err&&snapshot);
    if(!err&&snapshot)p_refresh_in_place(p,snapshot);fpe_dir_release(snapshot);
}
static void test_gesture_refresh_release_preserves_selected_names(void)
{
    puts("pending gesture: insertion/removal before release preserves restored selected names");
    Pane *p=&g.panes[0];
    for(int remove=0;remove<2;remove++){
        wchar_t *root=gesture_fixture(p,remove);gesture_press(p,L"c.txt",0,0);VERIFY("multiselection collapse is deferred",p->down_armed&&p->down_deferred);
        wchar_t *a=drag_path(root,L"a.txt");if(remove)VERIFY("owned preceding row is removed",DeleteFileW(a));else drag_write(a,"inserted before selected rows");
        gesture_refresh(p,root);gesture_release(p,0);gesture_selected(p,0,1,1);VERIFY("release starts no drag",gesture_calls==0);free(a);free(root);
    }
    gesture_observe=0;
}
static void test_gesture_changed_view_refuses_left_and_right_drag(void)
{
    puts("pending gesture: refresh/sort/filter revisions reject both button drag starts");
    Pane *p=&g.panes[0];
    for(int right=0;right<2;right++)for(int change=0;change<3;change++){
        wchar_t *root=gesture_fixture(p,0);gesture_press(p,L"c.txt",right,0);VERIFY("file gesture is armed",p->down_armed);
        if(change==0)gesture_refresh(p,root);else if(change==1)p_set_sort(p,FPE_SORT_NAME);else {wcscpy(p->filter,L".txt");p->filter_len=4;p_filter_changed(p);}
        gesture_move(p,right);VERIFY("stale gesture never reaches OLE dispatch",gesture_calls==0);VERIFY("stale drag flags clear without context suppression",!p->down_armed&&!p->down_deferred&&!p->right_suppress);
        /* A canceled right gesture may open its ordinary context menu on release;
         * cancel this fixture interaction instead. Valid right-release suppression
         * is exercised separately below and ordinary menus remain a full-suite lane. */
        if(right)SendMessageW(p->hwnd,WM_CANCELMODE,0,0);else gesture_release(p,0);gesture_selected(p,0,1,1);free(root);
    }
    gesture_observe=0;
}
static void test_gesture_navigation_and_tab_cancel_before_view_change(void)
{
    puts("pending gesture: navigation/tab entry cancels even before a new view arrives");
    Pane *p=&g.panes[0];wchar_t *root=gesture_fixture(p,0);wchar_t *child=drag_path(root,L"uncached");VERIFY("owned uncached destination exists",CreateDirectoryW(child,NULL));
    gesture_press(p,L"c.txt",0,0);uint64_t revision=fpe_view_revision(p->view);p_navigate(p,child,NULL,0);
    VERIFY("navigation has not yet replaced the current view",fpe_view_revision(p->view)==revision);
    VERIFY("navigation immediately cancels pending mouse state",!p->down_armed&&!p->down_deferred);gesture_move(p,0);VERIFY("pending navigation cannot dispatch old paths",gesture_calls==0);gesture_release(p,0);drag_settle(p);
    p_navigate(p,root,NULL,0);drag_settle(p);p_clear_sel(p);p_set_sel(p,fpe_view_find(p->view,L"b.txt"),1);p_set_sel(p,fpe_view_find(p->view,L"c.txt"),1);
    gesture_press(p,L"c.txt",1,0);int previous=p->tab,index=p->ntabs;VERIFY("private pathless tab slot is available",index<TABS_MAX);
    if(index<TABS_MAX){memset(&p->tabs[index],0,sizeof p->tabs[index]);p->tabs[index].opts=p->opts;p->ntabs++;revision=fpe_view_revision(p->view);tab_show(p,index);
        VERIFY("pathless same-options tab keeps view revision",fpe_view_revision(p->view)==revision);VERIFY("tab entry cancels pending gesture independently of navigation",!p->down_armed&&!p->down_deferred);
        gesture_move(p,1);VERIFY("tab change cannot dispatch prior paths",gesture_calls==0);SendMessageW(p->hwnd,WM_CANCELMODE,0,0);p->ntabs--;p->tab=previous;memset(&p->tabs[index],0,sizeof p->tabs[index]);}
    gesture_observe=0;free(child);free(root);
}
static void test_gesture_unchanged_click_and_fresh_press_controls(void)
{
    puts("pending gesture controls: ordinary release collapses, fresh modified press clears prior deferral");
    Pane *p=&g.panes[0];wchar_t *root=gesture_fixture(p,1);gesture_press(p,L"c.txt",0,0);gesture_release(p,0);gesture_selected(p,0,0,1);
    p_set_sel(p,fpe_view_find(p->view,L"b.txt"),1);gesture_press(p,L"c.txt",0,0);VERIFY("first selected press defers collapse",p->down_deferred);
    gesture_press(p,L"a.txt",0,MK_CONTROL);gesture_release(p,0);gesture_selected(p,1,1,1);VERIFY("ordinary click controls never dispatch OLE",gesture_calls==0);
    gesture_observe=0;free(root);
}
static void test_gesture_unchanged_left_and_right_drag_controls(void)
{
    puts("pending gesture controls: unchanged left/right drags dispatch exact selected paths once");
    Pane *p=&g.panes[0];
    for(int right=0;right<2;right++){
        wchar_t *root=gesture_fixture(p,0);gesture_press(p,L"c.txt",right,0);gesture_move(p,right);
        VERIFY("unchanged gesture dispatches exactly once with correct button",gesture_calls==1&&gesture_count==2&&gesture_right==right);
        wchar_t *b=drag_path(root,L"b.txt"),*c=drag_path(root,L"c.txt");VERIFY("OLE preparation receives exact selected paths",!wcscmp(gesture_paths[0],b)&&!wcscmp(gesture_paths[1],c));
        gesture_release(p,right);gesture_selected(p,0,1,1);VERIFY("completed drag clears pending state and retains right suppression",!p->down_armed&&!p->down_deferred&&p->right_suppress==right);
        if(right){drag_mouse(p->hwnd,WM_CONTEXTMENU,0,0,0);VERIFY("right-drag release suppresses only its generated context menu",!p->right_suppress);}
        free(b);free(c);free(root);
    }
    gesture_observe=0;
}
/* The stale source gesture is observed at dispatch; following menu/drop
 * interactions use real production modal loops and the registered OLE target.
 * No physical pointer or keyboard input is sent outside the owned test host. */
static int gesture_file_bytes(const wchar_t *path)
{
    const char expected[]="owned gesture bytes";char bytes[sizeof expected]={0};DWORD n=0;
    HANDLE f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    if(f==INVALID_HANDLE_VALUE)return 0;
    int ok=ReadFile(f,bytes,sizeof bytes,&n,NULL)&&n==sizeof expected-1&&!memcmp(bytes,expected,n);
    CloseHandle(f);return ok;
}
static void test_gesture_changed_view_modal_context_and_drop(void)
{
    gesture_modal_trace=1;
    puts("N11 changed-view gesture: real ordinary release menu, OLE cancel and selected-path copy");
    Pane *p=&g.panes[0],*target=&g.panes[1];
    for(int right=0;right<2;right++)for(int change=0;change<3;change++){
        wchar_t *root=gesture_fixture(p,0);gesture_press(p,L"c.txt",right,0);
        uint64_t before=fpe_view_revision(p->view);
        wchar_t *a=drag_path(root,L"a.txt");
        if(change==0){drag_write(a,"preceding insertion");gesture_refresh(p,root);}
        else if(change==1)p_set_sort(p,FPE_SORT_NAME);
        else {wcscpy(p->filter,L".txt");p->filter_len=4;p_filter_changed(p);}
        VERIFY("N11 fixture changes the pending gesture's actual view revision",before!=fpe_view_revision(p->view));
        gesture_move(p,right);
        VERIFY("N11 stale gesture refuses source OLE dispatch",gesture_calls==0);
        VERIFY("N11 stale gesture clears pending state without suppressing a regular menu",!p->down_armed&&!p->down_deferred&&!p->right_suppress);
        if(!right){gesture_release(p,0);gesture_selected(p,0,1,1);gesture_press(p,L"c.txt",1,0);}
        drag_menu_pick ordinary={.choice=-2};
        HANDLE thread=CreateThread(NULL,0,drag_pick_worker,&ordinary,0,NULL);
        VERIFY("N11 owned regular-menu observer starts",thread!=NULL);
        uint32_t seq=g.modal_seq;
        if(thread){
            /* For a stale right gesture this is the original release, with
             * no WM_CANCELMODE or substitute WM_CONTEXTMENU injected. */
            gesture_release(p,1);
            DWORD ended=WaitForSingleObject(thread,6000);CloseHandle(thread);
            VERIFY("N11 owned regular-menu observer finishes",ended==WAIT_OBJECT_0);
            VERIFY("N11 ordinary release opens Properties menu rather than drop actions",ordinary.seen&&ordinary.correct);
            VERIFY("N11 regular menu enters and exits the real modal loop",g.modal_seq>=seq+2);
        }else SendMessageW(p->hwnd,WM_CANCELMODE,0,0);
        VERIFY("N11 ordinary release leaves no armed drag or suppression",!p->down_armed&&!p->down_deferred&&!p->right_suppress);
        gesture_selected(p,0,1,1);gesture_observe=0;
        wchar_t leaf[80];_snwprintf(leaf,80,L"modal-target-%u",gesture_serial);
        wchar_t *dest=drag_path(drag_root,leaf);VERIFY("N11 OLE destination is newly owned",CreateDirectoryW(dest,NULL));
        p_navigate(target,dest,NULL,0);drag_settle(target);
        wchar_t **paths=NULL;uint32_t n=p_targets(p,&paths,NULL);
        VERIFY("N11 real drop takes both current selected paths",n==2);
        wchar_t *b_out=drag_path(dest,L"b.txt"),*c_out=drag_path(dest,L"c.txt"),*a_out=drag_path(dest,L"a.txt");
        if(n==2){
            drag_menu_pick cancel={.choice=-1};seq=g.modal_seq;
            DWORD effect=drag_drop_pick_paths(target,(const wchar_t *const *)paths,n,1,&cancel);
            VERIFY("N11 real OLE release cancellation returns no effect",effect==DROPEFFECT_NONE&&cancel.seen&&cancel.correct);
            VERIFY("N11 cancelled drop enters and exits its real modal loop",g.modal_seq>=seq+2);
            VERIFY("N11 cancelled drop leaves both current sources and destination intact",drag_exists(paths[0])&&drag_exists(paths[1])&&!drag_exists(b_out)&&!drag_exists(c_out));
            uint64_t prior_job=gesture_modal_job.job;
            drag_menu_pick copy={.choice=0};seq=g.modal_seq;
            effect=drag_drop_pick_paths(target,(const wchar_t *const *)paths,n,1,&copy);
            VERIFY("N11 subsequent real OLE release accepts Copy here",effect==DROPEFFECT_COPY&&copy.seen&&copy.correct);
            VERIFY("N11 accepted drop enters and exits its real modal loop",g.modal_seq>=seq+2);
            /* A published destination can still be held by its transfer.
             * Await the actual job terminal event before the one-shot byte
             * oracle. Keep the existing 5s fixture completion interval. */
            uint64_t until=GetTickCount64()+5000;
            while(GetTickCount64()<until){
                if(gesture_modal_job.job!=prior_job&&(gesture_modal_job.state==FPE_JOB_DONE||gesture_modal_job.state==FPE_JOB_FAILED||gesture_modal_job.state==FPE_JOB_CANCELLED))break;
                drag_pump(5);
            }
            VERIFY("N11 accepted copy completes both selected items without errors",gesture_modal_job.job!=prior_job&&gesture_modal_job.state==FPE_JOB_DONE&&!gesture_modal_job.errors&&gesture_modal_job.items_done==2&&gesture_modal_job.items_total==2);
            VERIFY("N11 selected b and c reach the actual destination",drag_exists(b_out)&&drag_exists(c_out));
            VERIFY("N11 copied selected files retain exact bytes",gesture_file_bytes(b_out)&&gesture_file_bytes(c_out));
            VERIFY("N11 copy preserves sources and excludes the inserted preceding row",gesture_file_bytes(paths[0])&&gesture_file_bytes(paths[1])&&!drag_exists(a_out));
            printf("N11_MODAL button=%s change=%s ordinary_menu=%d cancelled_menu=%d copy_menu=%d selected_paths=%u stale_source_dispatch=%d\n",right?"right":"left",change==0?"refresh-insert":change==1?"sort":"filter",ordinary.seen&&ordinary.correct,cancel.seen&&cancel.correct,copy.seen&&copy.correct,n,gesture_calls);
        }
        for(uint32_t i=0;i<n;i++)free(paths[i]);free(paths);
        free(a);free(root);free(dest);free(b_out);free(c_out);free(a_out);
    }
    gesture_observe=0;gesture_modal_trace=0;
}

static void gesture_case(const char *name,void (*run)(void))
{
    LARGE_INTEGER hz,start,end;QueryPerformanceFrequency(&hz);QueryPerformanceCounter(&start);int passed=drag_passed,failed=drag_failed;run();QueryPerformanceCounter(&end);
    printf("CASE %s assertions=%d failures=%d call_ms=%.3f\n",name,drag_passed-passed,drag_failed-failed,(double)(end.QuadPart-start.QuadPart)*1000/hz.QuadPart);
}

static const wchar_t path_prior[]=L"inert previous clipboard \x03b2";
static int path_fixture_open(void)
{
    uint64_t until=GetTickCount64()+1500;do{if(OpenClipboard(g.hwnd))return 1;Sleep(2);}while(GetTickCount64()<until);
    VERIFY("owned clipboard fixture acquisition succeeds",0);return 0;
}
static void path_seed(void)
{
    HGLOBAL data=clipboard_string(path_prior);VERIFY("prior Unicode fixture allocation succeeds",data!=NULL);if(!data)return;
    if(path_clip.mode==1){if(path_clip.unicode)GlobalFree(path_clip.unicode);path_clip.unicode=data;return;}
    if(!path_fixture_open()){GlobalFree(data);return;}VERIFY("prior Unicode fixture publishes",EmptyClipboard()&&SetClipboardData(CF_UNICODETEXT,data));VERIFY("fixture clipboard closes",CloseClipboard());
}
static void path_assert_text(const wchar_t *expected)
{
    if(path_clip.mode!=1&&!path_fixture_open())return;
    HANDLE h=path_clip.mode==1?path_clip.unicode:GetClipboardData(CF_UNICODETEXT);const wchar_t *text=h?GlobalLock(h):NULL;
    VERIFY("clipboard contains exact logical UTF-16 text and terminator",text&&GlobalSize(h)>=(wcslen(expected)+1)*sizeof(wchar_t)&&!wcscmp(text,expected));
    if(text)GlobalUnlock(h);if(path_clip.mode!=1)VERIFY("clipboard read closes",CloseClipboard());
}
static void path_reset_trace(void)
{
    int mode=path_clip.mode;HANDLE previous=path_clip.unicode;memset(&path_clip,0,sizeof path_clip);path_clip.mode=mode;path_clip.unicode=previous;
}
static void path_assert_owned(void)
{
    VERIFY("copy frees only untransferred clipboard handles",path_clip.bad_free==0);
    for(unsigned i=0;i<path_clip.count;i++)VERIFY("copy retains no owned global allocation",!path_clip.owned[i]);
    VERIFY("clipboard closes after publication attempt",!path_clip.opened);
    VERIFY("success is never shown before clipboard acceptance",path_clip.early_success==0);
}
static void path_dispatch(int operands)
{
    wchar_t *one=drag_path(drag_left,L"a space \x03b2.txt"),*two=drag_path(drag_left,L"second.txt");wchar_t *paths[]={one,two};
    fn_ctx c={.dir=drag_left,.pane=&g.panes[0],.where=FN_PANE,.state=FN_S_FOLDER,.n=operands==2?2:operands==1?1:0,.paths=operands?paths:NULL};
    wcscpy(g.panes[0].status,L"Inert before Copy Path");g.panes[0].status_err=0;
    VERIFY("actual file.copy_path registry command dispatches",fn_run_id("file.copy_path",&c));free(one);free(two);
}
static void path_busy_case(void)
{
    puts("Copy Path: busy clipboard retains prior Unicode and reports failure through actual registry/status");
    path_seed();path_reset_trace();path_clip.busy=1;path_dispatch(1);path_clip.busy=0;
    VERIFY("busy clipboard never reports Path copied",g.panes[0].status_err&&!wcsstr(g.panes[0].status,L"copied"));path_assert_text(path_prior);path_assert_owned();
}
static void path_failure_case(void)
{
    puts("Copy Path: output/backup allocation/lock/get/open/Empty/Set failures remain honest and retain prior Unicode");
    for(int operands=0;operands<3;operands++)for(int failure=1;failure<=9;failure++){
        path_seed();path_reset_trace();
        if(failure==1)path_clip.fail_alloc=1;else if(failure==2)path_clip.fail_lock=1;else if(failure==3)path_clip.fail_alloc=2;
        else if(failure==4)path_clip.fail_lock=2;else if(failure==5)path_clip.fail_lock=3;else if(failure==6)path_clip.fail_get=1;
        else if(failure==7)path_clip.fail_open=1;else if(failure==8)path_clip.fail_empty=1;else path_clip.fail_sets=1;
        path_dispatch(operands);VERIFY("failed publication reports error without copied status",g.panes[0].status_err&&!wcsstr(g.panes[0].status,L"copied"));path_assert_text(path_prior);path_assert_owned();
    }
}
static void path_success_case(void)
{
    puts("Copy Path: single/multiple/folder-fallback text is exact; success follows accepted publication");
    for(int operands=0;operands<3;operands++){
        path_seed();path_reset_trace();path_dispatch(operands);
        wchar_t expected[MAX_PATH*3];if(operands==0)_snwprintf(expected,MAX_PATH*3,L"%ls",drag_left);
        else if(operands==1)_snwprintf(expected,MAX_PATH*3,L"%ls\\a space \x03b2.txt",drag_left);
        else _snwprintf(expected,MAX_PATH*3,L"%ls\\a space \x03b2.txt\r\n%ls\\second.txt",drag_left,drag_left);
        path_assert_text(expected);VERIFY("accepted copy reports exact normal success status",!g.panes[0].status_err&&!wcscmp(g.panes[0].status,operands==2?L"2 paths copied":L"Path copied"));VERIFY("exactly one successful clipboard publication occurs",path_clip.sets==1);path_assert_owned();
    }
}
static void path_restore_case(void)
{
    puts("Copy Path: failed Unicode restoration is explicit and never displayed as success");
    path_seed();path_reset_trace();path_clip.fail_sets=3;path_dispatch(1);
    VERIFY("failed restore reports explicit clipboard loss",g.panes[0].status_err&&wcsstr(g.panes[0].status,L"could not be restored")&&!wcsstr(g.panes[0].status,L"copied"));
    VERIFY("failed publication/restoration both attempted",path_clip.sets==2&&!path_clip.unicode);path_assert_owned();
}
static void path_component(void (*run)(void))
{
    memset(&path_clip,0,sizeof path_clip);path_clip.mode=1;run();if(path_clip.unicode)GlobalFree(path_clip.unicode);memset(&path_clip,0,sizeof path_clip);
}
static void test_copy_path_busy_reports_failure_preserves_previous(void){path_component(path_busy_case);}
static void test_copy_path_api_failures_preserve_previous_unicode(void){path_component(path_failure_case);}
static void test_copy_path_success_exact_text_and_status(void){path_component(path_success_case);}
static void test_copy_path_failed_restore_reports_failure(void){path_component(path_restore_case);}
/* Unicode-only Set failure baseline avoids intentionally crashing the old
 * unchecked GlobalLock path; busy plus Set reproduces the visible defect. */
static void path_set_red_case(void){path_seed();path_reset_trace();path_clip.fail_sets=1;path_dispatch(1);VERIFY("failed Set cannot display copied status",g.panes[0].status_err);path_assert_text(path_prior);path_assert_owned();}

typedef struct path_backup {UINT formats[256];HANDLE data[256];unsigned count;} path_backup;
static void path_backup_free(UINT format,HANDLE h)
{
    if(!h)return;if(format==CF_BITMAP||format==CF_PALETTE)DeleteObject(h);else if(format==CF_ENHMETAFILE)DeleteEnhMetaFile(h);
    else if(format==CF_METAFILEPICT){METAFILEPICT *p=GlobalLock(h);if(p){DeleteMetaFile(p->hMF);GlobalUnlock(h);}GlobalFree(h);}else GlobalFree(h);
}
static int path_backup_begin(path_backup *b)
{
    memset(b,0,sizeof *b);if(!path_fixture_open())return 0;UINT f=0;int ok=1;SetLastError(0);
    while((f=EnumClipboardFormats(f))){if(b->count==256||f==CF_OWNERDISPLAY||(f>=CF_PRIVATEFIRST&&f<=CF_PRIVATELAST)||(f>=CF_GDIOBJFIRST&&f<=CF_GDIOBJLAST)){ok=0;break;}
        HANDLE h=GetClipboardData(f),copy=h?OleDuplicateData(h,(CLIPFORMAT)f,GMEM_MOVEABLE):NULL;if(!copy){ok=0;break;}b->formats[b->count]=f;b->data[b->count++]=copy;SetLastError(0);}
    if(!f&&GetLastError())ok=0;CloseClipboard();VERIFY("external clipboard custody is prepared before mutation",ok);
    if(!ok){for(unsigned i=0;i<b->count;i++)path_backup_free(b->formats[i],b->data[i]);b->count=0;}return ok;
}
static void path_backup_end(path_backup *b)
{
    if(path_fixture_open()){VERIFY("external clipboard restoration opens transaction",EmptyClipboard());for(unsigned i=0;i<b->count;i++){HANDLE h=SetClipboardData(b->formats[i],b->data[i]);VERIFY("external duplicable format is restored",h!=NULL);if(h)b->data[i]=NULL;}VERIFY("external clipboard restoration closes",CloseClipboard());}
    for(unsigned i=0;i<b->count;i++)path_backup_free(b->formats[i],b->data[i]);
}
static void path_live_case(void)
{
    puts("Copy Path live: real holder refusal and exact ordinary-desktop path publication");
    memset(&path_clip,0,sizeof path_clip);path_clip.mode=2;path_seed();path_reset_trace();
    clipboard_held=CreateEventW(NULL,TRUE,FALSE,NULL);clipboard_release=CreateEventW(NULL,TRUE,FALSE,NULL);HANDLE holder=clipboard_lock();
    path_dispatch(1);VERIFY("actual busy clipboard reports failure",g.panes[0].status_err&&!wcsstr(g.panes[0].status,L"copied"));clipboard_unlock(holder);path_assert_text(path_prior);path_assert_owned();
    CloseHandle(clipboard_held);CloseHandle(clipboard_release);path_success_case();memset(&path_clip,0,sizeof path_clip);
    puts("LIVE_CASE CopyPath busy=failed success=single,multiple,folder exact=unicode status=after-publication");
}

int WINAPI wWinMain(HINSTANCE inst,HINSTANCE prev,PWSTR command,int show)
{
    int argc=0;
    wchar_t **argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if (argv && argc>1 && !wcscmp(argv[1],L"--settings-race-child")) {
        int result=settings_race_child(argc,argv);
        LocalFree(argv);
        return result;
    }
    int symlink_context=argv&&argc>1&&!wcscmp(argv[1],L"--symlink-context");
    int symlink_only=symlink_context||(argv&&argc>1&&!wcscmp(argv[1],L"--symlink-drop"));
    int admission_only=argv&&argc>1&&!wcscmp(argv[1],L"--admission-controls");
    int shell_only=argv&&argc>1&&!wcscmp(argv[1],L"--shell-controls");
    int gesture_modal=argv&&argc>1&&!wcscmp(argv[1],L"--gesture-modal");
    int gesture_only=argv&&argc>1&&!wcscmp(argv[1],L"--gesture-revision");
    int path_only=argv&&argc>1&&!wcscmp(argv[1],L"--copy-path");
    int path_red=argv&&argc>1&&!wcscmp(argv[1],L"--copy-path-red");
    int path_live=argv&&argc>1&&!wcscmp(argv[1],L"--copy-path-live");
    if (argv) LocalFree(argv);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wchar_t temp[MAX_PATH];GetTempPathW(MAX_PATH,temp);
    if(admission_only||shell_only||symlink_only||gesture_modal||gesture_only||path_only||path_red||path_live){_snwprintf(drag_root,MAX_PATH+80,L"%lsopenpilot-gesture-tests-%lu-%llu",temp,GetCurrentProcessId(),(unsigned long long)GetTickCount64());if(!CreateDirectoryW(drag_root,NULL))return 3;}
    else {_snwprintf(drag_root,MAX_PATH+80,L"%lsopenpilot-drag-tests-%lu",temp,GetCurrentProcessId());CreateDirectoryW(drag_root,NULL);}
    _snwprintf(drag_left,MAX_PATH+100,L"%ls\\left",drag_root);_snwprintf(drag_right,MAX_PATH+100,L"%ls\\right",drag_root);
    CreateDirectoryW(drag_left,NULL);CreateDirectoryW(drag_right,NULL);
    fn_host host={host_now,host_arm,host_status};fn_init(&host);fn_register(app_functions,sizeof app_functions/sizeof *app_functions);
    static const fn_def operations_test_fn={.id="operations.show",.title=L"Operations",.life=FN_COLD,.where=FN_ANYWHERE,
        .opts=operations_opts,.run=run_operations_show};fn_register(&operations_test_fn,1);
    fn_register(&menu_context_function,1);
    functions_register();relay_register();palette_register();
    const char *settings=drag_settings;
    fn_settings(settings,strlen(settings));fn_configure_all();
    fpe_prof_enable(1);g.engine=fpe_engine_create(NULL);g.dual=1;g.ratio=0.5;g.show_tree=0;session_defaults(&g_ses);
    g.cf_effect=RegisterClipboardFormatW(L"Preferred DropEffect");
    for(int i=0;i<2;++i){Pane *p=&g.panes[i];p->idx=i;p->ntabs=1;p->drop_row=-1;p->opts.dirs_first=1;}
    register_class(L"OpenPilot",main_proc,0,inst);register_class(L"OpenPilotPane",pane_proc,CS_DBLCLKS,inst);
    register_class(L"OpenPilotTree",tree_proc,CS_DBLCLKS,inst);register_class(L"OpenPilotDebug",dbg_proc,0,inst);
    HWND window=CreateWindowExW(0,L"OpenPilot",L"Drag tests",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,50,50,1100,740,NULL,NULL,inst,NULL);
    if(!window){drag_remove_tree(drag_root);return 2;}
    for(int i=0;i<2;++i){fpe_dir *dir=read_start(i?drag_right:drag_left);if(dir){p_show_dir(&g.panes[i],dir,NULL,0);fpe_dir_release(dir);}}
    fn_start_hot(0);ShowWindow(window,SW_SHOWNOACTIVATE);UpdateWindow(window);drag_pump(100);
    uint64_t ready_until=GetTickCount64()+5000;
    while(!fn_running(fn_find("svc.drop.tree"))&&GetTickCount64()<ready_until)drag_pump(5);
    VERIFY("asynchronous warmup publishes all real targets before dragging",fn_running(fn_find("svc.drop.tree")));
    if(admission_only){
        path_backup original;if(path_backup_begin(&original)){
            gesture_case("test_clipboard_publication_ui",test_clipboard_publication_ui);
            gesture_case("test_right_drop_ui",test_right_drop_ui);path_backup_end(&original);
        }
    } else if(shell_only){
        gesture_case("test_plain_right_click_ui",test_plain_right_click_ui);
        gesture_case("test_menu_context_lifetime_ui",test_menu_context_lifetime_ui);
    } else if(symlink_only){
        printf("SYMLINK_FIXTURE windows_root=%ls\n",drag_root);
        if(symlink_context){
            gesture_case("test_tab_drag_ui",test_tab_drag_ui);
            gesture_case("test_right_drop_ui",test_right_drop_ui);
            gesture_case("test_shortcut_drop_ui",test_shortcut_drop_ui);
            gesture_case("test_right_drop_wsl_ui",test_right_drop_wsl_ui);
        }
        gesture_case("test_symlink_drop_ui",test_symlink_drop_ui);
        if(!symlink_context)gesture_case("test_shortcut_drop_ui",test_shortcut_drop_ui);
        gesture_case("test_plain_right_click_ui",test_plain_right_click_ui);
    } else if(gesture_modal){
        gesture_case("test_gesture_changed_view_modal_context_and_drop",test_gesture_changed_view_modal_context_and_drop);
    } else if(path_only){
        gesture_case("test_copy_path_busy_reports_failure_preserves_previous",test_copy_path_busy_reports_failure_preserves_previous);
        gesture_case("test_copy_path_api_failures_preserve_previous_unicode",test_copy_path_api_failures_preserve_previous_unicode);
        gesture_case("test_copy_path_success_exact_text_and_status",test_copy_path_success_exact_text_and_status);
        gesture_case("test_copy_path_failed_restore_reports_failure",test_copy_path_failed_restore_reports_failure);
    } else if(path_red){test_copy_path_busy_reports_failure_preserves_previous();path_component(path_set_red_case);}
    else if(path_live){path_backup original;if(path_backup_begin(&original)){gesture_case("path_live_case",path_live_case);gesture_case("test_clipboard_publication_ui",test_clipboard_publication_ui);path_backup_end(&original);}}
    else if(gesture_only){
        gesture_case("test_gesture_refresh_release_preserves_selected_names",test_gesture_refresh_release_preserves_selected_names);
        gesture_case("test_gesture_changed_view_refuses_left_and_right_drag",test_gesture_changed_view_refuses_left_and_right_drag);
        gesture_case("test_gesture_navigation_and_tab_cancel_before_view_change",test_gesture_navigation_and_tab_cancel_before_view_change);
        gesture_case("test_gesture_unchanged_click_and_fresh_press_controls",test_gesture_unchanged_click_and_fresh_press_controls);
        gesture_case("test_gesture_unchanged_left_and_right_drag_controls",test_gesture_unchanged_left_and_right_drag_controls);
        for(int i=0;i<2;i++){p_navigate(&g.panes[i],i?drag_right:drag_left,NULL,0);drag_settle(&g.panes[i]);}
        gesture_case("test_tab_drag_ui",test_tab_drag_ui);
    } else {
        test_copy_path_busy_reports_failure_preserves_previous();
        test_copy_path_api_failures_preserve_previous_unicode();
        test_copy_path_success_exact_text_and_status();
        test_copy_path_failed_restore_reports_failure();
        test_gesture_refresh_release_preserves_selected_names();
        test_gesture_changed_view_refuses_left_and_right_drag();
        test_gesture_navigation_and_tab_cancel_before_view_change();
        test_gesture_unchanged_click_and_fresh_press_controls();
        test_gesture_unchanged_left_and_right_drag_controls();
        test_gesture_changed_view_modal_context_and_drop();
        for(int i=0;i<2;i++){p_navigate(&g.panes[i],i?drag_right:drag_left,NULL,0);drag_settle(&g.panes[i]);}
    test_drop_startup_ui();test_drop_settings_lifecycle_ui();test_partial_painting_ui();test_deferred_pane_geometry();test_tab_drag_ui();test_right_drop_ui();test_shortcut_drop_ui();test_right_drop_wsl_ui();test_symlink_drop_ui();test_plain_right_click_ui();test_menu_context_lifetime_ui();test_palette_revision_ui();test_search_routing_ui();test_wsl_index_ui();
    test_folder_watch_ui();test_settings_commit_ui();test_settings_race_processes();test_clipboard_publication_ui();
    }
    VERIFY("dragging leaves no tab capture behind",g_tab_drag.source==NULL && GetCapture()!=g.panes[0].hwnd);
    char registry[65536];fn_report_csv(registry,sizeof registry);
    VERIFY("dragging has no function lifetime violations",strstr(registry,"metric,fn_violations,,,,0,")!=NULL);
    fpe_prof_row layout_rows[512];
    uint32_t layout_count = fpe_prof_rows(layout_rows, 512);
    int applied = 0, failed_batch = 0;
    for (uint32_t i = 0; i < layout_count; i++) {
        if (!strcmp(layout_rows[i].name, "layout.batch_applied")) applied = layout_rows[i].calls > 0;
        if (!strcmp(layout_rows[i].name, "layout.batch_failed")) failed_batch = layout_rows[i].calls > 0;
    }
    VERIFY("native layout batches apply without falling back to individual moves", applied && !failed_batch);
    wchar_t owned_profile[4096];DWORD profile_n=GetEnvironmentVariableW(L"OPENPILOT_DRAG_PROFILE",owned_profile,4096);
    write_report(profile_n&&profile_n<4096?owned_profile:L"build\\dragging\\drag-profile.csv");
    DestroyWindow(window);
    uint64_t close_until=GetTickCount64()+5000;while(dropwarm_wait_handle()&&GetTickCount64()<close_until)drag_pump(5);
    VERIFY("closing drains OLE work before final teardown",!dropwarm_wait_handle()&&fn_shutdown());
    settings_save_join();folderwatch_join();fpe_engine_destroy(g.engine);if((symlink_only||admission_only||gesture_modal)&&drag_failed)printf("OWNED_FIXTURE_RETAINED windows_root=%ls\n",drag_root);else drag_remove_tree(drag_root);
    printf("\n%d passed, %d failed%s\n",drag_passed,drag_failed,admission_only?"; 2 admission selectors":shell_only?"; 2 shell control selectors":symlink_context?"; 6 symlink context selectors":symlink_only?"; 3 symlink selectors":gesture_modal?"; 1 changed-view modal selector / 6 sequences":gesture_only?"; 6 gesture selectors":path_only?"; 4 Copy Path selectors":path_live?"; 2 live clipboard cases":path_red?"; Copy Path original-source red":"");return drag_failed?1:0;
}
