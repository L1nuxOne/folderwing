/* Regression tests for the standing engine harness's callback lifetime barrier.
 * Wait/cancel interception exists only in this executable. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "fpe.h"

static int passed, failed, real_jobs;
static DWORD answers[2], timeouts[2];
static unsigned waits, cancels;
static uint64_t cancelled_id;
static fpe_engine *cancelled_engine;
static HANDLE observed_event, release_blocker;
static char order[8];
static unsigned order_len;

#define CHECK(c) do { if (c) passed++; else { failed++; printf("FAIL %d: %s\n", __LINE__, #c); } } while (0)

static DWORD WINAPI fixture_wait(HANDLE event, DWORD timeout)
{
    if (waits >= 2) ExitProcess(10);
    order[order_len++] = 'W';
    timeouts[waits] = timeout;
    observed_event = event;
    DWORD flags;
    CHECK(GetHandleInformation(event, &flags));
    unsigned n = waits++;
    return real_jobs ? WaitForSingleObject(event, timeout) : answers[n];
}

static void fixture_cancel(fpe_engine *engine, uint64_t id)
{
    order[order_len++] = 'C';
    cancels++;
    cancelled_id = id;
    cancelled_engine = engine;
    if (real_jobs) {
        fpe_job_cancel(engine, id);
        /* The first real wait timed out while the worker was blocked. Release
         * it only AFTER cancellation, without timing sleeps or polling. */
        SetEvent(release_blocker);
    }
}

#define WaitForSingleObject fixture_wait
#define fpe_job_cancel fixture_cancel
#include "jobwait.h"
#undef fpe_job_cancel
#undef WaitForSingleObject

static void reset(DWORD initial, DWORD cleanup)
{
    answers[0] = initial; answers[1] = cleanup;
    waits = cancels = order_len = 0;
    memset(order, 0, sizeof order);
    cancelled_id = 0; cancelled_engine = NULL; observed_event = NULL;
    real_jobs = 0;
}

static void test_completed_job_needs_no_cleanup(void)
{
    reset(WAIT_OBJECT_0, WAIT_FAILED);
    HANDLE ev = test_callback_event(FALSE);
    CHECK(test_wait_job(NULL, 41, ev, 1234) == WAIT_OBJECT_0);
    CHECK(waits == 1 && !cancels && timeouts[0] == 1234);
    CHECK(observed_event == ev && !strcmp(order, "W"));
    CloseHandle(ev);
}

static void test_failed_submission_never_cancels_all_jobs(void)
{
    reset(WAIT_TIMEOUT, WAIT_OBJECT_0);
    HANDLE ev = test_callback_event(FALSE);
    CHECK(test_wait_job(NULL, 0, ev, 30000) == WAIT_FAILED);
    CHECK(!waits && !cancels);
    CloseHandle(ev);
}

static void test_timeout_drains_exact_job_and_stays_failed(void)
{
    reset(WAIT_TIMEOUT, WAIT_OBJECT_0);
    HANDLE ev = test_callback_event(FALSE);
    fpe_engine *engine = (fpe_engine *)&waits;
    CHECK(test_wait_job(engine, 123, ev, 17) == WAIT_TIMEOUT);
    CHECK(cancels == 1 && cancelled_id == 123 && cancelled_engine == engine);
    CHECK(waits == 2 && timeouts[0] == 17 && timeouts[1] == 30000);
    CHECK(observed_event == ev && !strcmp(order, "WCW"));
    DWORD flags; CHECK(GetHandleInformation(ev, &flags));
    CloseHandle(ev);
}

static void test_wait_failure_also_drains(void)
{
    reset(WAIT_FAILED, WAIT_OBJECT_0);
    HANDLE ev = test_callback_event(FALSE);
    CHECK(test_wait_job(NULL, 777, ev, 5000) == WAIT_FAILED);
    CHECK(cancels == 1 && cancelled_id == 777 && waits == 2);
    CHECK(!strcmp(order, "WCW"));
    CloseHandle(ev);
}

static void child_must_abort(const wchar_t *mode)
{
    wchar_t exe[32768], cmd[32768];
    DWORD n = GetModuleFileNameW(NULL, exe, 32768);
    CHECK(n > 0 && n < 32768);
    if (!n || n >= 32768) return;
    _snwprintf(cmd, 32768, L"\"%ls\" %ls", exe, mode);cmd[32767] = 0;
    STARTUPINFOW si = {sizeof si}; PROCESS_INFORMATION pi = {0};
    BOOL created = CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CHECK(created);
    if (!created) return;
    DWORD wait = WaitForSingleObject(pi.hProcess, 5000), code = 0;
    CHECK(wait == WAIT_OBJECT_0);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 11);
        WaitForSingleObject(pi.hProcess, INFINITE);
    }
    CHECK(GetExitCodeProcess(pi.hProcess, &code) && code == 9);
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
}

static void test_undrainable_job_aborts_process(void)
{
    child_must_abort(L"--drain-timeout");
    child_must_abort(L"--drain-failed");
}

static void test_incomplete_load_aborts_process(void)
{
    child_must_abort(L"--load-timeout");
    child_must_abort(L"--load-failed");
}

typedef struct result {
    HANDLE done, entered, release;
    fpe_job_progress last;
    int finals;
} result;

static void progress(void *user, const fpe_job_progress *p)
{
    result *r = user;
    if (p->state == FPE_JOB_RUNNING && r->entered) {
        SetEvent(r->entered);
        if (WaitForSingleObject(r->release, 30000) != WAIT_OBJECT_0) ExitProcess(12);
    }
    if (p->state == FPE_JOB_DONE || p->state == FPE_JOB_CANCELLED || p->state == FPE_JOB_FAILED) {
        r->last = *p;r->last.current = NULL;r->finals++;
        SetEvent(r->done); /* Last access to callback state. */
    }
}

