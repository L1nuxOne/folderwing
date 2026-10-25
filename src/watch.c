#include "internal.h"

struct fpe_watch {
    HANDLE       dir;
    HANDLE       thread;
    HANDLE       stop;
    HANDLE       ev;
    OVERLAPPED   ov;
    fpe_watch_cb cb;
    void        *user;
    uint32_t     debounce_ms;
    wsl_watch   *wsl; /* a folder in a WSL distro: its helper sets ev (dir is unused) */
    DWORD        buf[4096]; /* FILE_NOTIFY_INFORMATION needs DWORD alignment */
};

static fpe_gauge g_watches = FPE_GAUGE_INIT("watch.live");

static void fire(fpe_watch *w)
{
    int64_t t = fpe_prof_begin();
    w->cb(w->user, w);
    FPE_PROF_END("watch.fire", t);
}

#define WATCH_FILTER (FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | \
                      FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |    \
                      FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION)

static int arm(fpe_watch *w)
{
    memset(&w->ov, 0, sizeof w->ov);
    w->ov.hEvent = w->ev;
    ResetEvent(w->ev);
    return ReadDirectoryChangesW(w->dir, w->buf, sizeof w->buf, FALSE, WATCH_FILTER, NULL, &w->ov, NULL);
}

/* Changes are coalesced: the callback fires once things have been quiet for
 * debounce_ms, but no later than 4 * debounce_ms after the first change, so a
 * folder under constant churn (downloads, builds) still refreshes. Only "the
 * folder changed" is reported; consumers reload, which is cheap and immune to
 * notification buffer overflows. */
static DWORD WINAPI watch_thread(LPVOID p)
{
    fpe_watch *w = p;
    int armed = w->wsl ? 1 : arm(w);
    int pending = 0;
    uint64_t first = 0, last = 0;
    const uint64_t quiet = w->debounce_ms, cap = (uint64_t)w->debounce_ms * 4;

    if (!armed) fire(w);

    for (;;) {
        DWORD timeout = INFINITE;
        if (pending) {
            uint64_t now = GetTickCount64();
            uint64_t due_quiet = last + quiet, due_cap = first + cap;
            uint64_t due = due_quiet < due_cap ? due_quiet : due_cap;
            timeout = due > now ? (DWORD)(due - now) : 0;
        }

        HANDLE hs[2] = { w->stop, w->ev };
        DWORD r = WaitForMultipleObjects(armed ? 2 : 1, hs, FALSE, timeout);

        if (r == WAIT_OBJECT_0) break;

        if (r == WAIT_OBJECT_0 + 1 && w->wsl) {
            ResetEvent(w->ev);
            uint64_t now = GetTickCount64();
            if (!pending) { pending = 1; first = now; }
            last = now;
            continue;
        }

        if (r == WAIT_OBJECT_0 + 1) {
            DWORD n = 0;
            BOOL ok = GetOverlappedResult(w->dir, &w->ov, &n, FALSE);
            uint64_t now = GetTickCount64();
            if (!pending) { pending = 1; first = now; }
            last = now;
            /* A zero-byte success means the kernel buffer overflowed: still
             * just "something changed". A failure means the folder is gone. */
            armed = ok && arm(w);
            continue;
        }

        if (r == WAIT_TIMEOUT && pending) {
            pending = 0;
            fire(w);
            continue;
        }

        if (r == WAIT_FAILED) break;
    }

    if (armed && !w->wsl) {
        CancelIoEx(w->dir, &w->ov);
        DWORD n;
        GetOverlappedResult(w->dir, &w->ov, &n, TRUE);
    }
    return 0;
}

FPE_API fpe_err fpe_watch_start(const wchar_t *path, uint32_t debounce_ms,
                                fpe_watch_cb cb, void *user, fpe_watch **out)
{
    if (!out || !cb) return ERROR_INVALID_PARAMETER;
    *out = NULL;

    wchar_t *disp = NULL, *nt = NULL;
    fpe_err err = fpe_path_display(path, &disp);
    if (!err) err = fpe_path_nt(disp, &nt);
    if (err) { fpe_free(disp); fpe_free(nt); return err; }

    fpe_watch *w = fpe_zalloc(sizeof *w);
    if (!w) { fpe_free(disp); fpe_free(nt); return ERROR_NOT_ENOUGH_MEMORY; }
    w->cb = cb;
    w->user = user;
    w->debounce_ms = debounce_ms ? debounce_ms : 60;
    w->dir = INVALID_HANDLE_VALUE;
    w->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    w->ev = CreateEventW(NULL, TRUE, FALSE, NULL);

    /* \\wsl.localhost passes no change notifications; the helper's inotify does */
    if (w->ev && fpe_wsl_owns(disp)) w->wsl = fpe_wsl_watch(disp, w->ev);
    int native_only=fpe_wsl_requires_native(disp);fpe_free(disp);
    if(!w->wsl&&native_only)err=ERROR_NOT_SUPPORTED;
    if (!w->wsl&&!native_only) {
        /* FILE_SHARE_DELETE lets users delete or rename the watched folder. */
        w->dir = CreateFileW(nt, FILE_LIST_DIRECTORY,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
        err = w->dir == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    }
    fpe_free(nt);
    if (!err && w->stop && w->ev)
        w->thread = CreateThread(NULL, 64 * 1024, watch_thread, w, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!w->thread) {
        if (!err) err = GetLastError();
        fpe_wsl_unwatch(w->wsl);
        if (w->stop) CloseHandle(w->stop);
        if (w->ev) CloseHandle(w->ev);
        if (w->dir != INVALID_HANDLE_VALUE) CloseHandle(w->dir);
        fpe_free(w);
        return err ? err : ERROR_NOT_ENOUGH_MEMORY;
    }
    fpe_gauge_add(&g_watches, 1);
    *out = w;
    return 0;
}

FPE_API void fpe_watch_stop(fpe_watch *w)
{
    if (!w) return;
    fpe_gauge_add(&g_watches, -1);
    SetEvent(w->stop);
    WaitForSingleObject(w->thread, INFINITE);
    fpe_wsl_unwatch(w->wsl); /* before ev goes: the helper's reader sets it */
    CloseHandle(w->thread);
    CloseHandle(w->stop);
    CloseHandle(w->ev);
    if (w->dir != INVALID_HANDLE_VALUE) CloseHandle(w->dir);
    fpe_free(w);
}
