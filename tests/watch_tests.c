/* Production host ownership code with private engine/Win32 wrappers. Explicit
 * event barriers hold teardown and posting; the shipped host has no test hooks. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../app/folderwatch.h"

struct fpe_watch {
    fpe_watch_cb cb;
    void *user;
    HANDLE entered, release, stopped, callback;
    DWORD stop_thread;
    int stops;
};
static fpe_watch watches[256];
static int nwatches, fail_start, fail_thread;
static LONG post_hold;
static HANDLE post_entered, post_release;
static DWORD ui_thread;
static int passed, failed, notifications[2], wrong_thread;
#define VERIFY(label, cond) do { if (cond) { passed++; printf("  ok    %s\n", label); } \
    else { failed++; printf("  FAIL  %s (line %d)\n", label, __LINE__); } fflush(stdout); } while (0)
static void require(int cond) { if (!cond) { puts("FATAL: test barrier/resource unavailable"); exit(2); } }
static fpe_err fake_start(const wchar_t *path, uint32_t debounce, fpe_watch_cb cb, void *user, fpe_watch **out)
{
    *out = NULL;
    if (fail_start) return ERROR_PATH_NOT_FOUND;
    require(nwatches < 256);
    fpe_watch *w = &watches[nwatches++];
    w->cb = cb; w->user = user;
    w->entered = CreateEventW(NULL, TRUE, FALSE, NULL);
    w->release = CreateEventW(NULL, TRUE, TRUE, NULL);
    w->stopped = CreateEventW(NULL, TRUE, FALSE, NULL);
    require(w->entered && w->release && w->stopped);
    *out = w;
    return 0;
}
static void fake_stop(fpe_watch *w)
{
    w->stop_thread = GetCurrentThreadId(); w->stops++;
    SetEvent(w->entered);
    /* Bound broken-code experiments; ordinary tests release these explicitly. */
    require(WaitForSingleObject(w->release, 10000) == WAIT_OBJECT_0);
    if (w->callback) require(WaitForSingleObject(w->callback, 10000) == WAIT_OBJECT_0);
    SetEvent(w->stopped);
}
static BOOL WINAPI held_post(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (InterlockedExchange(&post_hold, 0)) {
        SetEvent(post_entered);
        require(WaitForSingleObject(post_release, 10000) == WAIT_OBJECT_0);
    }
    return PostMessageW(h, m, w, l);
}
static HANDLE WINAPI maybe_thread(LPSECURITY_ATTRIBUTES sa, SIZE_T size, LPTHREAD_START_ROUTINE run,
                                  LPVOID arg, DWORD flags, LPDWORD id)
{
    if (fail_thread) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    return CreateThread(sa, size, run, arg, flags, id);
}
#define fpe_watch_start fake_start
#define fpe_watch_stop fake_stop
#define PostMessageW held_post
#define CreateThread maybe_thread
#ifndef FOLDERWATCH_SOURCE
#define FOLDERWATCH_SOURCE "../app/folderwatch.c"
#endif
#include FOLDERWATCH_SOURCE
#undef fpe_watch_start
#undef fpe_watch_stop
#undef PostMessageW
#undef CreateThread

