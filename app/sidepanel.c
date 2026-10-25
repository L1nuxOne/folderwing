/*
 * sidepanel.c - the side panel and the panel apps it shows.
 *
 * The container (a child of the main window) has a grip on its left edge to
 * resize it, a header (a tab per app, state, Restart, close) and the slot:
 * the area the shown app's pictures fill. Each app runs on its own; the host
 * never waits for one. Messages from an app arrive through its reader
 * thread, and messages to it leave through its writer thread, so a full pipe
 * can't block the UI either. A ping every 250 ms tells a busy app from a
 * hung one. An app that isn't shown keeps running only if it asked to
 * (SIDEPANEL_KEEP); it is told it is hidden and its frames are taken without
 * being painted. Closing the panel asks the other apps to quit and ends
 * them shortly after if they don't.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <imm.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "app.h"
#include "panelproto.h"
#include "sidepanel.h"
#include "theme.h"
#include "startuptrace.h"

#define WM_PANEL_MSG      (WM_APP + 70) /* wParam: generation, lParam: msg * */
#define WM_PANEL_GONE     (WM_APP + 71) /* wParam: generation */
#define WM_PANEL_FOCUS    (WM_APP + 73) /* defer initial focus until panel layout has returned */
#define WM_PANEL_LAUNCHED (WM_APP + 72) /* wParam: generation */
#define TIMER_SLIDE 1
#define TIMER_WATCH 2
#define SLIDE_MS    100
#define HUNG_MS     1000
#define QUIT_MS     500                 /* a quitting app gets this long before it is ended */
#define QUEUE_MAX   (16u * 1024 * 1024) /* unsent bytes for an app that doesn't read: then it gets nothing more */
#define APPS        4

enum { A_OFF, A_STARTING, A_RUNNING, A_HUNG, A_GONE };

typedef struct msg {
    panelp_head head;
    uint8_t     data[];
} msg;

typedef struct reader_arg {
    HANDLE   from;
    HWND     to;
    uint32_t gen;
} reader_arg;

/* What goes to an app, written by its own thread. The UI thread and the
 * writer each hold a reference; the last one frees it. */
typedef struct writer {
    SRWLOCK            lock;
    CONDITION_VARIABLE cv;
    HANDLE             pipe;
    uint8_t           *buf;
    size_t             len, cap;
    int                closing, broken, refs;
} writer;

/* A launch owns its inputs and unpublished child independently of the UI record.
 * The UI and worker each hold a reference; cancellation never joins the worker. */
typedef struct launch_state {
    wchar_t path[MAX_PATH];
    HWND to;
    uint32_t gen;
    HANDLE child_in, child_out, proc, job;
    DWORD pid;
    volatile LONG refs, cancelled, done;
    int ok;
} launch_state;

typedef struct papp {
    wchar_t  path[MAX_PATH]; /* empty: a free record */
    unsigned flags;
    int      state;
    uint32_t gen;
    HANDLE   proc, job;
    launch_state *launch;
    writer  *out;
    DWORD    pid;

    wchar_t  title[64];
    DWORD    exit_code; /* how it ended (A_GONE) */
    int64_t  start_t0;
    int      framed; /* a first frame arrived */
    int      visible, focused; /* what it was last told */
    int      exit_pending;     /* pipe EOF can arrive before the process exit status */
    int      pipe_gone;        /* EOF may precede publication of a failed launch */
    int      helloed;          /* HELLO went: what is sent before waits in held, as it must come first */
    uint8_t *held;
    size_t   held_len;

    /* pictures */
    HANDLE   section, old_section;
    HBITMAP  bmp[2], old_bmp[2];
    int      bw, bh, old_w, old_h;
    uint32_t seq; /* the current section's number */
    int      shown, have_frame, ack_due;
    uint32_t cursor;
    int      capture; /* PANELP_CAPTURE: every key goes to the app */
    panelp_rect caret;
    int64_t  input_t0, resize_t0;
    int      resize_busy, resize_pending; /* a size sent and not drawn yet; another one waiting */
    uint64_t resize_at;

    /* watchdog */
    uint32_t ping, pong;
    uint64_t ping_at;

    RECT r_tab;
} papp;

static struct {
    sidepanel_config cfg;
    HWND             parent, hwnd, slot;
    int              open, x, y, w, h; /* where the layout put it */
    int              width;            /* wanted width, px; 0 = default */
    int              sliding;
    uint64_t         slide_t0;
    uint64_t         last_move;
    uint32_t         gens;

    papp     apps[APPS];
    papp    *cur; /* shown (or last shown while closed) */

    RECT r_restart, r_close;
    int  dragging;
} P;

static int px(int v) { return app_px(v); }
static int head_h(void) { return px(30); }
static int grip_w(void) { return px(5); }
static int live(const papp *a) { return a->state == A_STARTING || a->state == A_RUNNING || a->state == A_HUNG; }
static int is_shown(const papp *a) { return P.open && P.cur == a; }

/* ---- talking to an app ------------------------------------------------------------------- */

static void writer_unref(writer *wr)
{
    AcquireSRWLockExclusive(&wr->lock);
    int last = --wr->refs == 0;
    ReleaseSRWLockExclusive(&wr->lock);
    if (last) {
        free(wr->buf);
        free(wr);
    }
}

static DWORD WINAPI writer_thread(void *arg)
{
    writer *wr = arg;
    uint8_t *mine = NULL;
    size_t mine_cap = 0;
    AcquireSRWLockExclusive(&wr->lock);
    for (;;) {
        while (!wr->len && !wr->closing) SleepConditionVariableSRW(&wr->cv, &wr->lock, INFINITE, 0);
        if (!wr->len) break; /* closing, and everything is out */
        /* swap buffers: the UI thread keeps queueing while this one writes */
        uint8_t *b = wr->buf;
        size_t n = wr->len, c = wr->cap;
        wr->buf = mine;
        wr->cap = mine_cap;
        wr->len = 0;
        mine = b;
        mine_cap = c;
        ReleaseSRWLockExclusive(&wr->lock);
        int ok = 1;
        for (size_t at = 0; at < n && ok;) {
            DWORD w = 0;
            ok = WriteFile(wr->pipe, mine + at, (DWORD)(n - at < (1u << 20) ? n - at : 1u << 20), &w, NULL) && w;
            at += w;
        }
        AcquireSRWLockExclusive(&wr->lock);
        if (!ok) wr->broken = 1;
    }
    ReleaseSRWLockExclusive(&wr->lock);
    CloseHandle(wr->pipe); /* EOF: the app ends even if QUIT was lost */
    free(mine);
    writer_unref(wr);
    return 0;
}

