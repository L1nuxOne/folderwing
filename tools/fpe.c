/* fpe - command line front end for the engine: listing, benchmarks,
 * watching and file operations. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "fpe.h"

/* ---- output --------------------------------------------------------------- */

static HANDLE g_out;
static int g_console;

static void out(const wchar_t *fmt, ...)
{
    wchar_t buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf(buf, 4095, fmt, ap);
    va_end(ap);
    if (n < 0) n = 4095;
    buf[n] = 0;
    DWORD w;
    if (g_console) {
        WriteConsoleW(g_out, buf, (DWORD)n, &w, NULL);
    } else {
        char u8[4096 * 3];
        int m = WideCharToMultiByte(CP_UTF8, 0, buf, n, u8, sizeof u8, NULL, NULL);
        WriteFile(g_out, u8, (DWORD)m, &w, NULL);
    }
}

static void fmt_size(wchar_t *b, size_t cap, uint64_t v)
{
    static const wchar_t *units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double d = (double)v;
    int u = 0;
    while (d >= 1024 && u < 4) { d /= 1024; u++; }
    if (u == 0) _snwprintf(b, cap, L"%llu B", (unsigned long long)v);
    else _snwprintf(b, cap, L"%.1f %ls", d, units[u]);
}

static void fmt_time(wchar_t *b, size_t cap, int64_t ft)
{
    FILETIME f = { (DWORD)ft, (DWORD)(ft >> 32) }, lf;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&f, &lf);
    FileTimeToSystemTime(&lf, &st);
    _snwprintf(b, cap, L"%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
}

static uint64_t now_us(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart * 1000000.0 / (double)f.QuadPart);
}

static void err_msg(const wchar_t *what, DWORD err)
{
    wchar_t msg[512] = L"";
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0, msg, 512, NULL);
    out(L"error: %ls: %ls (%lu)\n", what, msg, err);
}

static int parse_sort(const wchar_t *s, fpe_sort_key *k)
{
    if (!_wcsicmp(s, L"name")) *k = FPE_SORT_NAME;
    else if (!_wcsicmp(s, L"ext")) *k = FPE_SORT_EXT;
    else if (!_wcsicmp(s, L"size")) *k = FPE_SORT_SIZE;
    else if (!_wcsicmp(s, L"mtime")) *k = FPE_SORT_MTIME;
    else if (!_wcsicmp(s, L"ctime")) *k = FPE_SORT_CTIME;
    else return 0;
    return 1;
}

/* ---- ls ------------------------------------------------------------------ */

