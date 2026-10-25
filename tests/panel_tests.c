/* Native lifecycle regression: production panel code, real pipes/processes/jobs.
 * Only this harness intercepts process creation to hold it at deterministic
 * cancellation boundaries. The shipped host has no test hooks. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <wchar.h>
#include "../app/panelproto.h"

typedef struct attempt {
    HANDLE entered, release, returned, process;
    int mode; /* 0 normal, 1 hold before creation, 2 hold after creation, 3 fail */
} attempt;
static attempt attempts[32];
static int nattempts;
static PVOID volatile next_attempt;
typedef struct exit_info { DWORD pid; UINT_PTR hwnd; } exit_info;
static exit_info *exit_shared;
static HANDLE exit_ready, exit_release;

static BOOL WINAPI held_create(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta,
                              BOOL inherit, DWORD flags, LPVOID env, LPCWSTR dir, LPSTARTUPINFOW si,
                              LPPROCESS_INFORMATION pi)
{
    attempt *a = InterlockedExchangePointer(&next_attempt, NULL);
    if (a && a->mode == 1) {
        SetEvent(a->entered);
        if (WaitForSingleObject(a->release, 10000) != WAIT_OBJECT_0) { SetLastError(ERROR_TIMEOUT); return FALSE; }
    }
    if (a && a->mode == 3) {
        SetEvent(a->entered); SetEvent(a->returned);
        SetLastError(ERROR_FILE_NOT_FOUND); return FALSE;
    }
    wchar_t child_cmd[MAX_PATH + 64];
    _snwprintf(child_cmd, MAX_PATH + 64, L"\"%ls\" --panel-child", app);
    (void)cmd;
    BOOL ok = CreateProcessW(app, child_cmd, pa, ta, inherit, flags, env, dir, si, pi);
    if (ok && a) DuplicateHandle(GetCurrentProcess(), pi->hProcess, GetCurrentProcess(), &a->process,
                                  SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, 0);
    if (exit_shared) {
        exit_shared->pid = ok ? pi->dwProcessId : 0;
        SetEvent(exit_ready);
        WaitForSingleObject(exit_release, 10000);
    }
    if (a && a->mode == 2) { SetEvent(a->entered); WaitForSingleObject(a->release, 10000); }
    if (a) SetEvent(a->returned);
    return ok;
}
#define CreateProcessW held_create
#ifndef PANEL_SOURCE
#define PANEL_SOURCE "../app/sidepanel.c"
#endif
#include PANEL_SOURCE
#undef CreateProcessW

int app_px(int v) { return v; }
HFONT app_font(void) { return GetStockObject(DEFAULT_GUI_FONT); }
static int passed, failed;
#define VERIFY(label, cond) do { if (cond) { passed++; printf("  ok    %s\n", label); } \
    else { failed++; printf("  FAIL  %s (line %d)\n", label, __LINE__); } fflush(stdout); } while (0)
static wchar_t self[MAX_PATH];
static HWND host;