static writer *writer_start(HANDLE pipe)
{
    writer *wr = calloc(1, sizeof *wr);
    if (!wr) return NULL;
    InitializeSRWLock(&wr->lock);
    InitializeConditionVariable(&wr->cv);
    wr->pipe = pipe;
    wr->refs = 2;
    HANDLE t = CreateThread(NULL, 64 * 1024, writer_thread, wr, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t) {
        free(wr);
        return NULL;
    }
    CloseHandle(t);
    return wr;
}

static void writer_close(writer *wr)
{
    if (!wr) return;
    AcquireSRWLockExclusive(&wr->lock);
    wr->closing = 1;
    WakeConditionVariable(&wr->cv);
    ReleaseSRWLockExclusive(&wr->lock);
    writer_unref(wr);
}

static int send_msg(papp *a, uint32_t type, const void *data, uint32_t size)
{
    writer *wr = a ? a->out : NULL;
    if (!wr || size > PANELP_MAX) return 0;
    panelp_head h = { type, size };
    size_t need = sizeof h + size;
    if (!a->helloed && type != PANELP_HELLO) {
        if (a->held_len + need > QUEUE_MAX) return 0;
        uint8_t *b = realloc(a->held, a->held_len + need);
        if (!b) return 0;
        memcpy(b + a->held_len, &h, sizeof h);
        if (size) memcpy(b + a->held_len + sizeof h, data, size);
        a->held = b;
        a->held_len += need;
        return 1;
    }
    int ok = 0;
    AcquireSRWLockExclusive(&wr->lock);
    if (!wr->broken && !wr->closing && wr->len + need <= QUEUE_MAX) {
        if (wr->len + need > wr->cap) {
            size_t cap = wr->cap ? wr->cap : 4096;
            while (cap < wr->len + need) cap *= 2;
            uint8_t *b = realloc(wr->buf, cap);
            if (b) wr->buf = b, wr->cap = cap;
        }
        if (wr->len + need <= wr->cap) {
            memcpy(wr->buf + wr->len, &h, sizeof h);
            if (size) memcpy(wr->buf + wr->len + sizeof h, data, size);
            wr->len += need;
            ok = 1;
            WakeConditionVariable(&wr->cv);
        }
    }
    ReleaseSRWLockExclusive(&wr->lock);
    return ok;
}

static int read_all(HANDLE h, void *buf, DWORD n)
{
    uint8_t *p = buf;
    while (n) {
        DWORD got = 0;
        if (!ReadFile(h, p, n, &got, NULL) || !got) return -1;
        p += got;
        n -= got;
    }
    return 0;
}

static DWORD WINAPI reader(void *arg)
{
    reader_arg a = *(reader_arg *)arg;
    free(arg);
    for (;;) {
        panelp_head h;
        if (read_all(a.from, &h, sizeof h) || h.size > PANELP_MAX) break;
        msg *m = malloc(sizeof *m + h.size);
        if (!m || read_all(a.from, m->data, h.size)) {
            free(m);
            break;
        }
        m->head = h;
        if (!PostMessageW(a.to, WM_PANEL_MSG, a.gen, (LPARAM)m)) free(m);
    }
    CloseHandle(a.from);
    PostMessageW(a.to, WM_PANEL_GONE, a.gen, 0);
    return 0;
}

typedef struct reap_arg {
    HANDLE proc, job;
} reap_arg;

/* Gives a quitting app QUIT_MS, then ends it; off the UI thread. */
static DWORD WINAPI reaper(void *arg)
{
    reap_arg a = *(reap_arg *)arg;
    free(arg);
    if (a.proc && WaitForSingleObject(a.proc, QUIT_MS) != WAIT_OBJECT_0 && a.job) TerminateJobObject(a.job, 1);
    if (a.proc) CloseHandle(a.proc);
    if (a.job) CloseHandle(a.job);
    return 0;
}

static void reap_async(HANDLE proc, HANDLE job)
{
    if (!proc && !job) return;
    reap_arg *ra = malloc(sizeof *ra);
    HANDLE t = NULL;
    if (ra) {
        *ra = (reap_arg){ proc, job };
        t = CreateThread(NULL, 64 * 1024, reaper, ra, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    }
    if (t) CloseHandle(t);
    else {
        free(ra);
        /* Closing the last job handle also contains a child on allocation failure. */
        if (job) CloseHandle(job);
        if (proc) CloseHandle(proc);
    }
}

static void launch_unref(launch_state *a)
{
    if (InterlockedDecrement(&a->refs)) return;
    if (a->child_in) CloseHandle(a->child_in);
    if (a->child_out) CloseHandle(a->child_out);
    reap_async(a->proc, a->job);
    free(a);
}

static DWORD WINAPI launch(void *arg)
{
    panel_startup_trace("host.launch.enter");
    launch_state *a = arg;
    wchar_t cmd[MAX_PATH + 8], dir[MAX_PATH];
    _snwprintf(cmd, MAX_PATH + 8, L"\"%ls\"", a->path);
    cmd[MAX_PATH + 7] = 0;
    wcscpy(dir, a->path);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    PROCESS_INFORMATION pi = { 0 };
    HANDLE inherit[2] = { a->child_in, a->child_out };
    SIZE_T asize = 0;
    if (InterlockedCompareExchange(&a->cancelled, 0, 0)) goto out;
    /* Assignment at creation contains even a suspended child if the host exits
     * while CreateProcess is stalled. There is no unassigned-child interval. */
    a->job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim = { 0 };
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (!a->job || !SetInformationJobObject(a->job, JobObjectExtendedLimitInformation, &lim, sizeof lim)) goto out;
    InitializeProcThreadAttributeList(NULL, 2, 0, &asize);
    attrs = malloc(asize);
    if (!attrs || !InitializeProcThreadAttributeList(attrs, 2, 0, &asize)) {
        free(attrs);
        attrs = NULL;
        goto out;
    }
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, NULL, NULL) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &a->job, sizeof a->job, NULL, NULL))
        goto out;
    STARTUPINFOEXW si = { { sizeof si } };
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = a->child_in;
    si.StartupInfo.hStdOutput = a->child_out;
    si.lpAttributeList = attrs;
    panel_startup_trace("host.create_process.begin");
    int64_t created_at = fpe_prof_begin();
    int created = CreateProcessW(a->path, cmd, NULL, NULL, TRUE, CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, NULL, dir,
                                &si.StartupInfo, &pi);
    FPE_PROF_END("panel.launch.create_process", created_at);
    panel_startup_trace("host.create_process.end");
    if (!created) goto out;
    a->proc = pi.hProcess;
    a->pid = pi.dwProcessId;
    if (!InterlockedCompareExchange(&a->cancelled, 0, 0)) {
        AllowSetForegroundWindow(pi.dwProcessId); /* its popup menus need the foreground */
        a->ok = ResumeThread(pi.hThread) != (DWORD)-1;
        panel_startup_trace("host.resume.end");
    }
    CloseHandle(pi.hThread);