static int cmd_ls(int argc, wchar_t **argv)
{
    const wchar_t *path = L".";
    fpe_view_opts o = { FPE_SORT_NAME, 0, 1, 0, 0 };
    const wchar_t *filter = NULL;
    uint32_t limit = UINT32_MAX;
    int plain = 0;

    for (int i = 0; i < argc; i++) {
        if (!wcscmp(argv[i], L"-s") && i + 1 < argc) {
            if (!parse_sort(argv[++i], &o.key)) { out(L"unknown sort key %ls\n", argv[i]); return 2; }
        } else if (!wcscmp(argv[i], L"-r")) o.descending = 1;
        else if (!wcscmp(argv[i], L"-a")) o.show_hidden = o.show_system = 1;
        else if (!wcscmp(argv[i], L"--mixed")) o.dirs_first = 0;
        else if (!wcscmp(argv[i], L"-f") && i + 1 < argc) filter = argv[++i];
        else if (!wcscmp(argv[i], L"-n") && i + 1 < argc) limit = (uint32_t)_wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--9p")) plain = 1;
        else path = argv[i];
    }

    /* WSL folders: through the distro's helper unless --9p */
    fpe_dir *d;
    fpe_err err = plain ? ERROR_NOT_SUPPORTED : fpe_wsl_read(path, &d);
    if (err == ERROR_NOT_SUPPORTED) err = fpe_dir_read(path, &d);
    if (err) { err_msg(path, err); return 1; }

    uint64_t t0 = now_us();
    fpe_view *v = fpe_view_create(d, &o);
    if (filter) fpe_view_set_filter(v, filter);
    uint64_t sort_us = now_us() - t0;

    uint32_t n = fpe_view_count(v);
    for (uint32_t i = 0; i < n && i < limit; i++) {
        const fpe_entry *e = fpe_view_at(v, i);
        wchar_t sz[32], tm[32];
        fmt_time(tm, 32, e->mtime);
        if (fpe_entry_is_dir(e)) wcscpy(sz, L"<dir>");
        else fmt_size(sz, 32, e->size);
        fpe_unix u;
        if (fpe_dir_unix(d, e, &u)) {
            wchar_t mode[11];
            fpe_unix_mode_string(u.mode, mode);
            out(L"%ls  %ls %-8ls %-8ls %10ls  %ls%ls%ls%ls\n", tm, mode, u.owner ? u.owner : L"?",
                u.group ? u.group : L"?", sz, fpe_view_name(v, i), fpe_entry_is_dir(e) ? L"\\" : L"",
                u.link ? L" -> " : L"", u.link ? u.link : L"");
        } else {
            out(L"%ls  %10ls  %ls%ls\n", tm, sz, fpe_view_name(v, i), fpe_entry_is_dir(e) ? L"\\" : L"");
        }
    }

    fpe_dir_stats st;
    fpe_dir_get_stats(d, &st);
    wchar_t total[32], mem[32];
    fmt_size(total, 32, st.file_bytes);
    fmt_size(mem, 32, st.memory_bytes);
    out(L"\n%u shown of %u (%u dirs, %u files, %ls)  read %.2f ms, sort+filter %.2f ms, %ls snapshot\n",
        n, st.entries, st.dirs, st.files, total, st.read_us / 1000.0, sort_us / 1000.0, mem);
    fpe_wsl_stats ws;
    fpe_wsl_get_stats(&ws);
    if (ws.starts)
        out(L"WSL helper: %ls, started in %u ms%ls\n", fpe_dir_is_linux(d) ? L"listed it" : L"not used",
            ws.last_start_ms, ws.failures ? L" (failed to answer)" : L"");

    fpe_view_destroy(v);
    fpe_dir_release(d);
    return 0;
}

/* ---- bench ---------------------------------------------------------------- */

