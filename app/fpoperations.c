/* Operations side-panel app. The explorer owns jobs and their results;
 * this process only presents snapshots and requests Start/Cancel/Clear. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>
#include "panelproto.h"
#include "operations.h"
#include "theme.h"
#define WM_HOST (WM_APP + 1)
#define WM_GONE (WM_APP + 2)
typedef struct msg { panelp_head h; uint8_t data[]; } msg;
typedef struct hot { RECT r; int action; } hot;
static struct {
    HANDLE in, out, section;
    HWND hwnd;
    HDC dc;
    HBITMAP bitmap, original;
    HFONT font, bold, small;
    uint8_t *shared;
    void *pixels;
    int w, h, dpi, next, waiting, pending, shown, focused;
    uint32_t seq;
    ops_model model;
    uint64_t selected, restore;
    int restore_policy, restore_focus, restore_scroll;
    int top, scroll, detail_height, focus, policy;
    hot hot[8]; int nhot;
    RECT list, detail;
} A;
static int S(int x) { return MulDiv(x, A.dpi, 96); }
static int write_all(const void *p, DWORD n)
{
    while (n) { DWORD w; if (!WriteFile(A.out, p, n, &w, NULL) || !w) return 0; p = (const uint8_t *)p + w; n -= w; }
    return 1;
}
static void send(uint32_t type, const void *p, uint32_t size)
{
    panelp_head h = { type, size };
    if (!write_all(&h, sizeof h) || (size && !write_all(p, size))) PostQuitMessage(1);
}
static int read_all(void *p, DWORD n)
{
    while (n) { DWORD got; if (!ReadFile(A.in, p, n, &got, NULL) || !got) return 0; p = (uint8_t *)p + got; n -= got; }
    return 1;
}
static DWORD WINAPI reader(void *arg)
{
    for (;;) {
        panelp_head h;
        if (!read_all(&h, sizeof h) || h.size > PANELP_MAX) break;
        msg *m = malloc(sizeof *m + h.size);
        if (!m) break;
        m->h = h;
        if (!read_all(m->data, h.size)) { free(m); break; }
        if (!PostMessageW(arg, WM_HOST, 0, (LPARAM)m)) free(m);
    }
    PostMessageW(arg, WM_GONE, 0, 0); return 0;
}
static void fonts(void)
{
    if (A.font) DeleteObject(A.font);
    if (A.bold) DeleteObject(A.bold);
    if (A.small) DeleteObject(A.small);
    A.font = CreateFontW(-MulDiv(90, A.dpi, 720), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    A.bold = CreateFontW(-MulDiv(95, A.dpi, 720), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    A.small = CreateFontW(-MulDiv(80, A.dpi, 720), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}
static int map(HANDLE section, int w, int h)
{
    if (A.shared) UnmapViewOfFile(A.shared);
    if (A.section) CloseHandle(A.section);
    if (A.bitmap) { SelectObject(A.dc, A.original); DeleteObject(A.bitmap); A.bitmap = NULL; }
    A.section = section; A.shared = MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 0);
    A.w = w; A.h = h; A.waiting = 0; A.next = 0;
    if (!A.dc) A.dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi = { { sizeof bi.bmiHeader, w, -h, 1, 32, BI_RGB } };
    A.bitmap = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &A.pixels, NULL, 0);
    if (A.bitmap) A.original = SelectObject(A.dc, A.bitmap);
    return A.shared && A.bitmap;
}
static void fill(RECT r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c); FillRect(A.dc, &r, b); DeleteObject(b);
}
static void text(const wchar_t *s, RECT r, COLORREF c, HFONT f, UINT flags)
{
    HGDIOBJ old = SelectObject(A.dc, f); SetTextColor(A.dc, c); SetBkMode(A.dc, TRANSPARENT);
    DrawTextW(A.dc, s, -1, &r, flags | DT_NOPREFIX); SelectObject(A.dc, old);
}
static void button(const wchar_t *s, int x, int y, int w, int action, int chosen)
{
    RECT r = { x, y, x + w, y + S(29) };
    fill(r, chosen ? C_SEL : C_EDIT);
    if (A.focused && A.focus == action) {
        HBRUSH b = CreateSolidBrush(C_ACCENT); FrameRect(A.dc, &r, b); DeleteObject(b);
    }
    text(s, r, C_TEXT, A.font, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (A.nhot < 8) A.hot[A.nhot++] = (hot){ r, action };
}
static int block(const wchar_t *s, int y, COLORREF c)
{
    RECT r = { S(12), y, A.w - S(12), y + S(20) };
    SelectObject(A.dc, A.font); DrawTextW(A.dc, s, -1, &r, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    text(s, r, c, A.font, DT_WORDBREAK);
    return r.bottom + S(8);
}
static void bytes(wchar_t *s, size_t cap, uint64_t n)
{
    if (n >= 1024ull*1024*1024) _snwprintf(s, cap, L"%.1f GB", n / (1024.0*1024*1024));
    else if (n >= 1024*1024) _snwprintf(s, cap, L"%.1f MB", n / (1024.0*1024));
    else if (n >= 1024) _snwprintf(s, cap, L"%.1f KB", n / 1024.0);
    else _snwprintf(s, cap, L"%llu B", (unsigned long long)n);
}
static ops_entry *selected(void) { return ops_find(&A.model, A.selected); }
static void present(void)
{
    if (!A.shared || !A.pixels || !A.shown) return;
    if (A.waiting) { A.pending = 1; return; }
    size_t n = (size_t)A.w * A.h * 4;
    memcpy(A.shared + n * A.next, A.pixels, n);
    panelp_frame f = { (uint32_t)A.next, A.seq, { 0, 0, A.w, A.h } };
    send(PANELP_FRAME, &f, sizeof f); A.next ^= 1; A.waiting = 1; A.pending = 0;
}
static void paint(void)
{
    if (!A.bitmap || !A.shown) return;
    A.nhot = 0;
    fill((RECT){0,0,A.w,A.h}, C_BG);
    wchar_t s[512]; uint32_t active = 0;
    for (uint32_t i = 0; i < A.model.n; i++) if (!ops_finished(A.model.entries[i].h.state)) active++;
    _snwprintf(s, 512, L"%u in progress / ready   %u results", active, A.model.n - active);
    text(s, (RECT){S(12),S(8),A.w-S(12),S(30)}, C_DIM, A.small, DT_SINGLELINE);
    button(L"Clear completed", S(12), S(34), S(132), 4, 0);
    int lh = A.model.n ? S(54) * (A.model.n < 3 ? (int)A.model.n : 3) : S(54);
    if (lh > A.h / 3) lh = A.h / 3;
    A.list = (RECT){0,S(74),A.w,S(74)+lh};
    int visible = lh / S(54); if (visible < 1) visible = 1;
    if (A.top > (int)A.model.n - visible) A.top = (int)A.model.n - visible;
    if (A.top < 0) A.top = 0;
    int saved = SaveDC(A.dc); IntersectClipRect(A.dc, A.list.left, A.list.top, A.list.right, A.list.bottom);
    for (int i = A.top; i < (int)A.model.n; i++) {
        int y = A.list.top + (i - A.top)*S(54);
        if (y >= A.list.bottom) break;
        ops_entry *e = &A.model.entries[i];
        if (e->h.token == A.selected) fill((RECT){0,y,A.w,y+S(53)}, C_SEL_IDLE);
        const wchar_t *base = wcsrchr(e->source, L'\\'); base = base ? base+1 : e->source;
        _snwprintf(s, 512, L"%ls  %ls%ls", ops_verb(e->h.op), base, e->h.count > 1 ? L" (+ more)" : L"");
        text(s, (RECT){S(12),y+S(4),A.w-S(12),y+S(27)}, C_TEXT,A.bold,DT_SINGLELINE|DT_END_ELLIPSIS);
        _snwprintf(s, 512, L"#%llu  %ls", (unsigned long long)e->h.token, ops_state(e));
        text(s, (RECT){S(12),y+S(29),A.w-S(12),y+S(49)}, e->h.errors ? C_ERROR : C_DIM,A.small,DT_SINGLELINE);
    }
    RestoreDC(A.dc, saved);
    int y = A.list.bottom + S(12);
    ops_entry *e = selected();
    if (!e) {
        block(L"Copy, move, delete and recycle jobs appear here. Results stay available when this panel closes.", y, C_DIM);
        present(); return;
    }
    _snwprintf(s, 512, L"%ls / %ls", ops_verb(e->h.op), ops_state(e));
    text(s,(RECT){S(12),y,A.w-S(12),y+S(24)},e->h.errors?C_ERROR:C_TEXT,A.bold,DT_SINGLELINE|DT_END_ELLIPSIS); y += S(32);
    if (e->h.state == OPS_PENDING && e->h.op <= FPE_OP_MOVE) {
        int gap=S(6), width=(A.w-S(24)-2*gap)/3;
        button(L"Keep both",S(12),y,width,10, A.policy==FPE_CONFLICT_RENAME);
        button(L"Skip existing",S(12)+width+gap,y,width,11,A.policy==FPE_CONFLICT_SKIP);
        button(L"Replace files",S(12)+2*(width+gap),y,width,12,A.policy==FPE_CONFLICT_OVERWRITE);
        y += S(37);
        text(A.policy==FPE_CONFLICT_RENAME ? L"Name clashes get a numbered copy." : A.policy==FPE_CONFLICT_SKIP ?
             L"Existing files stay; folders merge." : L"Existing files are replaced; folders merge.",
             (RECT){S(12),y,A.w-S(12),y+S(32)}, A.policy==FPE_CONFLICT_OVERWRITE?C_ERROR:C_DIM,A.small,DT_WORDBREAK);
        y += S(38);
    }
    if (e->h.state == FPE_JOB_RUNNING && (e->h.bytes_total || e->h.items_total)) {
        double done = e->h.bytes_total ? (double)e->h.bytes_done / e->h.bytes_total :
                                        (double)e->h.items_done / e->h.items_total;
        if (done > 1) done = 1;
        RECT bar = { S(12), y, A.w - S(12), y + S(4) };
        fill(bar, C_EDIT);
        bar.right = bar.left + (int)((bar.right - bar.left) * done);
        fill(bar, C_ACCENT);
        y += S(12);
    }
    if (!ops_finished(e->h.state)) {
        button(e->h.state==OPS_PENDING ? L"Start" : e->h.cancelling ? L"Cancel requested" : L"Cancel", S(12),y,S(142),2,0);
        if (e->h.state == OPS_PENDING) button(L"Cancel", S(164), y, S(85), 5, 0);
        y += S(37);
    }
    button(L"Copy result",S(12),y,S(114),3,0); y+=S(39);
    A.detail=(RECT){0,y,A.w,A.h-S(35)};
    saved=SaveDC(A.dc); IntersectClipRect(A.dc,0,y,A.w,A.detail.bottom);
    int start=y-A.scroll;
    y=block(L"Source",start,C_DIM); y=block(e->source,y,C_TEXT);
    if (e->h.count>1) { _snwprintf(s,512,L"%u selected items",e->h.count); y=block(s,y,C_DIM); }
    if (e->dest[0]) { y=block(L"Destination",y,C_DIM); y=block(e->dest,y,C_TEXT); }
    if (e->h.state!=OPS_PENDING) {
        wchar_t done[48],total[48]; bytes(done,48,e->h.bytes_done); bytes(total,48,e->h.bytes_total);
        _snwprintf(s,512,L"%u / %u items processed\n%ls / %ls\n%u errors   %u skipped",e->h.items_done,e->h.items_total,done,total,e->h.errors,e->h.skipped);
        y=block(s,y,C_TEXT);
        if (ops_finished(e->h.state) && e->h.started && e->h.ended>=e->h.started) {
            _snwprintf(s,512,L"Elapsed %.1f s",(e->h.ended-e->h.started)/1000.0); y=block(s,y,C_DIM);
        }
        if (e->current[0] && !ops_finished(e->h.state)) y=block(e->current,y,C_DIM);
        if (e->details[0]) y=block(e->details,y,C_TEXT);
        if (e->h.omitted) { _snwprintf(s,512,L"%u additional diagnostics omitted (history is bounded).",e->h.omitted); y=block(s,y,C_DIM); }
    }
    A.detail_height=y-start;
    RestoreDC(A.dc,saved);
    text(L"Tab: controls   Up/Down: jobs   PgUp/PgDn: details",(RECT){S(12),A.h-S(28),A.w-S(12),A.h},C_DIM,A.small,DT_SINGLELINE|DT_END_ELLIPSIS);
    present();
}
static void request(int action)
{
    ops_entry *e=selected();
    ops_action a={ e ? e->h.token : 0, (uint32_t)action, (uint32_t)A.policy };
    send(PANELP_OP_ACTION,&a,sizeof a);
}
static void copy_result(void)
{
    ops_entry *e=selected(); if (!e) return;
    size_t cap=wcslen(e->source)+wcslen(e->dest)+wcslen(e->details)+512;
    HGLOBAL mem=GlobalAlloc(GMEM_MOVEABLE,cap*sizeof(wchar_t)); if (!mem) return;
    wchar_t *s=GlobalLock(mem);
    if (!s) { GlobalFree(mem); return; }
    _snwprintf(s,cap,L"Operation #%llu\r\n%ls: %ls\r\nSource: %ls\r\nDestination: %ls\r\n%u/%u items processed; %u errors; %u skipped\r\n%ls%u additional diagnostics omitted\r\n",
        (unsigned long long)e->h.token,ops_verb(e->h.op),ops_state(e),e->source,e->dest,e->h.items_done,e->h.items_total,e->h.errors,e->h.skipped,e->details,e->h.omitted);
    GlobalUnlock(mem);
    int opened = 0;
    for (int i = 0; i < 10 && !opened; i++) {
        opened = OpenClipboard(A.hwnd);
        if (!opened) Sleep(10);
    }
    if (opened) { EmptyClipboard(); if (SetClipboardData(CF_UNICODETEXT,mem)) mem=NULL; CloseClipboard(); }
    if (mem) GlobalFree(mem);
}
static void act(int action)
{
    ops_entry *e=selected();
    if (action==4) request(OPS_CLEAR);
    else if (action==3) copy_result();
    else if (action==5 && e && e->h.state==OPS_PENDING) request(OPS_CANCEL);
    else if (action>=10 && action<=12) {
        if (e && e->h.state==OPS_PENDING) A.policy=action-10, e->h.policy=(uint32_t)A.policy;
    } else if (action==2 && e && !ops_finished(e->h.state) && !e->h.cancelling)
        request(e->h.state==OPS_PENDING ? OPS_START : OPS_CANCEL);
}
static void choose(int at)
{
    if (at<0 || at>=(int)A.model.n) return;
    A.selected=A.model.entries[at].h.token; A.policy=(int)A.model.entries[at].h.policy; A.scroll=0;
    if (at<A.top) A.top=at;
    if (at>=A.top+3) A.top=at-2;
}
static int contains(RECT r,int x,int y) { return x>=r.left && x<r.right && y>=r.top && y<r.bottom; }
static void input(const panelp_input *in)
{
    if (in->msg==WM_LBUTTONDOWN) {
        if (contains(A.list,in->x,in->y)) { choose(A.top+(in->y-A.list.top)/S(54)); A.focus=0; }
        else for (int i=0;i<A.nhot;i++) if (contains(A.hot[i].r,in->x,in->y)) { A.focus=A.hot[i].action; act(A.focus); break; }
    } else if (in->msg==WM_MOUSEWHEEL) {
        int d=-(short)HIWORD(in->wparam)/WHEEL_DELTA;
        if (in->y<A.list.bottom) A.top+=d;
        else A.scroll+=d*S(60);
    } else if (in->msg==WM_KEYDOWN) {
        UINT k=in->wparam;
        if (k==VK_TAB) {
            int ids[8]={0},n=1,at=0;
            for (int i=0;i<A.nhot;i++) ids[n++]=A.hot[i].action;
            for(int i=0;i<n;i++) if(ids[i]==A.focus) at=i;
            A.focus=ids[(at+(in->mods&PANELP_M_SHIFT?n-1:1))%n];
        } else if (k==VK_UP || k==VK_DOWN) {
            int at=0; for(uint32_t i=0;i<A.model.n;i++) if(A.model.entries[i].h.token==A.selected) at=(int)i;
            choose(at+(k==VK_DOWN?1:-1));
        } else if ((k==VK_LEFT || k==VK_RIGHT) && selected() && selected()->h.state==OPS_PENDING) {
            A.policy=(A.policy+(k==VK_RIGHT?1:2))%3; selected()->h.policy=A.policy;
        } else if (k==VK_RETURN || k==VK_SPACE) act(A.focus ? A.focus : 2);
        else if (k==VK_PRIOR || k==VK_NEXT) A.scroll+=(k==VK_NEXT?1:-1)*(A.detail.bottom-A.detail.top-S(20));
        else if (k==VK_ESCAPE) send(PANELP_CLOSE,NULL,0);
        else if (k=='C' && in->mods&PANELP_M_CTRL) copy_result();
    } else return;
    int max=A.detail_height-(A.detail.bottom-A.detail.top); if(max<0)max=0;
    if(A.scroll>max)A.scroll=max; if(A.scroll<0)A.scroll=0;
    paint();
}
static void on_host(msg *m)
{
    switch(m->h.type) {
    case PANELP_PING: if(m->h.size==4)send(PANELP_PONG,m->data,4);break;
    case PANELP_FRAME_ACK: A.waiting=0;if(A.pending)present();break;
    case PANELP_INPUT: if(m->h.size==sizeof(panelp_input))input((panelp_input *)m->data);break;
    case PANELP_FOCUS: if(m->h.size==4)A.focused=*(uint32_t *)m->data!=0;paint();break;
    case PANELP_SHOW: if(m->h.size==4)A.shown=*(uint32_t *)m->data!=0;if(A.shown)paint();break;
    case PANELP_OP_RESET:
        A.restore = A.selected;
        A.restore_policy = A.policy; A.restore_focus = A.focus; A.restore_scroll = A.scroll;
        ops_free(&A.model); A.selected = 0; A.top = A.scroll = 0;
        break; /* paint the replay as one snapshot on OP_SELECT */
    case PANELP_OP_UPDATE: {
        ops_entry e;
        if(!ops_decode(m->data,m->h.size,&e))break;
        ops_entry *old=ops_find(&A.model,e.h.token);
        int fresh=!old;
        if(!old && A.model.n<OPS_MAX)old=A.model.entries+A.model.n++;
        if(!old){ops_free_entry(&e);break;}
        if(!fresh)ops_free_entry(old);
        *old=e;
        if (e.h.token == A.restore) {
            choose((int)(old - A.model.entries));
            if (e.h.state == OPS_PENDING) A.policy = A.restore_policy, old->h.policy = A.policy;
            A.focus = A.restore_focus; A.scroll = A.restore_scroll;
        } else if ((!A.restore && fresh) || !selected()) {
            choose((int)(old - A.model.entries)); A.focus = e.h.state == OPS_PENDING ? 2 : 0;
        }
        paint();break;
    }
    case PANELP_OP_SELECT:
        if (m->h.size == sizeof(uint64_t)) {
            uint64_t token; memcpy(&token, m->data, sizeof token);
            ops_entry *e = ops_find(&A.model, token);
            if (e && A.selected != token) {
                choose((int)(e - A.model.entries)); A.focus = e->h.state == OPS_PENDING ? 2 : 0;
            }
            A.restore = 0;
            paint();
        }
        break;
    case PANELP_RESIZE: if(m->h.size==sizeof(panelp_size)) {
        panelp_size s;memcpy(&s,m->data,sizeof s);
        if(s.dpi && (int)s.dpi!=A.dpi){A.dpi=s.dpi;fonts();}
        A.seq=s.seq;if(map((HANDLE)(uintptr_t)s.section,s.w,s.h))paint();
    }break;
    case PANELP_QUIT: PostQuitMessage(0);break;
    }
}
static LRESULT CALLBACK hidden(HWND h,UINT m,WPARAM w,LPARAM l) { return DefWindowProcW(h,m,w,l); }
int WINAPI wWinMain(HINSTANCE inst,HINSTANCE prev,PWSTR cmd,int show)
{
    A.in=GetStdHandle(STD_INPUT_HANDLE);A.out=GetStdHandle(STD_OUTPUT_HANDLE);A.shown=1;
    panelp_head h;panelp_hello hi;
    if(!read_all(&h,sizeof h)||h.type!=PANELP_HELLO||h.size!=sizeof hi||!read_all(&hi,sizeof hi)||hi.magic!=PANELP_MAGIC||hi.version!=PANELP_VERSION)return 1;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    A.dpi=hi.dpi?hi.dpi:96;A.seq=hi.seq;fonts();
    WNDCLASSW wc={0};wc.lpfnWndProc=hidden;wc.hInstance=inst;wc.lpszClassName=L"FpOperationsHidden";RegisterClassW(&wc);
    A.hwnd=CreateWindowExW(WS_EX_TOOLWINDOW,wc.lpszClassName,L"",WS_POPUP,0,0,0,0,NULL,NULL,inst,NULL);
    if(!A.hwnd||!map((HANDLE)(uintptr_t)hi.section,hi.w,hi.h))return 1;
    HANDLE t=CreateThread(NULL,64*1024,reader,A.hwnd,STACK_SIZE_PARAM_IS_A_RESERVATION,NULL);if(!t)return 1;CloseHandle(t);
    panelp_ready ready={GetCurrentProcessId(),0,L"Operations"};send(PANELP_READY,&ready,sizeof ready);paint();
    MSG m;while(GetMessageW(&m,NULL,0,0)>0){if(m.message==WM_HOST){on_host((msg *)m.lParam);free((void *)m.lParam);}else if(m.message==WM_GONE)break;else DispatchMessageW(&m);}
    ops_free(&A.model);return 0;
}