out:
    if (attrs) {
        DeleteProcThreadAttributeList(attrs);
        free(attrs);
    }
    CloseHandle(a->child_in);
    CloseHandle(a->child_out);
    a->child_in = a->child_out = NULL;
    /* Publish all result fields before the UI can adopt them. After publication
     * the worker only touches atomic flags and its own reference. */
    InterlockedExchange(&a->done, 1);
    if (!InterlockedCompareExchange(&a->cancelled, 0, 0)) PostMessageW(a->to, WM_PANEL_LAUNCHED, a->gen, 0);
    panel_startup_trace("host.launch.posted");
    launch_unref(a);
    return 0;
}

/* Poll once; the UI never waits for process creation or a worker's return. */
static int launch_take(papp *a)
{
    launch_state *l = a->launch;
    if (!l) return 1;
    if (!InterlockedCompareExchange(&l->done, 0, 0)) return 0;
    a->launch = NULL;
    if (l->ok) {
        a->proc = l->proc;
        a->job = l->job;
        a->pid = l->pid;
        l->proc = l->job = NULL;
    }
    launch_unref(l);
    return 1;
}

static void launch_cancel(papp *a)
{
    launch_state *l = a->launch;
    a->launch = NULL;
    if (!l) return;
    InterlockedExchange(&l->cancelled, 1);
    launch_unref(l);
}

/* ---- pictures ------------------------------------------------------------------------- */

static void pixels_free_old(papp *a)
{
    for (int i = 0; i < 2; i++) {
        if (a->old_bmp[i]) DeleteObject(a->old_bmp[i]);
        a->old_bmp[i] = NULL;
    }
    if (a->old_section) CloseHandle(a->old_section);
    a->old_section = NULL;
}

static void pixels_free(papp *a)
{
    pixels_free_old(a);
    for (int i = 0; i < 2; i++) {
        if (a->bmp[i]) DeleteObject(a->bmp[i]);
        a->bmp[i] = NULL;
    }
    if (a->section) CloseHandle(a->section);
    a->section = NULL;
    a->bw = a->bh = 0;
    a->have_frame = a->ack_due = 0;
}

/* Two w x h buffers in one section. The last shown frame stays on screen
 * (cropped) until the app draws one at the new size. */
static int pixels_make(papp *a, int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    /* resized again before the app drew at the last size: the frame kept from before stays */
    if (a->have_frame) {
        pixels_free_old(a);
        for (int i = 0; i < 2; i++) a->old_bmp[i] = a->bmp[i];
        a->old_section = a->section;
        a->old_w = a->bw;
        a->old_h = a->bh;
    } else {
        for (int i = 0; i < 2; i++)
            if (a->bmp[i]) DeleteObject(a->bmp[i]);
        if (a->section) CloseHandle(a->section);
    }
    a->bmp[0] = a->bmp[1] = NULL;
    a->section = NULL;
    size_t one = (size_t)w * (size_t)h * 4;
    a->section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_COMMIT, 0, (DWORD)(one * 2), NULL);
    if (!a->section) return 0;
    BITMAPINFO bi = { { sizeof bi.bmiHeader, w, -h, 1, 32, BI_RGB } };
    for (int i = 0; i < 2; i++) {
        void *bits = NULL;
        a->bmp[i] = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, a->section, (DWORD)(one * (size_t)i));
        if (!a->bmp[i]) return 0;
    }
    a->bw = w;
    a->bh = h;
    a->seq++;
    a->have_frame = 0;
    a->ack_due = 0;
    return 1;
}