static uint32_t baseline_findfirst(const wchar_t *path)
{
    wchar_t pat[MAX_PATH + 8];
    _snwprintf(pat, MAX_PATH + 8, L"%ls\\*", path);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return 0;
    uint32_t n = 0;
    do n++; while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

/* What a typical explorer does: one heap allocation per name, plus a
 * growing array of records. */
typedef struct naive_item {
    wchar_t *name;
    uint64_t size;
    FILETIME mtime;
    DWORD attrs;
} naive_item;

static uint32_t baseline_store(const wchar_t *path)
{
    wchar_t pat[MAX_PATH + 8];
    _snwprintf(pat, MAX_PATH + 8, L"%ls\\*", path);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return 0;
    uint32_t n = 0, cap = 0;
    naive_item *items = NULL;
    do {
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            items = realloc(items, cap * sizeof *items);
        }
        naive_item *it = &items[n++];
        it->name = _wcsdup(fd.cFileName);
        it->size = (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow;
        it->mtime = fd.ftLastWriteTime;
        it->attrs = fd.dwFileAttributes;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (uint32_t i = 0; i < n; i++) free(items[i].name);
    free(items);
    return n;
}

static double median(double *v, int n)
{
    for (int i = 1; i < n; i++) {
        double x = v[i];
        int j = i;
        while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; j--; }
        v[j] = x;
    }
    return n ? v[n / 2] : 0;
}

static int cmd_bench(int argc, wchar_t **argv)
{
    const wchar_t *path = L".";
    int iters = 20;
    for (int i = 0; i < argc; i++) {
        if (!wcscmp(argv[i], L"-i") && i + 1 < argc) iters = _wtoi(argv[++i]);
        else path = argv[i];
    }
    if (iters < 1) iters = 1;
    if (iters > 1000) iters = 1000;

    wchar_t full[MAX_PATH];
    GetFullPathNameW(path, MAX_PATH, full, NULL);

    double t[1000];
    fpe_dir *d = NULL;
    fpe_err err = fpe_dir_read(full, &d);
    if (err) { err_msg(full, err); return 1; }
    fpe_dir_stats st;
    fpe_dir_get_stats(d, &st);
    out(L"%ls\n%u entries (%u dirs, %u files)\n\n", full, st.entries, st.dirs, st.files);

    /* Interleave the three readers so caching and background noise hit
     * them equally. */
    double t_ff[1000], t_store[1000];
    for (int i = 0; i < iters; i++) {
        fpe_dir *x;
        uint64_t a = now_us();
        fpe_dir_read(full, &x);
        t[i] = (now_us() - a) / 1000.0;
        fpe_dir_release(x);

        a = now_us();
        baseline_findfirst(full);
        t_ff[i] = (now_us() - a) / 1000.0;

        a = now_us();
        baseline_store(full);
        t_store[i] = (now_us() - a) / 1000.0;
    }
    double fpe_ms = median(t, iters), ff_ms = median(t_ff, iters), store_ms = median(t_store, iters);
    out(L"%-34ls %10ls\n", L"operation", L"median");
    out(L"%-34ls %8.3f ms\n", L"fpe_dir_read (warm)", fpe_ms);
    out(L"%-34ls %8.3f ms\n", L"FindFirstFileEx loop (no storage)", ff_ms);
    out(L"%-34ls %8.3f ms\n", L"FindFirstFileEx + malloc per entry", store_ms);
    fpe_dir *w = NULL;
    if (!fpe_wsl_read(full, &w)) { /* a WSL folder: the same through the distro's helper */
        fpe_dir_release(w);
        for (int i = 0; i < iters; i++) {
            uint64_t a = now_us();
            fpe_wsl_read(full, &w);
            t[i] = (now_us() - a) / 1000.0;
            fpe_dir_release(w);
        }
        out(L"%-34ls %8.3f ms\n", L"WSL helper (Linux details)", median(t, iters));
    }

    static const struct { fpe_sort_key k; const wchar_t *n; } keys[] = {
        { FPE_SORT_NAME, L"sort name (natural)" }, { FPE_SORT_EXT, L"sort type" },
        { FPE_SORT_SIZE, L"sort size" }, { FPE_SORT_MTIME, L"sort modified" },
    };
    for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++) {
        fpe_view_opts o = { keys[k].k, 0, 1, 1, 1 };
        for (int i = 0; i < iters; i++) {
            uint64_t a = now_us();
            fpe_view *v = fpe_view_create(d, &o);
            t[i] = (now_us() - a) / 1000.0;
            fpe_view_destroy(v);
        }
        out(L"%-34ls %8.3f ms\n", keys[k].n, median(t, iters));
    }

    fpe_view *v = fpe_view_create(d, NULL);
    const wchar_t *typed[] = { L"a", L"ab", L"abc" };
    for (int i = 0; i < iters; i++) {
        uint64_t a = now_us();
        for (int s = 0; s < 3; s++) fpe_view_set_filter(v, typed[s]);
        fpe_view_set_filter(v, NULL);
        t[i] = (now_us() - a) / 1000.0 / 4;
    }
    out(L"%-34ls %8.3f ms\n", L"filter per keystroke", median(t, iters));
    fpe_view_destroy(v);

    wchar_t mem[32];
    fmt_size(mem, 32, st.memory_bytes);
    out(L"\nsnapshot memory: %ls (%.1f bytes/entry)\n", mem,
        st.entries ? (double)st.memory_bytes / st.entries : 0.0);
    if (fpe_ms > 0) out(L"read vs FindFirstFileEx+malloc: %.2fx faster\n", store_ms / fpe_ms);

    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc)) {
        wchar_t ws[32], pb[32];
        fmt_size(ws, 32, pmc.PeakWorkingSetSize);
        fmt_size(pb, 32, pmc.PrivateUsage);
        out(L"process: peak working set %ls, private %ls\n", ws, pb);
    }
    fpe_dir_release(d);
    return 0;
}

/* ---- watch ---------------------------------------------------------------- */

static HANDLE g_quit;

static BOOL WINAPI on_ctrl(DWORD type)
{
    (void)type;
    SetEvent(g_quit);
    return TRUE;
}

typedef struct watch_ctx {
    const wchar_t *path;
    fpe_engine    *engine;
    uint32_t       last_count;
} watch_ctx;

