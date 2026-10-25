#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include "folderwatch.h"

#define FOLDERWATCH_CHANGED (WM_APP + 39)

typedef struct watch_owner {
    struct watch_owner *next;
    fpe_watch *watch;
    HWND sink;
    unsigned pane;
    UINT_PTR generation;
    LONG retired;
    folderwatch_notify notify;
    void *user; /* read only by the UI thread, never by a watcher or reaper */
} watch_owner;

static struct {
    HWND sink;
    HANDLE wake, thread;
    SRWLOCK lock;
    watch_owner *head, *tail, *active[2];
    int stopping;
} F;
/* Not reset on join: even a new manager cannot accept an old generation. */
static UINT_PTR generation;

static LRESULT CALLBACK sink_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == FOLDERWATCH_CHANGED) {
        if (w < 2 && !F.stopping) {
            watch_owner *r = F.active[w];
            if (r && r->generation == (UINT_PTR)l) {
                /* notify may navigate and retire r; do not access r afterward. */
                folderwatch_notify notify = r->notify;
                void *user = r->user;
                notify(user);
            }
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void changed(void *user, fpe_watch *watch)
{
    watch_owner *r = user;
    if (!InterlockedCompareExchange(&r->retired, 0, 0))
        PostMessageW(r->sink, FOLDERWATCH_CHANGED, r->pane, (LPARAM)r->generation);
    /* A post racing retirement is harmless: the UI checks the generation.
     * The message-only sink outlives every callback, including during close. */
}

static DWORD WINAPI reap(void *arg)
{
    for (;;) {
        WaitForSingleObject(F.wake, INFINITE);
        for (;;) {
            AcquireSRWLockExclusive(&F.lock);
            watch_owner *r = F.head;
            if (r) {
                F.head = r->next;
                if (!F.head) F.tail = NULL;
            }
            int done = F.stopping && !r;
            ReleaseSRWLockExclusive(&F.lock);
            if (done) return 0;
            if (!r) break;
            /* Never hold the queue lock across an engine join or helper IO. */
            fpe_watch_stop(r->watch);
            free(r);
        }
    }
}

static fpe_err init(void)
{
    if (F.thread) return F.stopping ? ERROR_SHUTDOWN_IN_PROGRESS : 0;
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = sink_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"OpenPilotFolderWatch";
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return GetLastError();
    F.sink = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, inst, NULL);
    if (!F.sink) return GetLastError();
    F.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (F.wake) F.thread = CreateThread(NULL, 64 * 1024, reap, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!F.thread) {
        DWORD err = GetLastError();
        if (F.wake) CloseHandle(F.wake);
        DestroyWindow(F.sink);
        F.wake = NULL; F.sink = NULL;
        return err ? err : ERROR_NOT_ENOUGH_MEMORY;
    }
    return 0;
}

static void retire(unsigned pane)
{
    watch_owner *r = F.active[pane];
    F.active[pane] = NULL; /* invalidate queued notifications before cleanup */
    if (!r) return;
    InterlockedExchange(&r->retired, 1);
    AcquireSRWLockExclusive(&F.lock);
    if (F.tail) F.tail->next = r;
    else F.head = r;
    F.tail = r;
    ReleaseSRWLockExclusive(&F.lock);
    SetEvent(F.wake);
}

fpe_err folderwatch_clear(unsigned pane)
{
    if (pane >= 2) return ERROR_INVALID_PARAMETER;
    retire(pane); /* No initialization, filesystem probe or blocking stop. */
    return 0;
}

fpe_err folderwatch_set(unsigned pane, const wchar_t *path, folderwatch_notify notify, void *user)
{
    if (pane >= 2 || !path || !notify) return ERROR_INVALID_PARAMETER;
    fpe_err err = init();
    if (err) return err;
    retire(pane);
    if (generation == UINTPTR_MAX) return ERROR_ARITHMETIC_OVERFLOW;
    watch_owner *r = calloc(1, sizeof *r);
    if (!r) return ERROR_NOT_ENOUGH_MEMORY;
    r->sink = F.sink; r->pane = pane; r->generation = ++generation;
    r->notify = notify; r->user = user;
    err = fpe_watch_start(path, 80, changed, r, &r->watch);
    if (err) { free(r); return err; }
    F.active[pane] = r;
    return 0;
}

void folderwatch_shutdown(void)
{
    if (!F.thread || F.stopping) return;
    retire(0); retire(1);
    AcquireSRWLockExclusive(&F.lock);
    F.stopping = 1;
    ReleaseSRWLockExclusive(&F.lock);
    SetEvent(F.wake);
}

void folderwatch_join(void)
{
    if (!F.thread) return;
    folderwatch_shutdown();
    WaitForSingleObject(F.thread, INFINITE);
    /* No watcher can post to this HWND after it is destroyed or reused. */
    DestroyWindow(F.sink);
    CloseHandle(F.thread); CloseHandle(F.wake);
    ZeroMemory(&F, sizeof F);
}