static void notify(void *user)
{
    if (GetCurrentThreadId() != ui_thread) wrong_thread++;
    notifications[(UINT_PTR)user]++;
}
static void pump(void)
{
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        if (m.message != WM_QUIT) DispatchMessageW(&m);
    }
}
static fpe_watch *start(unsigned pane)
{
    int before = nwatches;
    require(folderwatch_set(pane, L"private-fixture", notify, (void *)(UINT_PTR)pane) == 0);
    require(nwatches == before + 1);
    return &watches[before];
}
static void fire(fpe_watch *w) { w->cb(w->user, w); }
static DWORD WINAPI fire_thread(void *arg) { fire(arg); return 0; }
static void hold(fpe_watch *w) { ResetEvent(w->release); }
static void cleanup(void)
{
    folderwatch_join(); pump();
}
static void test_queued_generation(void)
{
    puts("queued notifications: pane and generation ownership");
    fpe_watch *old = start(0), *other = start(1);
    fire(old); /* queued before navigation */
    fpe_watch *fresh = start(0);
    fire(other); fire(fresh); pump();
    VERIFY("a queued old-folder event does not refresh the new folder", notifications[0] == 1);
    VERIFY("the other pane retains its own notification", notifications[1] == 1);
    VERIFY("refresh callbacks run on the UI thread", !wrong_thread);
    UINT_PTR current = F.active[0]->generation;
    PostMessageW(F.sink, FOLDERWATCH_CHANGED, 2, (LPARAM)current);
    PostMessageW(F.sink, FOLDERWATCH_CHANGED, 0, (LPARAM)(current - 1)); pump();
    VERIFY("invalid panes and stale generations are ignored", notifications[0] == 1 && notifications[1] == 1);
    cleanup();
    VERIFY("all replaced and active watches are stopped exactly once", old->stops == 1 && fresh->stops == 1 && other->stops == 1);
}
static void test_navigation_while_stopping(void)
{
    puts("navigation: engine teardown held behind an event");
    fpe_watch *old = start(0); hold(old);
    uint64_t t = GetTickCount64(); fpe_watch *fresh = start(0);
    printf("  replacement returned in %llu ms\n", (unsigned long long)(GetTickCount64() - t));
    VERIFY("replacement returns before old teardown is released", WaitForSingleObject(old->release, 0) == WAIT_TIMEOUT);
    VERIFY("the cleanup worker reaches the held stop", WaitForSingleObject(old->entered, 5000) == WAIT_OBJECT_0);
    VERIFY("blocking engine stop runs off the UI thread", old->stop_thread != ui_thread);
    fire(old); fire(fresh); pump();
    VERIFY("the retired callback is suppressed while its context stays alive", notifications[0] == 2);
    fpe_watch *right = start(1); fire(right); pump();
    VERIFY("the other pane remains responsive while teardown is held", notifications[1] == 2);
    SetEvent(old->release); cleanup();
    VERIFY("held and replacement watchers are fully joined", old->stops == 1 && fresh->stops == 1 && right->stops == 1);
}
static void test_callback_in_flight(void)
{
    puts("callback: post already in flight when its watcher is retired");
    fpe_watch *old = start(0);
    ResetEvent(post_entered); ResetEvent(post_release); InterlockedExchange(&post_hold, 1);
    old->callback = CreateThread(NULL, 0, fire_thread, old, 0, NULL); require(old->callback != NULL);
    VERIFY("old callback reaches the posting boundary", WaitForSingleObject(post_entered, 5000) == WAIT_OBJECT_0);
    fpe_watch *fresh = start(0);
    VERIFY("navigation returns while the old callback is still held", WaitForSingleObject(post_release, 0) == WAIT_TIMEOUT);
    VERIFY("old stop waits for the in-flight callback off the UI thread", WaitForSingleObject(old->entered, 5000) == WAIT_OBJECT_0 && WaitForSingleObject(old->stopped, 0) == WAIT_TIMEOUT);
    SetEvent(post_release); require(WaitForSingleObject(old->callback, 5000) == WAIT_OBJECT_0);
    fire(fresh); pump();
    VERIFY("a late post cannot refresh the replacement", notifications[0] == 3);
    cleanup();
    VERIFY("the old callback context is joined before release", old->stops == 1 && fresh->stops == 1 && WaitForSingleObject(old->stopped, 0) == WAIT_OBJECT_0);
}
static LRESULT CALLBACK host_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) folderwatch_shutdown();
    return DefWindowProcW(h, m, w, l);
}
static void test_window_close_in_flight(void)
{
    puts("window close: retained sink and callback lifetime");
    WNDCLASSW wc = {0}; wc.lpfnWndProc = host_proc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"WatchTestHost";
    require(RegisterClassW(&wc) != 0);
    HWND host = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL); require(host != NULL);
    fpe_watch *left = start(0), *right = start(1); hold(left);
    ResetEvent(post_entered); ResetEvent(post_release); InterlockedExchange(&post_hold, 1);
    right->callback = CreateThread(NULL, 0, fire_thread, right, 0, NULL); require(right->callback != NULL);
    VERIFY("a callback is held before window close", WaitForSingleObject(post_entered, 5000) == WAIT_OBJECT_0);
    fire(left); /* also leave an already queued event */
    HWND sink = F.sink; DestroyWindow(host);
    VERIFY("the host closes before native teardown is released", !IsWindow(host) && WaitForSingleObject(left->release, 0) == WAIT_TIMEOUT);
    VERIFY("the message-only sink stays alive after the host closes", IsWindow(sink));
    VERIFY("the reaper holds native teardown off the UI thread", WaitForSingleObject(left->entered, 5000) == WAIT_OBJECT_0 && left->stop_thread != ui_thread);
    VERIFY("new watchers are rejected during shutdown", folderwatch_set(0, L"private", notify, NULL) == ERROR_SHUTDOWN_IN_PROGRESS);
    SetEvent(post_release); require(WaitForSingleObject(right->callback, 5000) == WAIT_OBJECT_0); pump();
    VERIFY("queued and in-flight events cannot call a closed pane", notifications[0] == 3 && notifications[1] == 2);
    folderwatch_shutdown(); SetEvent(left->release); cleanup();
    VERIFY("final join drains both watches exactly once", left->stops == 1 && right->stops == 1);
    VERIFY("the notification sink is destroyed only after callbacks finish", !IsWindow(sink) && !F.thread && !F.wake);
}
static void notify_reentrant(void *user)
{
    notifications[0]++;
    start(0); /* may free the current context before the callback returns */
}
static void test_start_failures_and_reentrancy(void)
{
    puts("startup failure, recovery, and reentrant navigation");
    VERIFY("invalid arguments do not initialize a worker", folderwatch_set(2, L"private", notify, NULL) == ERROR_INVALID_PARAMETER && !F.thread);
    DWORD before = 0, after = 0; GetProcessHandleCount(GetCurrentProcess(), &before);
    fail_thread = 1;
    VERIFY("worker creation failure is reported before a native watch starts", folderwatch_set(0, L"private", notify, NULL) == ERROR_NOT_ENOUGH_MEMORY);
    fail_thread = 0; GetProcessHandleCount(GetCurrentProcess(), &after);
    VERIFY("failed initialization releases the wake event and sink", before == after && !F.thread && !F.sink && !F.wake);
    fpe_watch *old = start(0); fire(old); fail_start = 1;
    VERIFY("native watch failure is reported after retiring the old folder", folderwatch_set(0, L"missing", notify, NULL) == ERROR_PATH_NOT_FOUND);
    fail_start = 0; pump();
    VERIFY("failed replacement cannot deliver the old queued event", notifications[0] == 3 && !F.active[0]);
    require(folderwatch_set(0, L"private", notify_reentrant, NULL) == 0);
    fpe_watch *reentrant = &watches[nwatches - 1]; fire(reentrant); pump();
    VERIFY("a notification may safely navigate to a new watcher", notifications[0] == 4 && F.active[0] && F.active[0]->watch != reentrant);
    cleanup();
    VERIFY("startup failure and reentrant replacement both finish cleanup", old->stops == 1 && reentrant->stops == 1);
}
static void test_many_retirements(void)
{
    puts("retirement queue: many replacements behind one held native stop");
    int first = nwatches;
    fpe_watch *old = start(0); hold(old);
    DWORD before = 0, after = 0; GetProcessHandleCount(GetCurrentProcess(), &before);
    int native_before = nwatches;
    for (int i = 0; i < 60; i++) start(i & 1);
    GetProcessHandleCount(GetCurrentProcess(), &after);
    VERIFY("replacements do not allocate a thread or event per retirement", after - before == (DWORD)(nwatches - native_before) * 3);
    VERIFY("a stalled old stop cannot block a burst of pane replacements", WaitForSingleObject(old->release, 0) == WAIT_TIMEOUT);
    SetEvent(old->release); cleanup();
    int once = 1, background = 1;
    for (int i = first; i < nwatches; i++) { once &= watches[i].stops == 1; background &= watches[i].stop_thread != ui_thread; }
    VERIFY("every queued watcher is stopped exactly once", once);
    VERIFY("every queued stop executes off the UI thread", background);
}
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    ui_thread = GetCurrentThreadId();
    post_entered = CreateEventW(NULL, TRUE, FALSE, NULL); post_release = CreateEventW(NULL, TRUE, FALSE, NULL);
    require(post_entered && post_release);
    test_queued_generation(); test_navigation_while_stopping(); test_callback_in_flight();
    test_window_close_in_flight(); test_start_failures_and_reentrancy(); test_many_retirements();
    for (int i = 0; i < nwatches; i++) {
        CloseHandle(watches[i].entered); CloseHandle(watches[i].release); CloseHandle(watches[i].stopped);
        if (watches[i].callback) CloseHandle(watches[i].callback);
    }
    CloseHandle(post_entered); CloseHandle(post_release);
    printf("\n%d passed, %d failed\n", passed, failed); return failed ? 1 : 0;
}