static void on_reloaded(void *user, uint64_t ticket, fpe_err err, fpe_dir *dir)
{
    watch_ctx *c = user;
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (err) {
        out(L"[%02u:%02u:%02u] folder unavailable (%lu)\n", st.wHour, st.wMinute, st.wSecond, err);
        return;
    }
    fpe_dir_stats s;
    fpe_dir_get_stats(dir, &s);
    out(L"[%02u:%02u:%02u] changed: %u entries (%+d), reloaded in %.2f ms\n", st.wHour, st.wMinute,
        st.wSecond, s.entries, (int)s.entries - (int)c->last_count, s.read_us / 1000.0);
    c->last_count = s.entries;
    fpe_dir_release(dir);
}

static void on_change(void *user, fpe_watch *w)
{
    watch_ctx *c = user;
    fpe_engine_load(c->engine, c->path, on_reloaded, c);
}

static int cmd_watch(int argc, wchar_t **argv)
{
    watch_ctx c = { argc ? argv[0] : L".", fpe_engine_create(NULL), 0 };
    fpe_dir *d;
    if (!fpe_dir_read(c.path, &d)) { c.last_count = fpe_dir_count(d); fpe_dir_release(d); }

    fpe_watch *w;
    fpe_err err = fpe_watch_start(c.path, 60, on_change, &c, &w);
    if (err) { err_msg(c.path, err); fpe_engine_destroy(c.engine); return 1; }
    out(L"watching %ls (%u entries), Ctrl+C to stop\n", c.path, c.last_count);

    g_quit = CreateEventW(NULL, TRUE, FALSE, NULL);
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    WaitForSingleObject(g_quit, INFINITE);
    fpe_watch_stop(w);
    fpe_engine_destroy(c.engine);
    return 0;
}

/* ---- file operations ------------------------------------------------------ */

static HANDLE g_job_done;
static fpe_job_progress g_final;

static void on_job(void *user, const fpe_job_progress *p)
{
    wchar_t done[32], total[32];
    fmt_size(done, 32, p->bytes_done);
    fmt_size(total, 32, p->bytes_total);
    static const wchar_t *states[] = { L"queued", L"scanning", L"running", L"done", L"cancelled", L"failed" };
    out(L"\r%-9ls %u/%u items  %ls / %ls  errors %u  skipped %u     ", states[p->state], p->items_done,
        p->items_total, done, total, p->errors, p->skipped);
    if (p->errors && p->current && p->last_error) {
        out(L"\n  ! %ls (%lu)\n", p->current, p->last_error);
    }
    if (p->state >= FPE_JOB_DONE) {
        g_final = *p;
        out(L"\n");
        SetEvent(g_job_done);
    }
}

static BOOL WINAPI on_ctrl_job(DWORD type)
{
    (void)type;
    SetEvent(g_quit);
    return TRUE;
}

