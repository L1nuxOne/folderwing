/* Test-only lifetime barriers for stack-backed asynchronous callback state.
 * Include after windows.h and fpe.h. Never link this into product code. */
#ifndef OPENPILOT_TEST_JOBWAIT_H
#define OPENPILOT_TEST_JOBWAIT_H

#include <stdio.h>

static void test_wait_abort(const char *reason, DWORD wait)
{
    fprintf(stderr, "FATAL test callback cleanup: %s (wait=%lu)\n", reason, (unsigned long)wait);
    fflush(NULL);
    /* Do not unwind a callback's stack storage or run teardown against a live
     * worker. Terminate only this test process, with a non-passing exit code. */
    TerminateProcess(GetCurrentProcess(), 9);
    ExitProcess(9);
}

static HANDLE test_callback_event(BOOL manual_reset)
{
    HANDLE event = CreateEventW(NULL, manual_reset, FALSE, NULL);
    if (!event) test_wait_abort("cannot create completion event", WAIT_FAILED);
    return event;
}

static DWORD test_wait_job(fpe_engine *engine, uint64_t id, HANDLE completion, DWORD timeout)
{
    /* Zero means submission failed, but cancel(0) would cancel every job. */
    if (!id) return WAIT_FAILED;
    DWORD wait = WaitForSingleObject(completion, timeout);
    if (wait == WAIT_OBJECT_0) return wait;
    fprintf(stderr, "Test job %llu missed completion (wait=%lu); cancelling and draining\n",
            (unsigned long long)id, (unsigned long)wait);
    fpe_job_cancel(engine, id);
    DWORD drained = WaitForSingleObject(completion, 30000);
    if (drained != WAIT_OBJECT_0) test_wait_abort("job did not drain", drained);
    /* The terminal callback's SetEvent must be its last access to user state.
     * Cleanup is not another chance to pass the original assertion deadline. */
    return wait;
}

static DWORD test_wait_load(HANDLE completion, DWORD timeout)
{
    DWORD wait = WaitForSingleObject(completion, timeout);
    /* Loads have no per-ticket cancellation/drain API. Never return their
     * stack-backed user state while a callback could still arrive. */
    if (wait != WAIT_OBJECT_0) test_wait_abort("load did not complete", wait);
    return wait;
}

#endif