/* The section in the app's handle table, or 0. */
static uint64_t pixels_handle_for_app(papp *a)
{
    HANDLE dup = NULL;
    if (!a->section || !a->proc || !DuplicateHandle(GetCurrentProcess(), a->section, a->proc, &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return 0;
    return (uint64_t)(uintptr_t)dup;
}

/* ---- the apps' lives ------------------------------------------------------------------- */

static void repaint_header(void)
{
    if (!P.hwnd) return;
    RECT r = { 0, 0, P.w, head_h() };
    InvalidateRect(P.hwnd, &r, FALSE);
}

static papp *by_gen(uint32_t gen)
{
    for (int i = 0; i < APPS; i++)
        if (P.apps[i].path[0] && P.apps[i].gen == gen && P.apps[i].state != A_OFF) return &P.apps[i];
    return NULL;
}

static papp *by_path(const wchar_t *path)
{
    for (int i = 0; i < APPS; i++)
        if (P.apps[i].path[0] && !_wcsicmp(P.apps[i].path, path)) return &P.apps[i];
    return NULL;
}

static void watch_timer(void)
{
    int any = 0;
    for (int i = 0; i < APPS; i++) any |= live(&P.apps[i]) || P.apps[i].exit_pending;
    if (!P.hwnd) return;
    if (any) SetTimer(P.hwnd, TIMER_WATCH, 250, NULL);
    else KillTimer(P.hwnd, TIMER_WATCH);
}

static void app_stop(papp *a)
{
    launch_cancel(a);
    a->gen = ++P.gens; /* whatever the old app still sends is ignored */
    if (a->out) {
        send_msg(a, PANELP_QUIT, NULL, 0);
        writer_close(a->out);
        a->out = NULL;
    }
    reap_async(a->proc, a->job);
    a->proc = a->job = NULL;
    a->pid = 0;
    a->state = A_OFF;
    a->exit_code = 0;
    a->framed = 0;
    a->capture = 0;
    a->visible = a->focused = 0;
    a->helloed = a->pipe_gone = a->exit_pending = 0;
    free(a->held);
    a->held = NULL;
    a->held_len = 0;
    a->resize_busy = a->resize_pending = 0;
    pixels_free(a);
    watch_timer();
    if (P.slot && P.cur == a) InvalidateRect(P.slot, NULL, FALSE);
}

/* The record goes: the app ends and its tab with it. */
static void app_forget(papp *a)
{
    app_stop(a);
    a->path[0] = 0;
    a->title[0] = 0;
    if (P.cur == a) P.cur = NULL;
    repaint_header();
}

static void slot_size(int *w, int *h)
{
    RECT r = { 0 };
    if (P.slot) GetClientRect(P.slot, &r);
    *w = r.right;
    *h = r.bottom;
}

static int app_start(papp *a)
{
    panel_startup_trace("host.request");
    int64_t t = fpe_prof_begin();
    app_stop(a);
    if (!a->path[0] || GetFileAttributesW(a->path) == INVALID_FILE_ATTRIBUTES) {
        a->state = A_GONE;
        return -1;
    }
    a->title[0] = 0;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE from = NULL, to = NULL;
    launch_state *l = calloc(1, sizeof *l);
    a->launch = l;
    if (l) {
        l->refs = 1;
        l->to = P.hwnd;
        l->gen = a->gen;
        wcscpy(l->path, a->path);
    }
    reader_arg *ra = malloc(sizeof *ra);
    if (!l || !ra || !CreatePipe(&l->child_in, &to, &sa, 1 << 16) || !CreatePipe(&from, &l->child_out, &sa, 1 << 16)) {
        free(ra);
        if (from) CloseHandle(from);
        if (to) CloseHandle(to);
        app_stop(a);
        a->state = A_GONE;
        return -1;
    }
    SetHandleInformation(to, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(from, HANDLE_FLAG_INHERIT, 0);
    a->out = writer_start(to);
    if (!a->out) CloseHandle(to);
    *ra = (reader_arg){ from, P.hwnd, a->gen };
    HANDLE rt = a->out ? CreateThread(NULL, 64 * 1024, reader, ra, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL) : NULL;
    if (!rt) {
        free(ra);
        CloseHandle(from);
        app_stop(a);
        a->state = A_GONE;
        return -1;
    }
    CloseHandle(rt);
    InterlockedIncrement(&l->refs);
    HANDLE lt = CreateThread(NULL, 64 * 1024, launch, l, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!lt) {
        launch_unref(l); /* no worker owns the reserved reference */
        app_stop(a);
        a->state = A_GONE;
    } else {
        CloseHandle(lt);
        a->state = A_STARTING;
    }
    a->start_t0 = fpe_prof_begin();
    a->ping = a->pong = 0;
    watch_timer();
    repaint_header();
    FPE_PROF_END("panel.app_start", t);
    return lt ? 0 : -1;
}

/* What the app was told about being shown and having the keyboard, brought up to date. */
static void tell_state(papp *a)
{
    if (a->state != A_RUNNING && a->state != A_HUNG) return;
    uint32_t vis = is_shown(a), foc = vis && P.slot && GetFocus() == P.slot;
    if ((int)vis != a->visible) {
        a->visible = (int)vis;
        send_msg(a, PANELP_SHOW, &vis, 4);
    }
    if ((int)foc != a->focused) {
        a->focused = (int)foc;
        if (!foc) a->capture = 0;
        send_msg(a, PANELP_FOCUS, &foc, 4);
    }
}

static void tell_all(void)
{
    for (int i = 0; i < APPS; i++) tell_state(&P.apps[i]);
}

static void app_ended(papp *a)
{
    a->pipe_gone = 1;
    if (!launch_take(a)) return; /* EOF raced launch publication; completion retries this */
    a->exit_code = 0;
    a->exit_pending = a->proc && WaitForSingleObject(a->proc, 0) != WAIT_OBJECT_0;
    if (a->proc && !a->exit_pending) GetExitCodeProcess(a->proc, &a->exit_code);
    FPE_PROF_COUNT("panel.app_ended");
    if (!is_shown(a)) {
        app_forget(a);
        return;
    }
    a->state = A_GONE;
    watch_timer();
    repaint_header();
    if (P.slot) InvalidateRect(P.slot, NULL, FALSE);
}

/* The process exists: tell it where and how to draw. */
static void app_hello(papp *a)
{
    panel_startup_trace("host.hello.begin");
    if (!launch_take(a)) return;
    if (a->pipe_gone) { app_ended(a); return; }
    if (a->helloed) return;
    if (!a->proc) {
        app_stop(a);
        a->state = A_GONE;
        repaint_header();
        return;
    }
    int w, h;
    slot_size(&w, &h);
    panelp_hello hi = { PANELP_MAGIC, PANELP_VERSION, (uint32_t)px(96), (uint32_t)w, (uint32_t)h, 0, 0,
                        GetCurrentProcessId(), 0 };
    if (pixels_make(a, w, h)) {
        hi.section = pixels_handle_for_app(a);
        hi.seq = a->seq;
    }
    send_msg(a, PANELP_HELLO, &hi, sizeof hi);
    panel_startup_trace("host.hello.queued");
    a->helloed = 1;
    /* what was sent while it started, in order: messages one after another */
    for (size_t at = 0; at + sizeof(panelp_head) <= a->held_len;) {
        panelp_head h;
        memcpy(&h, a->held + at, sizeof h);
        send_msg(a, h.type, a->held + at + sizeof h, h.size);
        at += sizeof h + h.size;
    }
    free(a->held);
    a->held = NULL;
    a->held_len = 0;
    a->visible = 1; /* an app starts out shown */
    watch_timer();
}

static void resize_app(papp *a);

static void on_msg(papp *a, msg *m)
{
    switch (m->head.type) {
    case PANELP_READY:
        panel_startup_trace("host.ready.received");
        if (m->head.size >= sizeof(panelp_ready)) {
            panelp_ready r;
            memcpy(&r, m->data, sizeof r);
            r.title[63] = 0;
            wcscpy(a->title, r.title);
            a->state = A_RUNNING;
            FPE_PROF_END("panel.ready", a->start_t0);
            repaint_header();
            tell_state(a);
            if (!is_shown(a)) resize_app(a); /* started hidden: it still gets the slot's size */
            if (P.cfg.ready) P.cfg.ready(a->path);
        }
        break;
    case PANELP_FRAME:
        if (m->head.size >= sizeof(panelp_frame)) {
            panelp_frame f;
            memcpy(&f, m->data, sizeof f);
            if (f.seq != a->seq || !a->bmp[0]) break; /* drawn before the last resize */
            a->shown = f.buffer & 1;
            a->have_frame = 1;
            pixels_free_old(a);
            if (!a->framed) {
                panel_startup_trace("host.first_frame.received");
                FPE_PROF_END("panel.first_frame", a->start_t0);
            }
            a->framed = 1;
            if (a->input_t0) FPE_PROF_END("panel.input_to_frame", a->input_t0);
            if (a->resize_t0) FPE_PROF_END("panel.resize_to_frame", a->resize_t0);
            a->input_t0 = 0;
            a->resize_t0 = 0;
            a->resize_busy = 0;
            if (is_shown(a)) {
                a->ack_due = 1;
                RECT r = { f.dirty.x, f.dirty.y, f.dirty.x + f.dirty.w, f.dirty.y + f.dirty.h };
                InvalidateRect(P.slot, &r, FALSE);
            } else {
                send_msg(a, PANELP_FRAME_ACK, NULL, 0); /* not painted: the other buffer is free at once */
            }
            if (a->resize_pending) resize_app(a);
        }
        break;
    case PANELP_CURSOR:
        if (m->head.size >= 4) memcpy(&a->cursor, m->data, 4);
        break;
    case PANELP_CARET:
        if (m->head.size >= sizeof(panelp_rect)) memcpy(&a->caret, m->data, sizeof a->caret);
        break;
    case PANELP_PONG:
        if (m->head.size >= 4) memcpy(&a->pong, m->data, 4);
        if (a->pong == a->ping && a->state == A_HUNG) {
            a->state = A_RUNNING;
            repaint_header();
        }
        break;
    case PANELP_CLOSE:
        if (is_shown(a)) sidepanel_close();
        break;
    case PANELP_CAPTURE:
        a->capture = m->head.size >= 4 && *(const uint32_t *)m->data && is_shown(a) && GetFocus() == P.slot;
        break;
    default:
        if (P.cfg.message) P.cfg.message(a->path, m->head.type, m->data, m->head.size);
        break;
    }
}

static void watchdog(void)
{
    uint64_t now = GetTickCount64();
    for (int i = 0; i < APPS; i++) {
        papp *a = &P.apps[i];
        if (a->exit_pending && a->proc && WaitForSingleObject(a->proc, 0) == WAIT_OBJECT_0) {
            GetExitCodeProcess(a->proc, &a->exit_code);
            a->exit_pending = 0;
            watch_timer();
            repaint_header();
        }
        if (a->state == A_STARTING) { app_hello(a); continue; }
        if (a->state != A_RUNNING && a->state != A_HUNG) continue;
        if (a->resize_busy && now - a->resize_at > HUNG_MS) { /* no frame for that size: don't wait for it any longer */
            a->resize_busy = 0;
            if (a->resize_pending) resize_app(a);
        }
        if (a->pong == a->ping) {
            a->ping++;
            a->ping_at = now;
            send_msg(a, PANELP_PING, &a->ping, 4);
        } else if (now - a->ping_at > HUNG_MS && a->state == A_RUNNING) {
            a->state = A_HUNG;
            FPE_PROF_COUNT("panel.hung");
            repaint_header();
        }
    }
}

/* ---- slide ------------------------------------------------------------------------------ */

static void slide_step(void)
{
    double t = (double)(GetTickCount64() - P.slide_t0) / SLIDE_MS;
    if (t >= 1) {
        t = 1;
        P.sliding = 0;
        KillTimer(P.hwnd, TIMER_SLIDE);
    }
    double e = 1 - (1 - t) * (1 - t) * (1 - t); /* ease out */
    int from = P.x + P.w, x = from - (int)((from - P.x) * e + 0.5);
    SetWindowPos(P.hwnd, NULL, x, P.y, P.w, P.h, SWP_NOZORDER | SWP_NOACTIVATE);
}

/* ---- painting ---------------------------------------------------------------------------- */

static void fill(HDC dc, int x, int y, int w, int h, COLORREF c)
{
    RECT r = { x, y, x + w, y + h };
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

static int text_w(HDC dc, const wchar_t *s)
{
    SIZE sz;
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    return sz.cx;
}

static void text(HDC dc, const wchar_t *s, RECT r, COLORREF c, UINT fmt)
{
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &r, fmt | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

static const wchar_t *app_label(const papp *a)
{
    if (a->title[0]) return a->title;
    const wchar_t *s = wcsrchr(a->path, L'\\');
    return s ? s + 1 : a->path;
}

static void paint_container(HDC dc, int w, int h)
{
    SelectObject(dc, app_font());
    fill(dc, 0, 0, w, h, C_BG);
    fill(dc, 0, 0, grip_w(), h, C_SPLIT);
    int hh = head_h(), x = grip_w() + px(10), right = w - px(8);
    fill(dc, grip_w(), 0, w - grip_w(), hh, C_HEADER);
    papp *a = P.cur;

    P.r_close = (RECT){ right - px(24), 0, right, hh };
    text(dc, L"\x2715", P.r_close, C_DIM, DT_CENTER);
    right = P.r_close.left - px(6);

    SetRectEmpty(&P.r_restart);
    if (a && (a->state == A_HUNG || a->state == A_GONE)) {
        int rw = text_w(dc, L"Restart") + px(16);
        P.r_restart = (RECT){ right - rw, px(5), right, hh - px(5) };
        fill(dc, P.r_restart.left, P.r_restart.top, rw, P.r_restart.bottom - P.r_restart.top, C_EDIT);
        text(dc, L"Restart", P.r_restart, C_TEXT, DT_CENTER);
        right = P.r_restart.left - px(8);
    }

    wchar_t gone[48] = L"stopped";
    const wchar_t *state = L"";
    if (a) {
        if (a->exit_code >= 0xC0000000u) _snwprintf(gone, 48, L"crashed (0x%08lX)", a->exit_code);
        else if (a->exit_code) _snwprintf(gone, 48, L"stopped (exit %lu)", a->exit_code);
        state = a->state == A_STARTING ? L"starting\x2026" : a->state == A_HUNG ? L"not responding" : a->state == A_GONE ? gone : L"";
    }
    int sw = state[0] ? text_w(dc, state) + px(8) : 0;
    if (sw) {
        RECT r = { right - sw, 0, right, hh };
        text(dc, state, r, a->state == A_STARTING ? C_DIM : C_ERROR, DT_RIGHT);
        right -= sw + px(6);
    }

    /* a tab per app; with one it is just the title */
    int n = 0;
    for (int i = 0; i < APPS; i++) n += P.apps[i].path[0] != 0;
    for (int i = 0; i < APPS; i++) {
        papp *t = &P.apps[i];
        SetRectEmpty(&t->r_tab);
        if (!t->path[0]) continue;
        int tw = text_w(dc, app_label(t)) + (n > 1 ? px(20) : 0);
        if (x + tw > right) tw = right - x;
        if (tw <= px(16)) continue;
        t->r_tab = (RECT){ x, 0, x + tw, hh };
        if (n > 1) {
            if (t == a) fill(dc, x, 0, tw, hh, C_BG), fill(dc, x, hh - px(2), tw, px(2), C_ACCENT);
            RECT r = { x + px(10), 0, x + tw - px(10), hh };
            text(dc, app_label(t), r, t == a ? C_TEXT : C_DIM, DT_CENTER);
        } else {
            text(dc, app_label(t), t->r_tab, C_TEXT, DT_LEFT);
        }
        x += tw + px(2);
    }
}

/* Keep client coordinates for drawing and hit geometry while buffering only
 * the damaged rectangle. BeginPaint keeps the destination's exact clip. */
static void paint_container_buffer(HDC target, int w, int h, const RECT *damage)
{
    RECT client = { 0, 0, w, h }, area;
    if (!IntersectRect(&area, &client, damage)) return;
    HDC mem = CreateCompatibleDC(target);
    HBITMAP bitmap = NULL;
    HGDIOBJ old = NULL;
    if (mem) {
        bitmap = CreateCompatibleBitmap(target, area.right - area.left, area.bottom - area.top);
        if (bitmap) {
            old = SelectObject(mem, bitmap);
            if (old == HGDI_ERROR) old = NULL;
        }
    }
    int ready = old && SetWindowOrgEx(mem, area.left, area.top, NULL) &&
        IntersectClipRect(mem, area.left, area.top, area.right, area.bottom) != ERROR;
    if (ready) {
        paint_container(mem, w, h);
        if (!BitBlt(target, area.left, area.top, area.right - area.left, area.bottom - area.top,
                    mem, area.left, area.top, SRCCOPY))
            paint_container(target, w, h);
    } else {
        paint_container(target, w, h);
    }
    if (old) SelectObject(mem, old);
    if (bitmap) DeleteObject(bitmap);
    if (mem) DeleteDC(mem);
}

static void paint_slot(HDC dc, int w, int h, const RECT *damage)
{
    RECT client = { 0, 0, w, h }, area;
    if (!IntersectRect(&area, &client, damage)) return;
    papp *a = P.cur;
    HBITMAP b = !a ? NULL : a->have_frame ? a->bmp[a->shown] : a->old_bmp[a->shown] ? a->old_bmp[a->shown] : NULL;
    int bw = !a ? 0 : a->have_frame ? a->bw : a->old_w, bh = !a ? 0 : a->have_frame ? a->bh : a->old_h;
    if (b) {
        RECT image = { 0, 0, bw < w ? bw : w, bh < h ? bh : h }, copy;
        if (IntersectRect(&copy, &area, &image)) {
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ old = mem ? SelectObject(mem, b) : NULL;
            if (old && old != HGDI_ERROR) {
                if (!BitBlt(dc, copy.left, copy.top, copy.right - copy.left, copy.bottom - copy.top,
                            mem, copy.left, copy.top, SRCCOPY))
                    fill(dc, copy.left, copy.top, copy.right - copy.left, copy.bottom - copy.top, C_BG);
                SelectObject(mem, old);
            } else {
                fill(dc, copy.left, copy.top, copy.right - copy.left, copy.bottom - copy.top, C_BG);
            }
            if (mem) DeleteDC(mem);
        }
        RECT uncovered;
        if (bw < w && IntersectRect(&uncovered, &area, &(RECT){ bw, 0, w, h }))
            fill(dc, uncovered.left, uncovered.top, uncovered.right - uncovered.left, uncovered.bottom - uncovered.top, C_BG);
        if (bh < h && IntersectRect(&uncovered, &area, &(RECT){ 0, bh, w, h }))
            fill(dc, uncovered.left, uncovered.top, uncovered.right - uncovered.left, uncovered.bottom - uncovered.top, C_BG);
    } else {
        fill(dc, area.left, area.top, area.right - area.left, area.bottom - area.top, C_BG);
    }
}

/* ---- input ------------------------------------------------------------------------------- */

static uint32_t mods(void)
{
    return (GetKeyState(VK_CONTROL) < 0 ? PANELP_M_CTRL : 0) | (GetKeyState(VK_SHIFT) < 0 ? PANELP_M_SHIFT : 0) |
           (GetKeyState(VK_MENU) < 0 ? PANELP_M_ALT : 0);
}

static void forward(UINT m, WPARAM w, LPARAM l, int mouse)
{
    papp *a = P.cur;
    if (!a || a->state != A_RUNNING) return; /* a hung app gets nothing to pile up */
    panelp_input in = { m, (uint32_t)w, 0, 0, 0, 0, (uint32_t)l, mods() };
    if (mouse) {
        POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        if (m == WM_MOUSEWHEEL) ScreenToClient(P.slot, &pt);
        in.x = pt.x;
        in.y = pt.y;
        POINT sp = pt;
        ClientToScreen(P.slot, &sp);
        in.sx = sp.x;
        in.sy = sp.y;
    }
    if (!a->input_t0 && m == WM_CHAR) a->input_t0 = fpe_prof_begin(); /* other input may draw nothing */
    send_msg(a, PANELP_INPUT, &in, sizeof in);
}

/* An app is told the slot's new size, one size at a time: a drag makes
 * hundreds, and an app laying out for each in turn would fall seconds
 * behind. While it draws one, the next waits (only the latest is kept) and
 * the last frame stays on screen, cropped or padded. */
static void resize_app(papp *a)
{
    int w, h;
    slot_size(&w, &h);
    if (!a || (a->state != A_RUNNING && a->state != A_HUNG) || w <= 0 || h <= 0) return;
    if (w == a->bw && h == a->bh) {
        a->resize_pending = 0;
        return;
    }
    if (a->resize_busy) {
        a->resize_pending = 1;
        return;
    }
    if (!pixels_make(a, w, h)) return;
    panelp_size s = { (uint32_t)w, (uint32_t)h, (uint32_t)px(96), a->seq, pixels_handle_for_app(a) };
    if (!a->resize_t0) a->resize_t0 = fpe_prof_begin();
    a->resize_busy = 1;
    a->resize_pending = 0;
    a->resize_at = GetTickCount64();
    send_msg(a, PANELP_RESIZE, &s, sizeof s);
}

static LRESULT CALLBACK slot_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    papp *a = P.cur;
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        paint_slot(dc, r.right, r.bottom, &ps.rcPaint);
        EndPaint(h, &ps);
        if (a && a->ack_due) {
            a->ack_due = 0;
            send_msg(a, PANELP_FRAME_ACK, NULL, 0);
        }
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
        resize_app(a);
        return 0;
    case WM_MOUSEACTIVATE: return MA_ACTIVATE;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        if(P.cfg.focus)P.cfg.focus(h);else SetFocus(h);
        if(GetForegroundWindow()==P.parent)SetCapture(h);
        forward(m, w, l, 1);
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        if (!(w & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON))) ReleaseCapture();
        forward(m, w, l, 1);
        return 0;
    case WM_LBUTTONDBLCLK:
    case WM_MOUSEWHEEL: forward(m, w, l, 1); return 0;
    case WM_MOUSEMOVE: {
        uint64_t now = GetTickCount64();
        if (now - P.last_move < 8 && !(w & MK_LBUTTON)) return 0; /* hover at most ~120 Hz */
        P.last_move = now;
        forward(m, w, l, 1);
        return 0;
    }
    case WM_KEYDOWN:
        if (!(a && a->capture) && P.cfg.key && P.cfg.key((UINT)w)) return 0;
        forward(m, w, l, 0);
        return 0;
    case WM_KEYUP:
    case WM_CHAR: forward(m, w, l, 0); return 0;
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_SYSCHAR:
        if (!a || !a->capture) break; /* Alt+F4 and the window menu as usual */
        if (m == WM_SYSKEYDOWN) forward(m, w, l, 0);
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        tell_all();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && a && a->cursor) {
            SetCursor(LoadCursorW(NULL, MAKEINTRESOURCEW(a->cursor)));
            return TRUE;
        }
        break;
    case WM_IME_STARTCOMPOSITION: {
        /* the composition window goes where the app says text is typed */
        HIMC imc = ImmGetContext(h);
        if (imc && a) {
            COMPOSITIONFORM cf = { CFS_POINT, { a->caret.x, a->caret.y + a->caret.h }, { 0 } };
            ImmSetCompositionWindow(imc, &cf);
            CANDIDATEFORM cand = { 0, CFS_CANDIDATEPOS, { a->caret.x, a->caret.y + a->caret.h }, { 0 } };
            ImmSetCandidateWindow(imc, &cand);
        }
        if (imc) ImmReleaseContext(h, imc);
        break;
    }
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ---- container ------------------------------------------------------------------------- */

static int in_rect(const RECT *r, int x, int y) { return x >= r->left && x < r->right && y >= r->top && y < r->bottom; }

/* a becomes the app shown: hidden ones that don't keep running end */
static void show_app(papp *a, int focus)
{
    papp *was = P.cur;
    P.cur = a;
    if (was != a && P.cfg.relayout) P.cfg.relayout();
    if (was && was != a && was->path[0] && (!(was->flags & SIDEPANEL_KEEP) || was->state == A_GONE)) app_forget(was);
    if (a->state == A_OFF || a->state == A_GONE) app_start(a);
    else resize_app(a);
    if (focus && P.slot) PostMessageW(P.hwnd, WM_PANEL_FOCUS, a->gen, 0);
    tell_all();
    repaint_header();
    if (P.slot) InvalidateRect(P.slot, NULL, FALSE);
}

static LRESULT CALLBACK panel_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        paint_container_buffer(dc, r.right, r.bottom, &ps.rcPaint);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
        if (P.slot) {
            MoveWindow(P.slot, grip_w(), head_h(), LOWORD(l) - grip_w(), HIWORD(l) - head_h(), FALSE);
            InvalidateRect(P.slot, NULL, FALSE);
        }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            if (pt.x < grip_w() || P.dragging) {
                SetCursor(LoadCursorW(NULL, IDC_SIZEWE));
                return TRUE;
            }
        }
        break;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        if (x < grip_w()) {
            P.dragging = 1;
            SetCapture(h);
        } else if (in_rect(&P.r_close, x, y)) {
            sidepanel_close();
        } else if (in_rect(&P.r_restart, x, y)) {
            if (P.cur) app_start(P.cur);
        } else {
            for (int i = 0; i < APPS; i++)
                if (P.apps[i].path[0] && &P.apps[i] != P.cur && in_rect(&P.apps[i].r_tab, x, y)) {
                    show_app(&P.apps[i], 1);
                    break;
                }
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (P.dragging) {
            MSG q; /* only where the mouse is now matters: moves already queued are skipped */
            while (PeekMessageW(&q, h, WM_MOUSEFIRST, WM_MOUSELAST, PM_NOREMOVE) && q.message == WM_MOUSEMOVE) {
                PeekMessageW(&q, h, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_REMOVE);
                l = q.lParam;
            }
            POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) }; /* relative to where the panel is now */
            MapWindowPoints(h, P.parent, &pt, 1);
            P.width = P.x + P.w - pt.x;
            if (P.cfg.relayout) P.cfg.relayout();
            /* painted before the next move is taken, or moves would keep the paints waiting */
            RedrawWindow(P.parent, NULL, NULL, RDW_UPDATENOW | RDW_ALLCHILDREN);
        }
        return 0;
    case WM_LBUTTONUP:
        if (P.dragging) {
            P.dragging = 0;
            ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED: P.dragging = 0; return 0;
    case WM_TIMER:
        if (w == TIMER_SLIDE) slide_step();
        else if (w == TIMER_WATCH) watchdog();
        return 0;
    case WM_PANEL_FOCUS:
        if (P.open && P.cur && P.cur == by_gen((uint32_t)w)) {
            int64_t tf = fpe_prof_begin();
            if (P.cfg.focus) P.cfg.focus(P.slot); else SetFocus(P.slot);
            FPE_PROF_END("panel.focus", tf);
        }
        return 0;
    case WM_PANEL_LAUNCHED: {
        papp *a = by_gen((uint32_t)w);
        if (a) app_hello(a);
        return 0;
    }
    case WM_PANEL_MSG: {
        papp *a = by_gen((uint32_t)w);
        if (a) on_msg(a, (msg *)l);
        free((void *)l);
        return 0;
    }
    case WM_PANEL_GONE: {
        papp *a = by_gen((uint32_t)w);
        if (a && a->state != A_OFF) app_ended(a);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

static int create_windows(HWND parent)
{
    if (P.hwnd) return 1;
    static int registered;
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc = panel_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = L"OpenPilotSide";
        RegisterClassW(&wc);
        wc.lpfnWndProc = slot_proc;
        wc.style = CS_DBLCLKS;
        wc.lpszClassName = L"OpenPilotSideSlot";
        RegisterClassW(&wc);
        registered = 1;
    }
    P.parent = parent;
    P.hwnd = CreateWindowExW(0, L"OpenPilotSide", L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent, NULL, inst, NULL);
    P.slot = P.hwnd ? CreateWindowExW(0, L"OpenPilotSideSlot", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, P.hwnd, NULL, inst, NULL)
                    : NULL;
    return P.slot != NULL;
}

/* ---- public ------------------------------------------------------------------------------ */

void sidepanel_configure(const sidepanel_config *cfg) { P.cfg = *cfg; }

int sidepanel_open(void) { return P.open; }
HWND sidepanel_window(void) { return P.hwnd; }
const wchar_t *sidepanel_app(void) { return P.open && P.cur ? P.cur->path : NULL; }
int sidepanel_has_focus(void) { return P.open && P.slot && GetFocus() == P.slot; }

int sidepanel_running(const wchar_t *app)
{
    papp *a = app ? by_path(app) : NULL;
    return a && live(a);
}

uint32_t sidepanel_generation(const wchar_t *app)
{
    papp *a = app ? by_path(app) : NULL;
    return a ? a->gen : 0;
}

int sidepanel_send(const wchar_t *app, uint32_t type, const void *data, uint32_t size)
{
    papp *a = app ? by_path(app) : P.open ? P.cur : NULL;
    return a && live(a) && send_msg(a, type, data, size);
}

int sidepanel_width(int avail)
{
    if (!P.open) return 0;
    int w = P.width ? P.width : px(420), lo = px(260), hi = avail * 7 / 10;
    if (w > hi) w = hi;
    if (w < lo) w = lo;
    return w;
}

void sidepanel_place(int x, int y, int w, int h)
{
    if (!P.hwnd) return;
    int first = !IsWindowVisible(P.hwnd);
    P.x = x, P.y = y, P.w = w, P.h = h;
    if (first && P.cfg.slide) {
        P.sliding = 1;
        P.slide_t0 = GetTickCount64();
        SetWindowPos(P.hwnd, HWND_TOP, x + w, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOREDRAW | SWP_DEFERERASE);
        SetTimer(P.hwnd, TIMER_SLIDE, 8, NULL);
    } else if (P.sliding) {
        slide_step();
    } else {
        RECT r;
        GetWindowRect(P.hwnd, &r);
        MapWindowPoints(NULL, P.parent, (POINT *)&r, 2);
        if (first || r.left != x || r.top != y || r.right - r.left != w || r.bottom - r.top != h)
            SetWindowPos(P.hwnd, HWND_TOP, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOCOPYBITS | SWP_NOREDRAW | SWP_DEFERERASE);
    }
}

int sidepanel_show(HWND parent, const wchar_t *app, unsigned flags, int focus)
{
    int64_t t = fpe_prof_begin();
    if (!create_windows(parent)) return -1;
    papp *a = by_path(app);
    if (!a) {
        for (int i = 0; i < APPS && !a; i++)
            if (!P.apps[i].path[0]) a = &P.apps[i];
        /* all taken: the oldest hidden one gives way */
        for (int i = 0; i < APPS && !a; i++)
            if (&P.apps[i] != P.cur) app_forget(a = &P.apps[i]);
        if (!a) return -1;
        _snwprintf(a->path, MAX_PATH, L"%ls", app);
        a->path[MAX_PATH - 1] = 0;
        a->state = A_OFF;
    }
    a->flags = flags;
    if (!P.open) {
        P.open = 1;
        P.cur = a;                        /* the layout sizes the slot for it */
        if (P.cfg.relayout) P.cfg.relayout(); /* places it, so the app starts at its size */
    }
    show_app(a, focus);
    FPE_PROF_END("panel.open", t);
    return a->state == A_GONE ? -1 : 0;
}

int sidepanel_toggle(HWND parent, const wchar_t *app, unsigned flags)
{
    if (P.open && P.cur && !_wcsicmp(P.cur->path, app)) {
        sidepanel_close();
        return 1;
    }
    return sidepanel_show(parent, app, flags, 1);
}

void sidepanel_close(void)
{
    if (!P.open) return;
    int had_focus = P.hwnd && (GetFocus() == P.slot || IsChild(P.hwnd, GetFocus()));
    P.open = 0;
    P.sliding = 0;
    KillTimer(P.hwnd, TIMER_SLIDE);
    ShowWindow(P.hwnd, SW_HIDE);
    for (int i = 0; i < APPS; i++) {
        papp *a = &P.apps[i];
        if (a->path[0] && (!(a->flags & SIDEPANEL_KEEP) || !live(a))) app_forget(a);
    }
    tell_all();
    if (P.cfg.relayout) P.cfg.relayout();
    if (P.cfg.closed) P.cfg.closed(had_focus);
}

void sidepanel_focus(void)
{
    if (P.open) { if (P.cfg.focus) P.cfg.focus(P.slot); else SetFocus(P.slot); }
}

void sidepanel_shutdown(void)
{
    if (P.open) {
        P.open = 0;
        ShowWindow(P.hwnd, SW_HIDE);
    }
    /* Reapers allow a graceful quit without holding the window open. Jobs also
     * contain pending launches and end all helpers if the host exits first. */
    for (int i = 0; i < APPS; i++) app_stop(&P.apps[i]);
}

void sidepanel_stop_app(const wchar_t *path)
{
    for(int i=0;i<APPS;i++){papp *a=&P.apps[i];if(a->path[0]&&!_wcsicmp(a->path,path)){if(P.cur==a&&P.open)sidepanel_close();app_forget(a);break;}}
}