static int cmd_op(fpe_op op, int argc, wchar_t **argv)
{
    fpe_conflict policy = FPE_CONFLICT_RENAME;
    int plain9p=0;
    const wchar_t *items[256];
    int n = 0;
    for (int i = 0; i < argc; i++) {
        if (!wcscmp(argv[i], L"--skip")) policy = FPE_CONFLICT_SKIP;
        else if (!wcscmp(argv[i], L"--overwrite")) policy = FPE_CONFLICT_OVERWRITE;
        else if (!wcscmp(argv[i], L"--fail")) policy = FPE_CONFLICT_FAIL;
        else if (!wcscmp(argv[i], L"--9p")) {plain9p=1;fpe_wsl_configure(&(fpe_wsl_config){0,0,60000});}
        else {
            if (n == (int)(sizeof items / sizeof *items)) {
                out(L"too many operands (maximum 256, including any destination); no operation submitted\n");
                return 2;
            }
            items[n++] = argv[i];
        }
    }
    const wchar_t *dest = NULL;
    if (op == FPE_OP_COPY || op == FPE_OP_MOVE) {
        if (n < 2) { out(L"need at least one source and a destination folder\n"); return 2; }
        dest = items[--n];
    } else if (n < 1) {
        out(L"need at least one path\n");
        return 2;
    }

    fpe_engine *e = fpe_engine_create(NULL);
    g_job_done = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_quit = CreateEventW(NULL, TRUE, FALSE, NULL);
    SetConsoleCtrlHandler(on_ctrl_job, TRUE);

    uint64_t id = fpe_job_submit(e, op, items, (uint32_t)n, dest, policy, on_job, NULL);
    if (!id) { DWORD error=GetLastError();err_msg(L"operation not submitted",error?error:ERROR_INVALID_PARAMETER);fpe_engine_destroy(e);return 2; }

    HANDLE hs[2] = { g_job_done, g_quit };
    if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
        fpe_job_cancel(e, id);
        WaitForSingleObject(g_job_done, INFINITE);
    }
    fpe_engine_destroy(e);
    if(plain9p&&g_final.state==FPE_JOB_FAILED&&g_final.last_error==ERROR_NOT_SUPPORTED){
        int linux_mutation=dest&&fpe_wsl_path_kind(dest);
        for(int i=0;op==FPE_OP_MOVE&&i<n;i++)linux_mutation|=fpe_wsl_path_kind(items[i]);
        if(linux_mutation)out(L"Legacy --9p cannot provide required Linux directory durability. Transfer refused before mutation; use native WSL transfers (omit --9p). (%lu)\n",g_final.last_error);
    }
    return g_final.state == FPE_JOB_DONE && g_final.errors == 0 ? 0 : 1;
}

/* ---- main ----------------------------------------------------------------- */

static void usage(void)
{
    out(L"folderwing-cli %d.%d.%d - Folderwing file engine CLI\n\n"
        L"usage: folderwing-cli [--profile] <command> ...   (--profile prints a timing report at the end)\n\n"
        L"  folderwing-cli ls [path] [-s name|ext|size|mtime|ctime] [-r] [-a] [--mixed] [-f text] [-n max] [--9p]\n"
        L"  folderwing-cli bench [path] [-i iterations]\n"
        L"  folderwing-cli watch [path]\n"
        L"  folderwing-cli cp <src>... <dest-dir> [--skip|--overwrite|--fail] [--9p]\n"
        L"  folderwing-cli mv <src>... <dest-dir> [--skip|--overwrite|--fail] [--9p]\n"
        L"  folderwing-cli rm <path>...           (permanent)\n"
        L"  folderwing-cli recycle <path>...\n",
        FPE_VERSION_MAJOR, FPE_VERSION_MINOR, FPE_VERSION_PATCH);
}

int wmain(int argc, wchar_t **argv)
{
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    g_console = GetConsoleMode(g_out, &mode);

    int profile = argc > 1 && !wcscmp(argv[1], L"--profile");
    if (profile) {
        fpe_prof_enable(1);
        argv++;
        argc--;
    }
    if (argc < 2) { usage(); return 2; }
    const wchar_t *c = argv[1];
    int n = argc - 2;
    wchar_t **a = argv + 2;
    int rc;
    if (!wcscmp(c, L"ls")) rc = cmd_ls(n, a);
    else if (!wcscmp(c, L"bench")) rc = cmd_bench(n, a);
    else if (!wcscmp(c, L"watch")) rc = cmd_watch(n, a);
    else if (!wcscmp(c, L"cp")) rc = cmd_op(FPE_OP_COPY, n, a);
    else if (!wcscmp(c, L"mv")) rc = cmd_op(FPE_OP_MOVE, n, a);
    else if (!wcscmp(c, L"rm")) rc = cmd_op(FPE_OP_DELETE, n, a);
    else if (!wcscmp(c, L"recycle")) rc = cmd_op(FPE_OP_RECYCLE, n, a);
    else { usage(); return 2; }

    if (profile) {
        static char report[64 * 1024];
        fpe_prof_format(report, sizeof report);
        out(L"\n");
        DWORD w;
        if (g_console) {
            wchar_t wide[64 * 1024];
            int m = MultiByteToWideChar(CP_UTF8, 0, report, -1, wide, 64 * 1024);
            if (m > 1) WriteConsoleW(g_out, wide, (DWORD)(m - 1), &w, NULL);
        } else {
            WriteFile(g_out, report, (DWORD)strlen(report), &w, NULL);
        }
    }
    return rc;
}