static void test_real_queue_timeout_preserves_callback_lifetime(void)
{
    reset(WAIT_TIMEOUT, WAIT_OBJECT_0);
    wchar_t tmp[MAX_PATH], root[MAX_PATH], source[MAX_PATH], dest[MAX_PATH], output[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    _snwprintf(root, MAX_PATH, L"%lsopenpilot_jobwait_%lu", tmp, GetCurrentProcessId());
    _snwprintf(source, MAX_PATH, L"%ls\\source", root);
    _snwprintf(dest, MAX_PATH, L"%ls\\dest", root);
    _snwprintf(output, MAX_PATH, L"%ls\\source", dest);
    CHECK(CreateDirectoryW(root, NULL));CHECK(CreateDirectoryW(dest, NULL));
    HANDLE file = CreateFileW(source, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(file != INVALID_HANDLE_VALUE);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    fpe_engine *engine = fpe_engine_create(NULL);CHECK(engine != NULL);
    result first = {.done=test_callback_event(FALSE), .entered=test_callback_event(TRUE), .release=test_callback_event(TRUE)};
    result queued = {.done=test_callback_event(FALSE)}, unrelated = {.done=test_callback_event(FALSE)};
    const wchar_t *paths[] = {source};
    uint64_t a = fpe_job_submit(engine, FPE_OP_COPY, paths, 1, dest, FPE_CONFLICT_FAIL, progress, &first);
    CHECK(a && WaitForSingleObject(first.entered, 30000) == WAIT_OBJECT_0);
    if (!a || WaitForSingleObject(first.entered, 0) != WAIT_OBJECT_0) test_wait_abort("regression blocker failed", WAIT_FAILED);
    uint64_t b = fpe_job_submit(engine, FPE_OP_COPY, paths, 1, dest, FPE_CONFLICT_RENAME, progress, &queued);
    /* An unrelated missing source should report a normal error, not cancel. */
    wchar_t missing_path[MAX_PATH];_snwprintf(missing_path, MAX_PATH, L"%ls\\missing", root);
    const wchar_t *missing[] = {missing_path};
    uint64_t c = fpe_job_submit(engine, FPE_OP_COPY, missing, 1, dest, FPE_CONFLICT_FAIL, progress, &unrelated);
    CHECK(b != 0 && c != 0);
    if (!b || !c) test_wait_abort("regression submission failed", WAIT_FAILED);
    real_jobs = 1;release_blocker = first.release;
    CHECK(test_wait_job(engine, b, queued.done, 0) == WAIT_TIMEOUT);
    real_jobs = 0;
    CHECK(waits == 2 && cancels == 1 && cancelled_id == b && cancelled_engine == engine);
    CHECK(queued.finals == 1 && queued.last.state == FPE_JOB_CANCELLED);
    /* Engine shutdown itself cancels queued jobs. Observe the unrelated job
     * first so teardown cannot masquerade as cancellation by our helper. */
    DWORD unrelated_wait = WaitForSingleObject(unrelated.done, 30000);
    CHECK(unrelated_wait == WAIT_OBJECT_0);
    if (unrelated_wait != WAIT_OBJECT_0) test_wait_abort("unrelated regression job stalled", unrelated_wait);
    fpe_engine_destroy(engine);
    CHECK(first.finals == 1 && first.last.state == FPE_JOB_DONE && !first.last.errors);
    CHECK(unrelated.finals == 1 && unrelated.last.state != FPE_JOB_CANCELLED && unrelated.last.errors);
    CHECK(GetFileAttributesW(source) != INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesW(output) != INVALID_FILE_ATTRIBUTES);
    wchar_t renamed[MAX_PATH];_snwprintf(renamed, MAX_PATH, L"%ls\\source (2)", dest);
    CHECK(GetFileAttributesW(renamed) == INVALID_FILE_ATTRIBUTES);
    DWORD flags;CHECK(GetHandleInformation(queued.done, &flags));
    CloseHandle(first.done);CloseHandle(first.entered);CloseHandle(first.release);
    CloseHandle(queued.done);CloseHandle(unrelated.done);
    CHECK(DeleteFileW(output));CHECK(DeleteFileW(source));
    CHECK(RemoveDirectoryW(dest));CHECK(RemoveDirectoryW(root));
}

int wmain(int argc, wchar_t **argv)
{
    if (argc == 2) {
        int load = !wcsncmp(argv[1], L"--load-", 7);
        int error = wcsstr(argv[1], L"failed") != NULL;
        reset(load ? (error ? WAIT_FAILED : WAIT_TIMEOUT) : WAIT_TIMEOUT,
              error ? WAIT_FAILED : WAIT_TIMEOUT);
        HANDLE ev = test_callback_event(FALSE);
        if (load) test_wait_load(ev, 5000);
        else test_wait_job(NULL, 99, ev, 30000);
        return 13; /* Returning after an undrained callback is always a failure. */
    }
#define RUN(name) do { int before=failed; ULONGLONG t=GetTickCount64(); name(); printf("CASE %s %s %llu\n", #name, failed==before?"passed":"failed", GetTickCount64()-t); } while (0)
    RUN(test_completed_job_needs_no_cleanup);
    RUN(test_failed_submission_never_cancels_all_jobs);
    RUN(test_timeout_drains_exact_job_and_stays_failed);
    RUN(test_wait_failure_also_drains);
    RUN(test_undrainable_job_aborts_process);
    RUN(test_incomplete_load_aborts_process);
    RUN(test_real_queue_timeout_preserves_callback_lifetime);
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