static void pump(DWORD ms)
{
    uint64_t until = GetTickCount64() + ms;
    do {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message != WM_QUIT) { TranslateMessage(&m); DispatchMessageW(&m); }
        }
        Sleep(1);
    } while (GetTickCount64() < until);
}
static attempt *prepare(int mode)
{
    attempt *a = &attempts[nattempts++];
    a->mode = mode;
    a->entered = CreateEventW(NULL, TRUE, FALSE, NULL);
    a->release = CreateEventW(NULL, TRUE, FALSE, NULL);
    a->returned = CreateEventW(NULL, TRUE, FALSE, NULL);
    InterlockedExchangePointer(&next_attempt, a);
    return a;
}
static int wait_state(int state)
{
    uint64_t until = GetTickCount64() + 5000;
    while (GetTickCount64() < until) {
        papp *a = by_path(self);
        if (a && a->state == state) return 1;
        pump(5);
    }
    return 0;
}
static DWORD WINAPI release_later(void *arg)
{
    Sleep(1500); SetEvent((HANDLE)arg); return 0;
}
static HANDLE fallback(attempt *a)
{
    return CreateThread(NULL, 0, release_later, a->release, 0, NULL);
}
static void finish_fallback(HANDLE t)
{
    if (t) { WaitForSingleObject(t, 2000); CloseHandle(t); }
}
static int child_ended(attempt *a)
{
    return WaitForSingleObject(a->returned, 5000) == WAIT_OBJECT_0 && a->process &&
           WaitForSingleObject(a->process, 5000) == WAIT_OBJECT_0;
}
static void test_pending_close_and_reopen(void)
{
    puts("pending launch: close, reuse, and stale completion");
    attempt *old = prepare(1);
    VERIFY("panel starts with process creation held", sidepanel_show(host, self, 0, 0) == 0);
    VERIFY("worker reaches the held process-creation boundary", WaitForSingleObject(old->entered, 5000) == WAIT_OBJECT_0);
    uint32_t old_gen = P.cur ? P.cur->gen : 0;
    HANDLE rescue = fallback(old);
    uint64_t t = GetTickCount64();
    sidepanel_close();
    printf("  close returned in %llu ms\n", (unsigned long long)(GetTickCount64() - t));
    VERIFY("close returns while creation remains held", WaitForSingleObject(old->release, 0) == WAIT_TIMEOUT);
    VERIFY("closing releases the nonpersistent app record", by_path(self) == NULL);
    attempt *fresh = prepare(0);
    VERIFY("same helper can reopen before old creation finishes", sidepanel_show(host, self, 0, 0) == 0);
    VERIFY("reopened panel receives READY", wait_state(A_RUNNING));
    papp *a = by_path(self);
    DWORD fresh_pid = a ? a->pid : 0;
    VERIFY("reopened record has a fresh generation and process", a && a->gen != old_gen && fresh_pid != 0);
    SetEvent(old->release); finish_fallback(rescue);
    VERIFY("cancelled late child is reaped", child_ended(old));
    pump(100);
    a = by_path(self);
    VERIFY("stale completion cannot replace the reopened helper", a && a->pid == fresh_pid && a->state == A_RUNNING);
    VERIFY("new helper survives the old launch cleanup", fresh->process && WaitForSingleObject(fresh->process, 0) == WAIT_TIMEOUT);
    sidepanel_close();
    VERIFY("normally running helper quits on close", child_ended(fresh));
}
static void test_pending_shutdown(void)
{
    puts("pending launch: shutdown after a suspended child exists");
    attempt *a = prepare(2);
    VERIFY("shutdown fixture starts", sidepanel_show(host, self, 0, 0) == 0);
    VERIFY("worker holds a real suspended child", WaitForSingleObject(a->entered, 5000) == WAIT_OBJECT_0 && a->process);
    BOOL in_job = FALSE;
    VERIFY("suspended child belongs to a job before launch returns", a->process && IsProcessInJob(a->process, NULL, &in_job) && in_job);
    HANDLE rescue = fallback(a);
    sidepanel_shutdown();
    VERIFY("shutdown returns while creation remains held", WaitForSingleObject(a->release, 0) == WAIT_TIMEOUT);
    VERIFY("shutdown releases the active lifecycle", !sidepanel_running(self));
    SetEvent(a->release); finish_fallback(rescue);
    VERIFY("cancelled suspended child exits", child_ended(a));
    app_forget(by_path(self));
}
static void test_delayed_exit_status(void)
{
    puts("pipe EOF: delayed process exit status is collected without waiting");
    attempt *a = prepare(0);
    VERIFY("exit-status fixture becomes ready", sidepanel_show(host, self, 0, 0) == 0 && wait_state(A_RUNNING));
    VERIFY("exit-status fixture accepts its private EOF request", sidepanel_send(self, 0x7ffffffeu, NULL, 0));
    VERIFY("pipe closure marks the app Gone", wait_state(A_GONE));
    uint64_t until = GetTickCount64() + 5000;
    papp *record = by_path(self);
    while (record && !record->exit_code && GetTickCount64() < until) pump(10);
    VERIFY("Gone eventually reports the real delayed exit code", record && record->exit_code == 42);
    sidepanel_close();
    VERIFY("delayed-exit fixture leaves no child behind", child_ended(a));
}
static void test_failed_launch_cleanup(void)
{
    puts("launch failure: EOF before completion and a subsequent restart");
    DWORD before = 0, after = 0;
    pump(50); GetProcessHandleCount(GetCurrentProcess(), &before);
    for (int i = 0; i < 4; i++) {
        attempt *a = prepare(3);
        VERIFY("failure fixture requests a launch", sidepanel_show(host, self, 0, 0) == 0);
        VERIFY("creation failure reaches Gone without blocking", wait_state(A_GONE));
        VERIFY("failed launch has no running child", !sidepanel_running(self) && !a->process);
        sidepanel_close();
        CloseHandle(a->entered); a->entered = NULL;
        CloseHandle(a->release); a->release = NULL;
        CloseHandle(a->returned); a->returned = NULL;
    }
    pump(100); GetProcessHandleCount(GetCurrentProcess(), &after);
    VERIFY("repeated failure retains no pipe, process, job or worker handles", after <= before);
    attempt *a = prepare(0);
    VERIFY("panel can start after repeated failures", sidepanel_show(host, self, 0, 0) == 0 && wait_state(A_RUNNING));
    sidepanel_close();
    VERIFY("restarted helper exits cleanly", child_ended(a));
}
static LRESULT CALLBACK host_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE) { sidepanel_shutdown(); DestroyWindow(h); PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}
static int init_host(void)
{
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = host_proc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"OpenPilotPanelTestHost";
    RegisterClassW(&wc);
    host = CreateWindowExW(0, wc.lpszClassName, L"Owned OpenPilot panel lifecycle fixture", WS_OVERLAPPEDWINDOW,
                           0, 0, 800, 600, NULL, NULL, wc.hInstance, NULL);
    return host != NULL;
}
static int exit_host(const wchar_t *name)
{
    wchar_t part[160];
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    exit_shared = map ? MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof *exit_shared) : NULL;
    _snwprintf(part, 160, L"%ls-ready", name); exit_ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, part);
    _snwprintf(part, 160, L"%ls-release", name); exit_release = OpenEventW(SYNCHRONIZE, FALSE, part);
    if (!exit_shared || !exit_ready || !exit_release || !init_host()) return 2;
    exit_shared->hwnd = (UINT_PTR)host;
    if (sidepanel_show(host, self, 0, 0)) return 3;
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}
static void test_host_exit_containment(void)
{
    puts("host exit: a creation call held after the child exists");
    wchar_t name[128], part[160], cmd[MAX_PATH + 200];
    _snwprintf(name, 128, L"Local\\OpenPilotPanelExit-%lu-%llu", GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(exit_info), name);
    exit_info *info = map ? MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof *info) : NULL;
    _snwprintf(part, 160, L"%ls-ready", name); HANDLE ready = CreateEventW(NULL, TRUE, FALSE, part);
    _snwprintf(part, 160, L"%ls-release", name); HANDLE release = CreateEventW(NULL, TRUE, FALSE, part);
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi = { 0 };
    _snwprintf(cmd, MAX_PATH + 200, L"\"%ls\" --exit-host %ls", self, name);
    BOOL created = info && ready && release && CreateProcessW(self, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    VERIFY("owned host fixture starts", created);
    HANDLE child = NULL;
    if (created) {
        VERIFY("fixture holds process creation after child birth", WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0 && info->pid);
        child = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, info->pid);
        VERIFY("observer owns a handle to the still suspended child", child && WaitForSingleObject(child, 0) == WAIT_TIMEOUT);
        PostMessageW((HWND)info->hwnd, WM_CLOSE, 0, 0);
        VERIFY("host exits without releasing the stalled creation call", WaitForSingleObject(pi.hProcess, 1500) == WAIT_OBJECT_0);
        VERIFY("host exit terminates its suspended child", child && WaitForSingleObject(child, 1500) == WAIT_OBJECT_0);
        SetEvent(release);
        if (WaitForSingleObject(pi.hProcess, 5000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 9);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    if (child) {
        if (WaitForSingleObject(child, 2000) != WAIT_OBJECT_0) TerminateProcess(child, 9);
        CloseHandle(child);
    }
    if (info) UnmapViewOfFile(info);
    if (map) CloseHandle(map);
    if (ready) CloseHandle(ready);
    if (release) CloseHandle(release);
}
static int panel_child(void)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
    for (;;) {
        panelp_head h;
        if (read_all(in, &h, sizeof h) || h.size > PANELP_MAX) return 0;
        void *data = h.size ? malloc(h.size) : NULL;
        if (h.size && (!data || read_all(in, data, h.size))) { free(data); return 1; }
        free(data);
        if (h.type == PANELP_QUIT) return 0;
        if (h.type == 0x7ffffffeu) { CloseHandle(out); Sleep(300); return 42; }
        if (h.type == PANELP_HELLO) {
            panelp_ready r = { 0 }; wcscpy(r.title, L"Lifecycle fixture");
            panelp_head reply = { PANELP_READY, sizeof r }; DWORD written;
            WriteFile(out, &reply, sizeof reply, &written, NULL); WriteFile(out, &r, sizeof r, &written, NULL);
        }
        if (h.type == PANELP_PING) { /* fixtures finish well before hung classification matters */ }
    }
}
int wmain(int argc, wchar_t **argv)
{
    GetModuleFileNameW(NULL, self, MAX_PATH);
    if (argc > 1 && !wcscmp(argv[1], L"--panel-child")) return panel_child();
    if (argc > 2 && !wcscmp(argv[1], L"--exit-host")) return exit_host(argv[2]);
    VERIFY("native panel host fixture is created", init_host());
    if (host) {
        test_pending_close_and_reopen();
        test_pending_shutdown();
        test_failed_launch_cleanup();
        test_delayed_exit_status();
        test_host_exit_containment();
        sidepanel_shutdown(); pump(100); DestroyWindow(host);
    }
    for (int i = 0; i < nattempts; i++) {
        attempt *a = &attempts[i];
        if (a->release) SetEvent(a->release);
        if (a->process) {
            if (WaitForSingleObject(a->process, 5000) != WAIT_OBJECT_0) TerminateProcess(a->process, 9);
            CloseHandle(a->process);
        }
        if (a->entered) CloseHandle(a->entered);
        if (a->release) CloseHandle(a->release);
        if (a->returned) CloseHandle(a->returned);
    }
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
