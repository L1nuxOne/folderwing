/* Cancellable, generation-tagged creation for PDF/image/text helpers. */
#define WIN32_LEAN_AND_MEAN
#include "previewlaunch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct preview_launch {
    LONG refs, done, cancelled;
    HANDLE child_in, child_out, section, process, job;
    HWND window;
    UINT message;
    uint32_t generation;
    DWORD pid;
    SIZE_T memory_limit;
    int ok;
    wchar_t exe[MAX_PATH + 16], dir[MAX_PATH];
};

static void release(preview_launch *a)
{
    if (InterlockedDecrement(&a->refs)) return;
    if (a->child_in) CloseHandle(a->child_in);
    if (a->child_out) CloseHandle(a->child_out);
    if (a->section) CloseHandle(a->section);
    if (a->job) CloseHandle(a->job); /* KILL_ON_JOB_CLOSE owns a late child */
    if (a->process) CloseHandle(a->process);
    free(a);
}

static HANDLE restricted_token(void)
{
    HANDLE own = NULL, low = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT,
                          &own)) return NULL;
    if (!CreateRestrictedToken(own, DISABLE_MAX_PRIVILEGE, 0, NULL, 0, NULL, 0, NULL, &low)) low = NULL;
    CloseHandle(own);
    if (!low) return NULL;
    SID_IDENTIFIER_AUTHORITY label = SECURITY_MANDATORY_LABEL_AUTHORITY;
    PSID sid = NULL;
    if (!AllocateAndInitializeSid(&label, 1, SECURITY_MANDATORY_LOW_RID, 0, 0, 0, 0, 0, 0, 0, &sid)) {
        CloseHandle(low);
        return NULL;
    }
    TOKEN_MANDATORY_LABEL tml = { { sid, SE_GROUP_INTEGRITY } };
    BOOL low_integrity = SetTokenInformation(low, TokenIntegrityLevel, &tml,
                                             (DWORD)(sizeof tml + GetLengthSid(sid)));
    FreeSid(sid);
    if (!low_integrity) {
        CloseHandle(low);
        return NULL;
    }
    return low;
}

static DWORD WINAPI launch_worker(void *arg)
{
    preview_launch *a = arg;
    HANDLE token = NULL;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    PROCESS_INFORMATION pi = {0};
    SIZE_T asize = 0;
    if (InterlockedCompareExchange(&a->cancelled, 0, 0)) goto out;
    token = restricted_token();
    if (!token) goto out;
    a->job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim = {0};
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                                           JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION |
                                           JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    lim.ProcessMemoryLimit = a->memory_limit;
    if (!a->job || !SetInformationJobObject(a->job, JobObjectExtendedLimitInformation, &lim, sizeof lim)) goto out;
    InitializeProcThreadAttributeList(NULL, 2, 0, &asize);
    attrs = malloc(asize);
    if (!attrs || !InitializeProcThreadAttributeList(attrs, 2, 0, &asize)) goto out;
    HANDLE inherit[3] = { a->child_in, a->child_out, a->section };
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, NULL, NULL) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &a->job, sizeof a->job, NULL, NULL)) goto out;
    STARTUPINFOEXW si = { { sizeof si } };
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = a->child_in;
    si.StartupInfo.hStdOutput = a->child_out;
    si.lpAttributeList = attrs;
    wchar_t cmd[MAX_PATH + 64];
    _snwprintf(cmd, MAX_PATH + 64, L"\"%ls\" %llu", a->exe, (unsigned long long)(uintptr_t)a->section);
    cmd[MAX_PATH + 63] = 0;
    if (!CreateProcessAsUserW(token, a->exe, cmd, NULL, NULL, TRUE,
                              CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                              NULL, a->dir, &si.StartupInfo, &pi)) goto out;
    a->process = pi.hProcess;
    a->pid = pi.dwProcessId;
    if (!InterlockedCompareExchange(&a->cancelled, 0, 0))
        a->ok = ResumeThread(pi.hThread) != (DWORD)-1;
    CloseHandle(pi.hThread);
out:
    if (attrs) {
        DeleteProcThreadAttributeList(attrs);
        free(attrs);
    }
    if (token) CloseHandle(token);
    if (a->child_in) { CloseHandle(a->child_in); a->child_in = NULL; }
    if (a->child_out) { CloseHandle(a->child_out); a->child_out = NULL; }
    if (a->section) { CloseHandle(a->section); a->section = NULL; }
    InterlockedExchange(&a->done, 1);
    if (!InterlockedCompareExchange(&a->cancelled, 0, 0))
        PostMessageW(a->window, a->message, a->generation, 0);
    release(a);
    return 0;
}

static int duplicate_inheritable(HANDLE source, HANDLE *dest)
{
    return source && DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), dest, 0, TRUE, DUPLICATE_SAME_ACCESS);
}

preview_launch *preview_launch_begin(const wchar_t *exe_name, HANDLE child_in, HANDLE child_out,
                                     HANDLE section, SIZE_T memory_limit, HWND window, UINT message,
                                     uint32_t generation)
{
    preview_launch *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->refs = 1;
    a->window = window; a->message = message; a->generation = generation;
    a->memory_limit = memory_limit;
    DWORD n = GetModuleFileNameW(NULL, a->dir, MAX_PATH);
    wchar_t *slash = n && n < MAX_PATH ? wcsrchr(a->dir, L'\\') : NULL;
    if (!slash || (size_t)(slash - a->dir) + wcslen(exe_name) + 2 >= MAX_PATH + 16) goto fail;
    wcscpy(slash + 1, exe_name);
    wcscpy(a->exe, a->dir);
    slash[1] = 0;
    if (!duplicate_inheritable(child_in, &a->child_in) ||
        !duplicate_inheritable(child_out, &a->child_out) ||
        !duplicate_inheritable(section, &a->section)) goto fail;
    InterlockedIncrement(&a->refs); /* worker */
    HANDLE thread = CreateThread(NULL, 0, launch_worker, a, 0, NULL);
    if (!thread) { release(a); goto fail; }
    CloseHandle(thread);
    return a;
fail:
    release(a);
    return NULL;
}

int preview_launch_take(preview_launch **slot, HANDLE *process, HANDLE *job, DWORD *pid)
{
    preview_launch *a = *slot;
    if (!a || !InterlockedCompareExchange(&a->done, 0, 0)) return 0;
    *slot = NULL;
    int ok = a->ok;
    if (ok) {
        *process = a->process; a->process = NULL;
        *job = a->job; a->job = NULL;
        *pid = a->pid;
    }
    release(a);
    return ok ? 1 : -1;
}

void preview_launch_cancel(preview_launch **slot)
{
    preview_launch *a = *slot;
    *slot = NULL;
    if (!a) return;
    InterlockedExchange(&a->cancelled, 1);
    release(a);
}
