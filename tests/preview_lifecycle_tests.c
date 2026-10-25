/* Real Windows jobs/pipes with only process creation held by this harness. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../app/pipewriter.h"
#include "../app/previewreader.h"

static HANDLE launch_entered, launch_release, first_child;
static volatile LONG creates, resumes, sid_calls, integrity_calls, fail_sid, fail_integrity;

static BOOL WINAPI injected_sid(PSID_IDENTIFIER_AUTHORITY authority, BYTE count, DWORD a, DWORD b,
                                DWORD c, DWORD d, DWORD e, DWORD f, DWORD g, DWORD h, PSID *sid)
{
    InterlockedIncrement(&sid_calls);
    if (InterlockedCompareExchange(&fail_sid, 0, 0)) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    return AllocateAndInitializeSid(authority, count, a, b, c, d, e, f, g, h, sid);
}

static BOOL WINAPI injected_integrity(HANDLE token, TOKEN_INFORMATION_CLASS info_class,
                                      LPVOID info, DWORD length)
{
    if (info_class == TokenIntegrityLevel) {
        InterlockedIncrement(&integrity_calls);
        if (InterlockedCompareExchange(&fail_integrity, 0, 0)) {
            SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
    }
    return SetTokenInformation(token, info_class, info, length);
}

static DWORD WINAPI counted_resume(HANDLE thread)
{
    InterlockedIncrement(&resumes);
    return ResumeThread(thread);
}
static BOOL WINAPI held_create(HANDLE token, LPCWSTR app, LPWSTR command, LPSECURITY_ATTRIBUTES process_sa,
                               LPSECURITY_ATTRIBUTES thread_sa, BOOL inherit, DWORD flags, LPVOID env,
                               LPCWSTR dir, LPSTARTUPINFOW startup, LPPROCESS_INFORMATION result)
{
    LONG call = InterlockedIncrement(&creates);
    if (call == 1) {
        SetEvent(launch_entered);
        if (WaitForSingleObject(launch_release, 10000) != WAIT_OBJECT_0) { SetLastError(ERROR_TIMEOUT); return FALSE; }
    }
    BOOL ok = CreateProcessAsUserW(token, app, command, process_sa, thread_sa, inherit, flags, env, dir,
                                   startup, result);
    if (ok && call == 1)
        DuplicateHandle(GetCurrentProcess(), result->hProcess, GetCurrentProcess(), &first_child,
                        0, FALSE, DUPLICATE_SAME_ACCESS);
    return ok;
}
#define AllocateAndInitializeSid injected_sid
#define SetTokenInformation injected_integrity
#define CreateProcessAsUserW held_create
#define ResumeThread counted_resume
#include "../app/previewlaunch.c"
#undef ResumeThread
#undef CreateProcessAsUserW
#undef SetTokenInformation
#undef AllocateAndInitializeSid

static int passed, failed;
#define CHECK(label, condition) do { \
    if (condition) { passed++; printf("  ok    %s\n", label); } \
    else { failed++; printf("  FAIL  %s\n", label); } \
} while (0)

typedef struct fixture {
    HANDLE child_in, to, from, child_out, section;
} fixture;

static int fixture_make(fixture *f)
{
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    memset(f, 0, sizeof *f);
    f->section = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, 4096, NULL);
    return f->section && CreatePipe(&f->child_in, &f->to, &sa, 4096) &&
           CreatePipe(&f->from, &f->child_out, &sa, 4096);
}

static void fixture_close(fixture *f)
{
    HANDLE *handles[] = {&f->child_in, &f->to, &f->from, &f->child_out, &f->section};
    for (size_t i = 0; i < sizeof handles / sizeof *handles; i++)
        if (*handles[i]) { CloseHandle(*handles[i]); *handles[i] = NULL; }
}

static DWORD process_integrity_rid(HANDLE process)
{
    HANDLE token = NULL;
    DWORD length = 0, rid = 0;
    TOKEN_MANDATORY_LABEL *label = NULL;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return 0;
    GetTokenInformation(token, TokenIntegrityLevel, NULL, 0, &length);
    label = malloc(length);
    if (label && GetTokenInformation(token, TokenIntegrityLevel, label, length, &length) &&
        IsValidSid(label->Label.Sid) && *GetSidSubAuthorityCount(label->Label.Sid)) {
        BYTE last = *GetSidSubAuthorityCount(label->Label.Sid) - 1;
        rid = *GetSidSubAuthority(label->Label.Sid, last);
    }
    free(label);
    CloseHandle(token);
    return rid;
}

static void test_held_launch(void)
{
    puts("preview launch: cancel held creation, interact, reopen, reap both jobs");
    fixture a = {0}, b = {0};
    preview_launch *old = NULL, *newer = NULL;
    HANDLE proc = NULL, job = NULL;
    DWORD pid = 0;
    launch_entered = CreateEventW(NULL, TRUE, FALSE, NULL);
    launch_release = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK("held-launch fixtures allocate", launch_entered && launch_release && fixture_make(&a) && fixture_make(&b));
    if (!launch_entered || !launch_release || !a.section || !b.section) goto done;
    old = preview_launch_begin(L"preview_lifecycle_tests.exe", a.child_in, a.child_out, a.section,
                               (SIZE_T)32 << 20, NULL, WM_APP + 1, 1);
    CHECK("first helper launch reaches held CreateProcess", old &&
          WaitForSingleObject(launch_entered, 5000) == WAIT_OBJECT_0);
    ULONGLONG start = GetTickCount64();
    preview_launch_cancel(&old);
    CHECK("cancel returns while process creation is still held", GetTickCount64() - start < 300 && !old);
    newer = preview_launch_begin(L"preview_lifecycle_tests.exe", b.child_in, b.child_out, b.section,
                                 (SIZE_T)32 << 20, NULL, WM_APP + 1, 2);
    int adopted = 0;
    ULONGLONG until = GetTickCount64() + 5000;
    while (newer && !adopted && GetTickCount64() < until) {
        adopted = preview_launch_take(&newer, &proc, &job, &pid);
        if (!adopted) Sleep(5);
    }
    CHECK("a new helper starts before the old creation is released", adopted == 1 && proc && job && pid);
    if (proc) CHECK("resumed helper actually has low integrity", process_integrity_rid(proc) == SECURITY_MANDATORY_LOW_RID);
    if (job) TerminateJobObject(job, 1);
    if (proc) { CHECK("reopened helper exits with its job", WaitForSingleObject(proc, 2000) == WAIT_OBJECT_0); CloseHandle(proc); }
    if (job) CloseHandle(job);
    SetEvent(launch_release);
    until = GetTickCount64() + 5000;
    while (!first_child && GetTickCount64() < until) Sleep(5);
    CHECK("cancelled late helper is reaped without being resumed", first_child &&
          WaitForSingleObject(first_child, 3000) == WAIT_OBJECT_0);
done:
    if (launch_release) SetEvent(launch_release);
    preview_launch_cancel(&old); preview_launch_cancel(&newer);
    if (first_child) { CloseHandle(first_child); first_child = NULL; }
    fixture_close(&a); fixture_close(&b);
    if (launch_entered) CloseHandle(launch_entered);
    if (launch_release) CloseHandle(launch_release);
}

static void test_token_failure(const char *name, volatile LONG *fault, int expect_sid_call,
                               int expect_integrity_call)
{
    fixture f = {0};
    preview_launch *launch = NULL;
    HANDLE proc = NULL, job = NULL;
    DWORD pid = 0;
    LONG before_creates = InterlockedCompareExchange(&creates, 0, 0);
    LONG before_resumes = InterlockedCompareExchange(&resumes, 0, 0);
    LONG before_sids = InterlockedCompareExchange(&sid_calls, 0, 0);
    LONG before_integrity = InterlockedCompareExchange(&integrity_calls, 0, 0);
    CHECK("token-failure fixture allocates", fixture_make(&f));
    if (!f.section || !f.child_in || !f.to || !f.from || !f.child_out) goto done;
    InterlockedExchange(fault, 1);
    launch = preview_launch_begin(L"preview_lifecycle_tests.exe", f.child_in, f.child_out, f.section,
                                  (SIZE_T)32 << 20, NULL, WM_APP + 1, 3);
    int result = 0;
    ULONGLONG until = GetTickCount64() + 5000;
    while (launch && !result && GetTickCount64() < until) {
        result = preview_launch_take(&launch, &proc, &job, &pid);
        if (!result) Sleep(5);
    }
    CHECK(name, result == -1 && !proc && !job && !pid &&
          InterlockedCompareExchange(&creates, 0, 0) == before_creates &&
          InterlockedCompareExchange(&resumes, 0, 0) == before_resumes &&
          InterlockedCompareExchange(&sid_calls, 0, 0) == before_sids + expect_sid_call &&
          InterlockedCompareExchange(&integrity_calls, 0, 0) == before_integrity + expect_integrity_call);
done:
    InterlockedExchange(fault, 0);
    preview_launch_cancel(&launch);
    if (proc) CloseHandle(proc);
    if (job) CloseHandle(job);
    fixture_close(&f);
}

static void test_held_writer(void)
{
    puts("preview writer: bounded queue and responsive cancellation with a stopped reader");
    HANDLE read_end = NULL, write_end = NULL, next_read = NULL, next_write = NULL;
    CHECK("held-reader fixture allocates", CreatePipe(&read_end, &write_end, NULL, 4096));
    if (!read_end || !write_end) goto done;
    pipe_writer *writer = pipe_writer_begin(write_end, 1 << 20, NULL, WM_APP + 2, 1);
    char *large = malloc(1 << 20);
    if (large) memset(large, 'x', 1 << 20);
    CHECK("a large request queues without waiting for the reader", writer && large &&
          !pipe_writer_send(writer, large, 1 << 20, 0, 0));
    CHECK("backpressure refuses a request over the queue limit", writer && large &&
          pipe_writer_send(writer, large, (1 << 20) + 1, 0, 0));
    ULONGLONG start = GetTickCount64();
    pipe_writer_cancel(&writer);
    CHECK("stopping a blocked writer returns promptly", GetTickCount64() - start < 300 && !writer);
    CHECK("a fresh writer can publish while the old reader is still held",
          CreatePipe(&next_read, &next_write, NULL, 4096));
    if (next_read && next_write) {
        pipe_writer *fresh = pipe_writer_begin(next_write, 4096, NULL, WM_APP + 2, 2);
        const char word[] = "reopened";
        char got[sizeof word] = {0}; DWORD n = 0;
        int sent = fresh && !pipe_writer_send(fresh, word, sizeof word, 0, 0);
        int read = sent && ReadFile(next_read, got, sizeof got, &n, NULL);
        CHECK("reopened writer delivers exactly its own request", read && n == sizeof word && !memcmp(got, word, n));
        pipe_writer_cancel(&fresh);
    }
    free(large);
done:
    if (read_end) CloseHandle(read_end);
    if (write_end) CloseHandle(write_end);
    if (next_read) CloseHandle(next_read);
    if (next_write) CloseHandle(next_write);
}

int wmain(int argc, wchar_t **argv)
{
    if (argc > 1) {
        char buf[128]; DWORD n = 0;
        while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), buf, sizeof buf, &n, NULL) && n) {}
        return 0;
    }
    DWORD before = 0, after = 0;
    test_held_launch();
    GetProcessHandleCount(GetCurrentProcess(), &before); /* Win32 caches three handles on first process creation */
    InterlockedExchange(&creates, 0);
    test_held_launch();
    test_held_writer();
    test_token_failure("SID allocation failure creates and resumes no helper", &fail_sid, 1, 0);
    test_token_failure("integrity update failure creates and resumes no helper", &fail_integrity, 1, 1);
    ULONGLONG until = GetTickCount64() + 3000;
    do { GetProcessHandleCount(GetCurrentProcess(), &after); if (after <= before) break; Sleep(10); }
    while (GetTickCount64() < until);
    CHECK("cancelled launches and writes release their owned handles", after <= before);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
