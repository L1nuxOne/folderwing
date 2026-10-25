#include "../src/namecodec.h"
/* Self-contained engine tests. Each run works in a fresh temp folder. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <sddl.h>
#include <winioctl.h>
#include <string.h>

#include "fpe.h"
#include "jobwait.h"

static int g_failed, g_passed;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) g_passed++;                                                    \
        else { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

static wchar_t g_root[MAX_PATH];

static void path_join(wchar_t *out, const wchar_t *a, const wchar_t *b)
{
    _snwprintf(out, MAX_PATH, L"%ls\\%ls", a, b);
    out[MAX_PATH - 1] = 0;
}

static void make_file(const wchar_t *rel, size_t size, DWORD attrs)
{
    wchar_t p[MAX_PATH];
    path_join(p, g_root, rel);
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { printf("  cannot create %ls\n", p); return; }
    char buf[4096];
    memset(buf, 'x', sizeof buf);
    while (size) {
        DWORD n = size > sizeof buf ? (DWORD)sizeof buf : (DWORD)size, w;
        WriteFile(h, buf, n, &w, NULL);
        size -= n;
    }
    CloseHandle(h);
    if (attrs) SetFileAttributesW(p, attrs);
}

static void make_dir(const wchar_t *rel)
{
    wchar_t p[MAX_PATH];
    path_join(p, g_root, rel);
    CreateDirectoryW(p, NULL);
}

static int exists(const wchar_t *rel)
{
    wchar_t p[MAX_PATH];
    path_join(p, g_root, rel);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

static uint64_t file_size(const wchar_t *rel)
{
    wchar_t p[MAX_PATH];
    path_join(p, g_root, rel);
    WIN32_FILE_ATTRIBUTE_DATA ad;
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &ad)) return UINT64_MAX;
    return (uint64_t)ad.nFileSizeHigh << 32 | ad.nFileSizeLow;
}

/* ---------------------------------------------------------------------- */

static void test_natural_compare(void)
{
    printf("natural compare\n");
    CHECK(fpe_compare_names(L"file2", L"file10") < 0);
    CHECK(fpe_compare_names(L"file10", L"file2") > 0);
    CHECK(fpe_compare_names(L"File1", L"file2") < 0);
    CHECK(fpe_compare_names(L"a", L"B") < 0);
    CHECK(fpe_compare_names(L"abc", L"abcd") < 0);
    CHECK(fpe_compare_names(L"x9y", L"x10y") < 0);
    CHECK(fpe_compare_names(L"v1.9", L"v1.10") < 0);
    CHECK(fpe_compare_names(L"_a", L"a") < 0);
    CHECK(fpe_compare_names(L"9", L"a") < 0);
    CHECK(fpe_compare_names(L"same", L"same") == 0);
    CHECK(fpe_compare_names(L"\x00e9t\x00e9", L"zebra") < 0); /* été < zebra */
    CHECK(fpe_compare_names(L"\x00e9t\x00e9", L"apple") > 0);
}

static fpe_dir *read_root(void)
{
    fpe_dir *d = NULL;
    fpe_err err = fpe_dir_read(g_root, &d);
    CHECK(err == 0 && d);
    return d;
}

static void test_dir_read(void)
{
    printf("directory snapshot\n");
    fpe_dir *d = read_root();
    if (!d) return;

    CHECK(fpe_dir_count(d) == 8);
    fpe_dir_stats st;
    fpe_dir_get_stats(d, &st);
    CHECK(st.dirs == 2);
    CHECK(st.files == 6);
    CHECK(st.file_bytes == 100 + 2000 + 5 + 7 + 1 + 3);
    CHECK(st.memory_bytes > 0 && st.memory_bytes < 64 * 1024);

    const fpe_entry *e = fpe_dir_entries(d);
    int saw_ext = 0;
    for (uint32_t i = 0; i < fpe_dir_count(d); i++) {
        const wchar_t *n = fpe_dir_name(d, &e[i]);
        CHECK(wcslen(n) == e[i].name_len);
        if (wcscmp(n, L"a.txt") == 0) {
            CHECK(e[i].size == 100);
            CHECK(wcscmp(n + e[i].ext, L"txt") == 0);
            saw_ext = 1;
        }
        if (wcscmp(n, L"sub") == 0) CHECK(fpe_entry_is_dir(&e[i]));
    }
    CHECK(saw_ext);
    fpe_dir_release(d);

    fpe_dir *missing = NULL;
    wchar_t p[MAX_PATH];
    path_join(p, g_root, L"does-not-exist");
    fpe_err err = fpe_dir_read(p, &missing);
    CHECK(err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND);
    CHECK(missing == NULL);
}

static void test_view(void)
{
    printf("view sort + filter\n");
    fpe_dir *d = read_root();
    if (!d) return;

    fpe_view *v = fpe_view_create(d, NULL);
    CHECK(v != NULL);
    /* hidden.txt is hidden; everything else visible, dirs first */
    CHECK(fpe_view_count(v) == 7);
    CHECK(wcscmp(fpe_view_name(v, 0), L"empty") == 0);
    CHECK(wcscmp(fpe_view_name(v, 1), L"sub") == 0);
    CHECK(wcscmp(fpe_view_name(v, 2), L"a.txt") == 0);
    CHECK(wcscmp(fpe_view_name(v, 3), L"B.TXT") == 0);
    CHECK(wcscmp(fpe_view_name(v, 4), L"file2.log") == 0);
    CHECK(wcscmp(fpe_view_name(v, 5), L"file10.log") == 0);

    fpe_view_opts o;
    fpe_view_get_opts(v, &o);
    o.key = FPE_SORT_SIZE;
    o.descending = 1;
    fpe_view_set_opts(v, &o);
    CHECK(wcscmp(fpe_view_name(v, 2), L"B.TXT") == 0); /* 2000 bytes */

    o.show_hidden = 1;
    fpe_view_set_opts(v, &o);
    CHECK(fpe_view_count(v) == 8);

    o.key = FPE_SORT_EXT;
    o.descending = 0;
    fpe_view_set_opts(v, &o);
    /* after dirs: .log, .log, then .md, then .txt ... */
    CHECK(wcscmp(fpe_view_name(v, 2), L"file2.log") == 0);

    fpe_view_set_filter(v, L"FILE");
    CHECK(fpe_view_count(v) == 2);
    fpe_view_set_filter(v, L"file1");
    CHECK(fpe_view_count(v) == 1);
    CHECK(wcscmp(fpe_view_name(v, 0), L"file10.log") == 0);
    fpe_view_set_filter(v, L"");
    CHECK(fpe_view_count(v) == 8);
    fpe_view_set_filter(v, L"zzz");
    CHECK(fpe_view_count(v) == 0);
    fpe_view_set_filter(v, NULL);

    CHECK(fpe_view_find(v, L"b.txt") != UINT32_MAX);
    CHECK(fpe_view_find(v, L"nope") == UINT32_MAX);

    /* swapping the snapshot keeps options */
    fpe_dir *d2 = read_root();
    CHECK(fpe_view_set_dir(v, d2) == 0);
    CHECK(fpe_view_count(v) == 8);
    fpe_dir_release(d2);

    fpe_view_destroy(v);
    fpe_dir_release(d);
}

static void test_view_order_snapshot(void)
{
    printf("view snapshots: immutable visible order and monotonic revisions\n");
    fpe_dir *d=read_root();if(!d)return;fpe_view *v=fpe_view_create(d,NULL);CHECK(v!=NULL);if(!v){fpe_dir_release(d);return;}
    uint32_t order[32],count=fpe_view_count(v);uint64_t rev=fpe_view_revision(v);CHECK(rev!=0&&count<32);
    CHECK(fpe_view_copy_order(v,order,32)==0);
    for(uint32_t i=0;i<count;i++)CHECK(order[i]==fpe_view_entry_index(v,i));
    uint32_t original=order[0];CHECK(fpe_view_copy_order(v,order,count-1)==ERROR_INSUFFICIENT_BUFFER);CHECK(order[0]==original);
    CHECK(fpe_view_copy_order(v,NULL,32)==ERROR_INVALID_PARAMETER);CHECK(fpe_view_revision(NULL)==0);
    fpe_view_opts opts;fpe_view_get_opts(v,&opts);fpe_view_set_opts(v,&opts);CHECK(fpe_view_revision(v)==rev);
    CHECK(fpe_view_set_filter(v,L"file")==0&&fpe_view_revision(v)>rev);rev=fpe_view_revision(v);
    CHECK(order[0]==original&&wcscmp(fpe_dir_name(d,fpe_dir_entries(d)+order[0]),L"empty")==0);
    CHECK(fpe_view_set_filter(v,L"FILE")==0&&fpe_view_revision(v)==rev);
    opts.descending=1;fpe_view_set_opts(v,&opts);CHECK(fpe_view_revision(v)>rev);rev=fpe_view_revision(v);
    CHECK(fpe_view_set_dir(v,d)==0&&fpe_view_revision(v)>rev);
    CHECK(fpe_view_set_filter(v,L"not present")==0);CHECK(fpe_view_copy_order(v,NULL,0)==0);
    fpe_view *other=fpe_view_create(d,NULL);CHECK(other&&fpe_view_revision(other)!=fpe_view_revision(v));
    fpe_view_destroy(other);fpe_view_destroy(v);fpe_dir_release(d);
}

/* Sorting must be correct whatever order the filesystem returns, including
 * the presorted and reversed runs the merge sort takes shortcuts on. */
static void test_view_sort_orders(void)
{
    printf("view sort orders\n");
    wchar_t dir[MAX_PATH], p[MAX_PATH];
    _snwprintf(dir, MAX_PATH, L"%ls_many", g_root);
    CreateDirectoryW(dir, NULL);
    enum { N = 300 };
    for (int i = 0; i < N; i++) {
        _snwprintf(p, MAX_PATH, L"%ls\\item%d.%ls", dir, (i * 7919) % 1000, (i % 3) ? L"txt" : L"log");
        HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        DWORD w;
        char buf[64] = { 0 };
        WriteFile(h, buf, (DWORD)((i * 37) % 64), &w, NULL);
        CloseHandle(h);
    }

    fpe_dir *d = NULL;
    CHECK(fpe_dir_read(dir, &d) == 0 && d);
    if (d) {
        fpe_view_opts o = { FPE_SORT_NAME, 0, 1, 1, 1 };
        fpe_view *asc = fpe_view_create(d, &o);
        o.descending = 1;
        fpe_view *desc = fpe_view_create(d, &o);
        uint32_t n = fpe_view_count(asc);
        CHECK(n == N && fpe_view_count(desc) == n);
        int ok = 1;
        for (uint32_t i = 0; i + 1 < n; i++)
            if (fpe_compare_names(fpe_view_name(asc, i), fpe_view_name(asc, i + 1)) > 0) ok = 0;
        for (uint32_t i = 0; i < n; i++)
            if (wcscmp(fpe_view_name(asc, i), fpe_view_name(desc, n - 1 - i)) != 0) ok = 0;
        CHECK(ok);

        fpe_view_get_opts(desc, &o);
        o.key = FPE_SORT_SIZE;
        for (int dsc = 0; dsc < 2; dsc++) {
            o.descending = (uint8_t)dsc;
            fpe_view_set_opts(desc, &o);
            ok = 1;
            for (uint32_t i = 0; i + 1 < n; i++) {
                uint64_t a = fpe_view_at(desc, i)->size, b = fpe_view_at(desc, i + 1)->size;
                if (dsc ? a < b : a > b) ok = 0;
                /* equal sizes fall back to ascending names */
                if (a == b && fpe_compare_names(fpe_view_name(desc, i), fpe_view_name(desc, i + 1)) > 0) ok = 0;
            }
            CHECK(ok);
        }
        fpe_view_destroy(asc);
        fpe_view_destroy(desc);
        fpe_dir_release(d);
    }

    WIN32_FIND_DATAW fd;
    _snwprintf(p, MAX_PATH, L"%ls\\*", dir);
    HANDLE f = FindFirstFileW(p, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == L'.') continue;
            _snwprintf(p, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
            DeleteFileW(p);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryW(dir);
}

/* ---------------------------------------------------------------------- */

static int prof_row(const char *name, fpe_prof_row *out)
{
    fpe_prof_row rows[256];
    uint32_t n = fpe_prof_rows(rows, 256);
    for (uint32_t i = 0; i < n; i++)
        if (strcmp(rows[i].name, name) == 0) { *out = rows[i]; return 1; }
    return 0;
}

static void test_profiling(void)
{
    printf("profiling\n");
    fpe_prof_row r;
    fpe_prof_enable(0);
    fpe_prof_reset();
    CHECK(fpe_prof_begin() == 0);

    fpe_dir *d = read_root();
    fpe_dir_release(d);
    CHECK(!prof_row("dir.read", &r)); /* zones stay silent while off */
    int64_t base = prof_row("dir.snapshots", &r) ? r.value : 0;

    fpe_prof_enable(1);
    d = read_root();
    CHECK(prof_row("dir.snapshots", &r) && r.value == base + 1);
    CHECK(prof_row("dir.snapshots.bytes", &r) && r.value > 0);
    fpe_view *v = fpe_view_create(d, NULL);
    fpe_view_set_filter(v, L"file");
    CHECK(prof_row("dir.read", &r) && r.calls == 1 && r.total_ms > 0);
    CHECK(prof_row("dir.read/query", &r) && r.calls >= 1);
    CHECK(prof_row("dir.read/open", &r) && r.calls == 1);
    CHECK(prof_row("view.sort", &r) && r.calls == 1);
    CHECK(prof_row("view.filter", &r) && r.calls == 2);
    CHECK(prof_row("view.live", &r) && r.value >= 1);
    fpe_view_destroy(v);
    fpe_dir_release(d);
    CHECK(prof_row("dir.snapshots", &r) && r.value == base);
    CHECK(prof_row("view.live", &r) && r.value == 0);

    int64_t t = fpe_prof_begin();
    Sleep(2);
    FPE_PROF_END("test.sleep", t);
    CHECK(prof_row("test.sleep", &r) && r.calls == 1 && r.total_ms >= 1.0);

    for (int i = 0; i < 3; i++) FPE_PROF_COUNT("test.count");
    CHECK(prof_row("test.count", &r) && r.calls == 3 && r.total_ms == 0);

    static char report[16384];
    size_t len = fpe_prof_format(report, sizeof report);
    CHECK(len > 0 && strlen(report) == len);
    CHECK(strstr(report, "dir.read") != NULL);
    CHECK(strstr(report, "dir.snapshots.bytes") != NULL);
    CHECK(strstr(report, "test.count") != NULL);

    char tiny[16];
    CHECK(fpe_prof_format(tiny, sizeof tiny) < sizeof tiny); /* truncates safely */

    fpe_prof_reset();
    CHECK(!prof_row("dir.read", &r));
    CHECK(!prof_row("test.count", &r));
    fpe_prof_enable(0);
    FPE_PROF_COUNT("test.count");
    CHECK(!prof_row("test.count", &r));
}

/* ---------------------------------------------------------------------- */

typedef struct load_result {
    HANDLE   ev;
    fpe_err  err;
    uint32_t count;
} load_result;

static void on_load(void *user, uint64_t ticket, fpe_err err, fpe_dir *dir)
{
    load_result *r = user;
    r->err = err;
    r->count = dir ? fpe_dir_count(dir) : 0;
    fpe_dir_release(dir);
    SetEvent(r->ev);
}

static void test_engine_cache(fpe_engine *e)
{
    printf("engine async load + cache\n");
    CHECK(fpe_engine_cached(e, g_root) == NULL);

    load_result r = { test_callback_event(FALSE), 1, 0 };
    CHECK(fpe_engine_load(e, g_root, on_load, &r) != 0);
    CHECK(test_wait_load(r.ev, 5000) == WAIT_OBJECT_0);
    CHECK(r.err == 0 && r.count == 8);

    fpe_dir *c = fpe_engine_cached(e, g_root);
    CHECK(c != NULL);
    if (c) {
        CHECK(fpe_dir_count(c) == 8);
        fpe_dir_release(c);
    }

    /* the cache key ignores case and trailing separators */
    wchar_t alt[MAX_PATH];
    _snwprintf(alt, MAX_PATH, L"%ls\\", g_root);
    CharUpperW(alt);
    c = fpe_engine_cached(e, alt);
    CHECK(c != NULL);
    fpe_dir_release(c);

    fpe_engine_invalidate(e, g_root);
    CHECK(fpe_engine_cached(e, g_root) == NULL);
    CloseHandle(r.ev);
}

/* ---------------------------------------------------------------------- */

static void on_watch(void *user, fpe_watch *w)
{
    InterlockedIncrement((volatile LONG *)user);
}

static void test_watch(void)
{
    printf("change watching\n");
    volatile LONG fired = 0;
    fpe_watch *w = NULL;
    CHECK(fpe_watch_start(g_root, 30, on_watch, (void *)&fired, &w) == 0);
    if (!w) return;

    /* a burst of changes collapses into very few callbacks */
    for (int i = 0; i < 20; i++) {
        wchar_t n[32];
        _snwprintf(n, 32, L"burst%d.tmp", i);
        make_file(n, 1, 0);
    }
    for (int t = 0; t < 100 && fired == 0; t++) Sleep(10);
    Sleep(150);
    CHECK(fired >= 1);
    CHECK(fired <= 3);

    for (int i = 0; i < 20; i++) {
        wchar_t n[32], p[MAX_PATH];
        _snwprintf(n, 32, L"burst%d.tmp", i);
        path_join(p, g_root, n);
        DeleteFileW(p);
    }
    fpe_watch_stop(w);
}

/* ---------------------------------------------------------------------- */

typedef struct job_result {
    HANDLE           ev;
    fpe_job_progress last;
    int              finals;
} job_result;

static void on_job(void *user, const fpe_job_progress *p)
{
    job_result *r = user;
    r->last = *p;
    r->last.current = NULL;
    if (p->state == FPE_JOB_DONE || p->state == FPE_JOB_CANCELLED || p->state == FPE_JOB_FAILED) {
        r->finals++;
        SetEvent(r->ev);
    }
}

static job_result run(fpe_engine *e, fpe_op op, const wchar_t *const *src, uint32_t n,
                      const wchar_t *dest, fpe_conflict policy)
{
    job_result r;
    memset(&r, 0, sizeof r);
    r.ev = test_callback_event(FALSE);
    uint64_t id = fpe_job_submit(e, op, src, n, dest, policy, on_job, &r);
    CHECK(id != 0);
    if (id) CHECK(test_wait_job(e, id, r.ev, 30000) == WAIT_OBJECT_0);
    Sleep(5);
    CloseHandle(r.ev);
    return r;
}

static void test_jobs(fpe_engine *e)
{
    printf("file operation jobs\n");
    wchar_t sub[MAX_PATH], dst[MAX_PATH], a[MAX_PATH];
    path_join(sub, g_root, L"sub");
    path_join(dst, g_root, L"dst");
    path_join(a, g_root, L"a.txt");
    make_dir(L"dst");

    const wchar_t *srcs[] = { sub, a };
    job_result r = run(e, FPE_OP_COPY, srcs, 2, dst, FPE_CONFLICT_RENAME);
    CHECK(r.finals == 1);
    CHECK(r.last.state == FPE_JOB_DONE);
    CHECK(r.last.errors == 0);
    CHECK(r.last.bytes_total == 100 + 4096 + 10);
    CHECK(file_size(L"dst\\sub\\deep\\inner.bin") == 4096);
    CHECK(file_size(L"dst\\sub\\note.md") == 10);
    CHECK(file_size(L"dst\\a.txt") == 100);

    /* same again: both kept */
    r = run(e, FPE_OP_COPY, srcs, 2, dst, FPE_CONFLICT_RENAME);
    CHECK(r.last.errors == 0);
    CHECK(exists(L"dst\\sub (2)\\deep\\inner.bin"));
    CHECK(exists(L"dst\\a (2).txt"));

    /* skip policy leaves existing files alone */
    r = run(e, FPE_OP_COPY, srcs + 1, 1, dst, FPE_CONFLICT_SKIP);
    CHECK(r.last.skipped == 1);

    /* copying a file into its own folder makes a copy next to it */
    r = run(e, FPE_OP_COPY, srcs + 1, 1, g_root, FPE_CONFLICT_OVERWRITE);
    CHECK(exists(L"a (2).txt"));

    /* a folder into itself is refused */
    wchar_t inside[MAX_PATH];
    path_join(inside, g_root, L"sub\\deep");
    r = run(e, FPE_OP_COPY, srcs, 1, inside, FPE_CONFLICT_RENAME);
    CHECK(r.last.errors == 1);
    CHECK(r.last.last_error == ERROR_INVALID_PARAMETER);
    CHECK(!exists(L"sub\\deep\\sub"));

    /* move (same volume rename) */
    wchar_t moved[MAX_PATH], from[MAX_PATH];
    path_join(from, g_root, L"dst\\sub (2)");
    path_join(moved, g_root, L"empty");
    const wchar_t *mv[] = { from };
    r = run(e, FPE_OP_MOVE, mv, 1, moved, FPE_CONFLICT_RENAME);
    CHECK(r.last.errors == 0);
    CHECK(!exists(L"dst\\sub (2)"));
    CHECK(exists(L"empty\\sub (2)\\deep\\inner.bin"));

    /* moving a folder onto an existing one merges it */
    make_dir(L"m1");
    make_dir(L"m1\\sub");
    make_file(L"m1\\sub\\extra.txt", 4, 0);
    make_file(L"m1\\sub\\note.md", 3, 0);
    path_join(from, g_root, L"m1\\sub");
    r = run(e, FPE_OP_MOVE, mv, 1, dst, FPE_CONFLICT_OVERWRITE);
    CHECK(r.last.errors == 0);
    CHECK(!exists(L"m1\\sub"));
    CHECK(file_size(L"dst\\sub\\extra.txt") == 4);
    CHECK(file_size(L"dst\\sub\\note.md") == 3);
    CHECK(exists(L"dst\\sub\\deep\\inner.bin"));

    /* delete removes a whole tree, including read-only files */
    make_file(L"dst\\sub\\ro.txt", 1, FILE_ATTRIBUTE_READONLY);
    const wchar_t *del[] = { dst };
    r = run(e, FPE_OP_DELETE, del, 1, NULL, FPE_CONFLICT_RENAME);
    CHECK(r.last.state == FPE_JOB_DONE);
    CHECK(r.last.errors == 0);
    CHECK(!exists(L"dst"));

    /* a junction inside a deleted folder is unlinked, its target survives */
    make_dir(L"jtest");
    wchar_t link[MAX_PATH], target[MAX_PATH], cmd[3 * MAX_PATH];
    path_join(link, g_root, L"jtest\\link");
    path_join(target, g_root, L"sub");
    _snwprintf(cmd, 3 * MAX_PATH, L"cmd /c mklink /J \"%ls\" \"%ls\" >nul", link, target);
    _wsystem(cmd);
    if (exists(L"jtest\\link\\note.md")) {
        wchar_t jt[MAX_PATH];
        path_join(jt, g_root, L"jtest");
        const wchar_t *dj[] = { jt };
        r = run(e, FPE_OP_DELETE, dj, 1, NULL, FPE_CONFLICT_RENAME);
        CHECK(r.last.errors == 0);
        CHECK(!exists(L"jtest"));
        CHECK(exists(L"sub\\note.md"));
        CHECK(exists(L"sub\\deep\\inner.bin"));
    } else {
        printf("  (junction test skipped: mklink unavailable)\n");
    }

    /* invalid submissions are rejected up front */
    CHECK(fpe_job_submit(e, FPE_OP_COPY, srcs, 1, NULL, FPE_CONFLICT_RENAME, NULL, NULL) == 0);
    CHECK(fpe_job_submit(e, FPE_OP_DELETE, srcs, 0, NULL, FPE_CONFLICT_RENAME, NULL, NULL) == 0);
}

/* Windows transactional overwrite and no-follow merge contracts. All files,
 * links, and outside-selection sentinels belong to this run's temp tree. */
static int win_test_junction(const wchar_t *link,const wchar_t *target)
{
    wchar_t command[3*MAX_PATH];_snwprintf(command,3*MAX_PATH,L"cmd.exe /c mklink /J \"%ls\" \"%ls\" >nul",link,target);
    return _wsystem(command)==0;
}
static int win_test_retarget_junction(const wchar_t *link,const wchar_t *target)
{
    struct {DWORD tag;WORD length,reserved,suboff,sublen,printoff,printlen;wchar_t path[2*MAX_PATH+8];} data={0};
    data.tag=IO_REPARSE_TAG_MOUNT_POINT;_snwprintf(data.path,2*MAX_PATH+8,L"\\??\\%ls",target);
    data.sublen=(WORD)(wcslen(data.path)*sizeof(wchar_t));data.printoff=data.sublen+sizeof(wchar_t);
    wcscpy((wchar_t*)((unsigned char*)data.path+data.printoff),target);data.printlen=(WORD)(wcslen(target)*sizeof(wchar_t));data.length=8+data.printoff+data.printlen+sizeof(wchar_t);
    HANDLE h=CreateFileW(link,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(h==INVALID_HANDLE_VALUE)return 0;DWORD done;int ok=DeviceIoControl(h,FSCTL_SET_REPARSE_POINT,&data,data.length+8,NULL,0,&done,NULL);CloseHandle(h);return ok;
}
static int win_test_contents(const wchar_t *path,uint64_t bytes,unsigned char expected)
{
    HANDLE h=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    if(h==INVALID_HANDLE_VALUE)return 0;LARGE_INTEGER size;int ok=GetFileSizeEx(h,&size)&&(uint64_t)size.QuadPart==bytes;
    unsigned char data[65536];DWORD got;
    while(ok&&bytes){DWORD request=bytes>sizeof data?sizeof data:(DWORD)bytes;if(!ReadFile(h,data,request,&got,NULL)||got!=request){ok=0;break;}
        for(DWORD i=0;i<got;i++)if(data[i]!=expected){ok=0;break;}bytes-=got;}
    CloseHandle(h);return ok;
}
static int win_test_fill(const wchar_t *path,unsigned char byte,DWORD bytes)
{
    HANDLE h=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);if(h==INVALID_HANDLE_VALUE)return 0;
    unsigned char data[256];memset(data,byte,sizeof data);DWORD done=0;int ok=bytes<=sizeof data&&WriteFile(h,data,bytes,&done,NULL)&&done==bytes;CloseHandle(h);return ok;
}

static int operand_file_id(const wchar_t *path,BY_HANDLE_FILE_INFORMATION *info)
{
    HANDLE h=CreateFileW(path,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(h==INVALID_HANDLE_VALUE)return 0;
    int ok=GetFileInformationByHandle(h,info);CloseHandle(h);return ok;
}
static void test_windows_job_operands_refuse_literal_suffix_aliases(fpe_engine *engine)
{
    puts("ordinary Windows jobs refuse ambiguous operands before queueing");
    wchar_t base[512],dest[512],dotdest[512],spacedest[512],branch[512];
    _snwprintf(base,512,L"\\\\?\\%ls\\operand-source",g_root);
    _snwprintf(dest,512,L"\\\\?\\%ls\\operand-dest",g_root);
    _snwprintf(dotdest,512,L"\\\\?\\%ls\\operand-dest.",g_root);
    _snwprintf(spacedest,512,L"\\\\?\\%ls\\operand-dest ",g_root);
    _snwprintf(branch,512,L"%ls\\branch.",base);
    CHECK(CreateDirectoryW(base,NULL)&&CreateDirectoryW(dest,NULL)&&CreateDirectoryW(dotdest,NULL)&&
        CreateDirectoryW(spacedest,NULL)&&CreateDirectoryW(branch,NULL));
    wchar_t files[4][600];const wchar_t *names[]={L"report",L"report.",L"report ",L"branch.\\leaf"};
    BY_HANDLE_FILE_INFORMATION before[4]={0},after={0};
    for(unsigned i=0;i<4;i++){
        _snwprintf(files[i],600,L"%ls\\%ls",base,names[i]);
        CHECK(win_test_fill(files[i],(unsigned char)('a'+i),i+1));
        CHECK(operand_file_id(files[i],&before[i]));
    }
    for(unsigned i=1;i<4;i++)for(unsigned spelling=0;spelling<3;spelling++){
        wchar_t input[600];wcscpy(input,spelling?files[i]+4:files[i]);
        if(spelling==2)for(wchar_t *p=input;*p;p++)if(*p==L'\\')*p=L'/';
        const wchar_t *one[]={input};
        for(fpe_op op=FPE_OP_COPY;op<=FPE_OP_RECYCLE;op++){
            job_result result={0};result.ev=test_callback_event(FALSE);SetLastError(0);
            uint64_t id=fpe_job_submit(engine,op,one,1,op<=FPE_OP_MOVE?dest:NULL,FPE_CONFLICT_FAIL,on_job,&result);
            DWORD error=GetLastError();CHECK(!id&&error==ERROR_INVALID_NAME);
            if(id)CHECK(test_wait_job(engine,id,result.ev,30000)==WAIT_OBJECT_0);
            CHECK(!result.finals);CloseHandle(result.ev);
        }
    }
    const wchar_t *mixed[]={files[0],files[1]};
    SetLastError(0);CHECK(!fpe_job_submit(engine,FPE_OP_COPY,mixed,2,dest,FPE_CONFLICT_FAIL,NULL,NULL)&&GetLastError()==ERROR_INVALID_NAME);
    const wchar_t *one[]={files[0]},*destinations[]={dotdest,spacedest};
    for(unsigned i=0;i<2;i++){
        SetLastError(0);CHECK(!fpe_job_submit(engine,FPE_OP_COPY,one,1,destinations[i],FPE_CONFLICT_FAIL,NULL,NULL)&&GetLastError()==ERROR_INVALID_NAME);
        const wchar_t *relative[]={L"report"};SetLastError(0);
        CHECK(!fpe_job_submit_tree(engine,base,relative,1,destinations[i],FPE_CONFLICT_FAIL,NULL,NULL,NULL)&&GetLastError()==ERROR_INVALID_NAME);
    }
    for(unsigned i=0;i<4;i++){
        CHECK(operand_file_id(files[i],&after)&&before[i].dwVolumeSerialNumber==after.dwVolumeSerialNumber&&
            before[i].nFileIndexHigh==after.nFileIndexHigh&&before[i].nFileIndexLow==after.nFileIndexLow);
        CHECK(win_test_contents(files[i],i+1,(unsigned char)('a'+i)));
    }
    /* Ordinary names and explicit relative navigation remain supported. */
    wchar_t navigation[600];_snwprintf(navigation,600,L"%ls\\.\\..\\operand-source\\report",base);
    one[0]=navigation;job_result copied=run(engine,FPE_OP_COPY,one,1,dest,FPE_CONFLICT_FAIL);
    wchar_t target[600];_snwprintf(target,600,L"%ls\\report",dest);
    CHECK(copied.finals==1&&!copied.last.errors&&win_test_contents(target,1,'a'));
    CHECK(DeleteFileW(target));
    for(unsigned i=0;i<4;i++)CHECK(DeleteFileW(files[i]));
    CHECK(RemoveDirectoryW(branch)&&RemoveDirectoryW(base)&&RemoveDirectoryW(dest)&&RemoveDirectoryW(dotdest)&&RemoveDirectoryW(spacedest));
}
static int win_test_dacl(const wchar_t *path,unsigned char *storage,DWORD cap)
{DWORD needed=0;return GetFileSecurityW(path,DACL_SECURITY_INFORMATION,(PSECURITY_DESCRIPTOR)storage,cap,&needed)!=0;}
static int win_test_same_dacl(unsigned char *a,unsigned char *b)
{
    PACL x=NULL,y=NULL;BOOL xp,yp,xd,yd;SECURITY_DESCRIPTOR_CONTROL xc,yc;DWORD rev;
    return GetSecurityDescriptorDacl(a,&xp,&x,&xd)&&GetSecurityDescriptorDacl(b,&yp,&y,&yd)&&xp&&yp&&x&&y&&
        GetSecurityDescriptorControl(a,&xc,&rev)&&GetSecurityDescriptorControl(b,&yc,&rev)&&
        (xc&SE_DACL_PROTECTED)==(yc&SE_DACL_PROTECTED)&&x->AclSize==y->AclSize&&!memcmp(x,y,x->AclSize);
}
static int win_test_basic(const wchar_t *path,FILE_BASIC_INFO *basic)
{
    HANDLE h=CreateFileW(path,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,NULL);
    int ok=h!=INVALID_HANDLE_VALUE&&GetFileInformationByHandleEx(h,FileBasicInfo,basic,sizeof *basic);if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);return ok;
}
typedef struct win_job_test {
    job_result result;fpe_engine *engine;int action,acted,ok,blocked_source,blocked_dest,temp_blocked;
    uint64_t positive;HANDLE lock;
    const wchar_t *source,*dest,*saved,*outside;
} win_job_test;
static void win_test_progress(void *user,const fpe_job_progress *p)
{
    win_job_test *v=user;
    if(p->state==FPE_JOB_RUNNING){
        if(v->action==5&&!v->acted){v->acted=1;v->ok=win_test_retarget_junction(v->source,v->outside);}
        else if(v->action==3&&!v->acted){
            v->acted=1;v->ok=MoveFileExW(v->source,v->saved,0)&&win_test_junction(v->source,v->outside);
        }else if((v->action==1||v->action==2)&&!v->acted){
            if(!p->bytes_done)Sleep(70); /* Cross the existing50ms reporting throttle; never used as a latency oracle. */
            else if(p->bytes_done<p->bytes_total){
                v->acted=1;v->positive=p->bytes_done;
                if(v->action==1){
                    wchar_t pattern[MAX_PATH],temp[MAX_PATH],renamed[MAX_PATH];wcscpy(pattern,v->dest);wchar_t *slash=wcsrchr(pattern,L'\\');
                    if(slash){slash[1]=0;wcscpy(temp,pattern);wcscat(pattern,L".openpilot-copy-*");WIN32_FIND_DATAW found;HANDLE scan=FindFirstFileW(pattern,&found);
                        if(scan!=INVALID_HANDLE_VALUE){FindClose(scan);wcscat(temp,found.cFileName);_snwprintf(renamed,MAX_PATH,L"%ls-held",temp);int moved=MoveFileExW(temp,renamed,0);v->temp_blocked=!moved;if(moved)MoveFileExW(renamed,temp,0);}}
                    fpe_job_cancel(v->engine,p->job);v->ok=1;
                }
                else{v->lock=CreateFileW(v->dest,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);v->ok=v->lock!=INVALID_HANDLE_VALUE;}
            }
        }
    }
    on_job(&v->result,p);
}
static void win_test_event(void *user,const fpe_job_event *event)
{
    win_job_test *v=user;if(event->kind==FPE_JOB_ERROR)printf("  Windows job error %u: %ls\n",event->error,event->path?event->path:L"(none)");if(v->action!=4||v->acted||event->kind!=FPE_JOB_SKIPPED)return;v->acted=1;
    int moved=MoveFileExW(v->source,v->saved,0);v->blocked_source=!moved;if(moved)MoveFileExW(v->saved,v->source,0);
    wchar_t saved[MAX_PATH];_snwprintf(saved,MAX_PATH,L"%ls-held",v->dest);
    moved=MoveFileExW(v->dest,saved,0);v->blocked_dest=!moved;if(moved)MoveFileExW(saved,v->dest,0);
}
static void win_test_run(win_job_test *v,fpe_op op,const wchar_t *source,const wchar_t *dest,fpe_conflict policy)
{
    v->result.ev=test_callback_event(FALSE);const wchar_t *sources[]={source};
    uint64_t id=fpe_job_submit_ex(v->engine,op,sources,1,dest,policy,win_test_progress,win_test_event,v);
    CHECK(id!=0);CHECK(test_wait_job(v->engine,id,v->result.ev,15000)==WAIT_OBJECT_0);
    Sleep(5);CloseHandle(v->result.ev);v->result.ev=NULL;
}
static void test_windows_overwrite_integrity(fpe_engine *engine)
{
    puts("Windows overwrite: positive-progress cancel, denied commit, streams and metadata");
    make_dir(L"win_overwrite");make_dir(L"win_overwrite\\source");make_dir(L"win_overwrite\\target");
    make_file(L"win_overwrite\\source\\selected.bin",64u*1024u*1024u,0);
    make_file(L"win_overwrite\\source\\selected.bin:proof",13,0);
    make_file(L"win_overwrite\\target\\selected.bin",7,0);make_file(L"win_overwrite\\target\\selected.bin:prior",19,0);
    wchar_t source[MAX_PATH],destdir[MAX_PATH],dest[MAX_PATH],ads[MAX_PATH];
    path_join(source,g_root,L"win_overwrite\\source\\selected.bin");path_join(destdir,g_root,L"win_overwrite\\target");path_join(dest,destdir,L"selected.bin");
    CHECK(win_test_fill(dest,'o',7));_snwprintf(ads,MAX_PATH,L"%ls:prior",dest);CHECK(win_test_fill(ads,'p',19));
    PSECURITY_DESCRIPTOR sd=NULL;CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;OW)",SDDL_REVISION_1,&sd,NULL));
    CHECK(sd&&SetFileSecurityW(dest,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,sd));if(sd)LocalFree(sd);
    unsigned char old_dacl[2048],new_dacl[2048];CHECK(win_test_dacl(dest,old_dacl,sizeof old_dacl));
    CHECK(SetFileAttributesW(dest,FILE_ATTRIBUTE_READONLY|FILE_ATTRIBUTE_HIDDEN));FILE_BASIC_INFO before,after;
    CHECK(win_test_basic(dest,&before));
    win_job_test v={.engine=engine,.action=1,.dest=dest};win_test_run(&v,FPE_OP_COPY,source,destdir,FPE_CONFLICT_OVERWRITE);
    CHECK(v.acted&&v.ok&&v.positive>0&&v.positive<64u*1024u*1024u);CHECK(v.temp_blocked);CHECK(v.result.finals==1&&v.result.last.state==FPE_JOB_CANCELLED&&v.result.last.errors==0);
    CHECK(win_test_contents(source,64u*1024u*1024u,'x'));CHECK(win_test_contents(dest,7,'o'));
    _snwprintf(ads,MAX_PATH,L"%ls:prior",dest);CHECK(win_test_contents(ads,19,'p'));
    CHECK(win_test_basic(dest,&after)&&before.FileAttributes==after.FileAttributes&&before.CreationTime.QuadPart==after.CreationTime.QuadPart&&before.LastWriteTime.QuadPart==after.LastWriteTime.QuadPart&&before.ChangeTime.QuadPart==after.ChangeTime.QuadPart);
    CHECK(SetFileAttributesW(dest,FILE_ATTRIBUTE_READONLY));CHECK(win_test_basic(dest,&before));
    v=(win_job_test){.engine=engine,.action=2,.dest=dest};win_test_run(&v,FPE_OP_COPY,source,destdir,FPE_CONFLICT_OVERWRITE);
    CHECK(v.acted&&v.ok&&v.positive>0);CHECK(v.result.finals==1&&v.result.last.errors==1);if(v.lock&&v.lock!=INVALID_HANDLE_VALUE)CloseHandle(v.lock);
    CHECK(win_test_contents(source,64u*1024u*1024u,'x')&&win_test_contents(dest,7,'o'));CHECK(win_test_contents(ads,19,'p'));
    CHECK(win_test_basic(dest,&after)&&before.FileAttributes==after.FileAttributes&&before.LastWriteTime.QuadPart==after.LastWriteTime.QuadPart&&before.ChangeTime.QuadPart==after.ChangeTime.QuadPart);
    const wchar_t *sources[]={source};job_result result=run(engine,FPE_OP_COPY,sources,1,destdir,FPE_CONFLICT_OVERWRITE);
    if(result.last.errors)printf("  overwrite completion error %u\n",result.last.last_error);CHECK(result.finals==1&&!result.last.errors);CHECK(win_test_contents(dest,64u*1024u*1024u,'x'));
    CHECK(win_test_dacl(dest,new_dacl,sizeof new_dacl)&&win_test_same_dacl(old_dacl,new_dacl));
    _snwprintf(ads,MAX_PATH,L"%ls:proof",dest);CHECK(win_test_contents(ads,13,'x'));
    FILE_BASIC_INFO expected;CHECK(win_test_basic(source,&expected)&&win_test_basic(dest,&after)&&expected.LastWriteTime.QuadPart==after.LastWriteTime.QuadPart&&expected.FileAttributes==after.FileAttributes);
    result=run(engine,FPE_OP_COPY,sources,1,destdir,FPE_CONFLICT_OVERWRITE);CHECK(result.finals==1&&!result.last.errors&&win_test_contents(dest,64u*1024u*1024u,'x'));
    CHECK(win_test_dacl(dest,new_dacl,sizeof new_dacl)&&win_test_same_dacl(old_dacl,new_dacl));
    HANDLE busy=CreateFileW(dest,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);CHECK(busy!=INVALID_HANDLE_VALUE);
    result=run(engine,FPE_OP_COPY,sources,1,destdir,FPE_CONFLICT_OVERWRITE);CHECK(result.last.errors==1&&win_test_contents(dest,64u*1024u*1024u,'x'));if(busy!=INVALID_HANDLE_VALUE)CloseHandle(busy);
    make_file(L"win_overwrite\\source\\fast",7,0);make_file(L"win_overwrite\\target\\fast",3,0);
    path_join(source,g_root,L"win_overwrite\\source\\fast");sources[0]=source;path_join(dest,destdir,L"fast");CHECK(win_test_fill(dest,'o',3));
    busy=CreateFileW(dest,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);CHECK(busy!=INVALID_HANDLE_VALUE);
    result=run(engine,FPE_OP_MOVE,sources,1,destdir,FPE_CONFLICT_OVERWRITE);CHECK(result.last.errors==1&&win_test_contents(source,7,'x')&&win_test_contents(dest,3,'o'));if(busy!=INVALID_HANDLE_VALUE)CloseHandle(busy);
    result=run(engine,FPE_OP_MOVE,sources,1,destdir,FPE_CONFLICT_OVERWRITE);CHECK(!result.last.errors&&GetFileAttributesW(source)==INVALID_FILE_ATTRIBUTES&&win_test_contents(dest,7,'x'));
    make_file(L"win_overwrite\\source\\empty",0,0);path_join(source,g_root,L"win_overwrite\\source\\empty");sources[0]=source;
    result=run(engine,FPE_OP_COPY,sources,1,destdir,FPE_CONFLICT_FAIL);path_join(dest,destdir,L"empty");if(result.last.errors)printf("  empty completion error %u\n",result.last.last_error);CHECK(!result.last.errors&&win_test_contents(dest,0,0));
    wchar_t pattern[MAX_PATH];path_join(pattern,destdir,L".openpilot-copy-*");WIN32_FIND_DATAW found;HANDLE h=FindFirstFileW(pattern,&found);CHECK(h==INVALID_HANDLE_VALUE);if(h!=INVALID_HANDLE_VALUE)FindClose(h);
}
static void test_windows_junction_merge_integrity(fpe_engine *engine)
{
    puts("Windows merges: source and destination junctions remain leaves, ordinary merges still finish");make_dir(L"win_merge");
    for(unsigned op=0;op<2;op++)for(unsigned policy=1;policy<=2;policy++)for(unsigned kind=0;kind<4;kind++){
        wchar_t rel[160],root[MAX_PATH],srcdir[MAX_PATH],dstdir[MAX_PATH],source[MAX_PATH],target[MAX_PATH],outside[MAX_PATH],outdest[MAX_PATH],payload[MAX_PATH],marker[MAX_PATH];
        _snwprintf(rel,160,L"win_merge\\case-%u-%u-%u",op,policy,kind);make_dir(rel);path_join(root,g_root,rel);
        path_join(srcdir,root,L"source");path_join(dstdir,root,L"target");path_join(outside,root,L"outside-source");path_join(outdest,root,L"outside-target");
        CHECK(CreateDirectoryW(srcdir,NULL)&&CreateDirectoryW(dstdir,NULL)&&CreateDirectoryW(outside,NULL)&&CreateDirectoryW(outdest,NULL));
        _snwprintf(rel,160,L"win_merge\\case-%u-%u-%u\\outside-source\\payload",op,policy,kind);make_file(rel,23,0);
        _snwprintf(rel,160,L"win_merge\\case-%u-%u-%u\\outside-target\\marker",op,policy,kind);make_file(rel,31,0);
        path_join(source,srcdir,L"branch");path_join(target,dstdir,L"branch");
        if(kind&1)CHECK(win_test_junction(source,outside));else{CHECK(CreateDirectoryW(source,NULL));_snwprintf(rel,160,L"win_merge\\case-%u-%u-%u\\source\\branch\\payload",op,policy,kind);make_file(rel,13,0);}
        if(kind&2)CHECK(win_test_junction(target,outdest));else CHECK(CreateDirectoryW(target,NULL));
        const wchar_t *items[]={source};job_result result=run(engine,op?FPE_OP_MOVE:FPE_OP_COPY,items,1,dstdir,(fpe_conflict)policy);
        CHECK(result.finals==1&&result.last.state==FPE_JOB_DONE);
        path_join(payload,outside,L"payload");CHECK(win_test_contents(payload,23,'x'));path_join(marker,outdest,L"marker");CHECK(win_test_contents(marker,31,'x'));
        path_join(payload,outdest,L"payload");CHECK(GetFileAttributesW(payload)==INVALID_FILE_ATTRIBUTES);
        if(kind){
            CHECK((kind&1)?result.last.skipped==1&&result.last.errors==0:result.last.errors==1);
            CHECK(GetFileAttributesW(source)!=INVALID_FILE_ATTRIBUTES);
            if(!(kind&1)){path_join(payload,source,L"payload");CHECK(win_test_contents(payload,13,'x'));}
            if(!(kind&2)){path_join(payload,target,L"payload");CHECK(GetFileAttributesW(payload)==INVALID_FILE_ATTRIBUTES);}
        }else{
            if(result.last.errors)printf("  merge op=%u policy=%u error=%u\n",op,policy,result.last.last_error);CHECK(!result.last.errors&&!result.last.skipped);path_join(payload,target,L"payload");CHECK(win_test_contents(payload,13,'x'));
            CHECK(op?GetFileAttributesW(source)==INVALID_FILE_ATTRIBUTES:GetFileAttributesW(source)!=INVALID_FILE_ATTRIBUTES);
        }
    }
    make_dir(L"win_merge\\leaf-source");make_dir(L"win_merge\\leaf-target");make_dir(L"win_merge\\leaf-outside");make_file(L"win_merge\\leaf-outside\\sentinel",17,0);
    wchar_t source[MAX_PATH],outside[MAX_PATH],target[MAX_PATH],link[MAX_PATH],sentinel[MAX_PATH];
    path_join(source,g_root,L"win_merge\\leaf-source\\junction");path_join(outside,g_root,L"win_merge\\leaf-outside");path_join(target,g_root,L"win_merge\\leaf-target");CHECK(win_test_junction(source,outside));
    const wchar_t *items[]={source};job_result leaf=run(engine,FPE_OP_MOVE,items,1,target,FPE_CONFLICT_FAIL);CHECK(!leaf.last.errors&&!leaf.last.skipped&&leaf.finals==1);
    path_join(link,target,L"junction");CHECK((GetFileAttributesW(link)&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))==(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT));
    CHECK(GetFileAttributesW(source)==INVALID_FILE_ATTRIBUTES);path_join(sentinel,outside,L"sentinel");CHECK(win_test_contents(sentinel,17,'x'));
}
static void test_windows_junction_replacement(fpe_engine *engine)
{
    puts("Windows merges: late junction replacement refused, active source/destination namespaces pinned");make_dir(L"win_late");
    for(unsigned op=0;op<2;op++){
        wchar_t rel[160],root[MAX_PATH],srcdir[MAX_PATH],dstdir[MAX_PATH],source[MAX_PATH],target[MAX_PATH],kept[MAX_PATH],outside[MAX_PATH],payload[MAX_PATH];
        _snwprintf(rel,160,L"win_late\\case-%u",op);make_dir(rel);path_join(root,g_root,rel);
        path_join(srcdir,root,L"source");path_join(dstdir,root,L"target");path_join(outside,root,L"outside");CHECK(CreateDirectoryW(srcdir,NULL)&&CreateDirectoryW(dstdir,NULL)&&CreateDirectoryW(outside,NULL));
        path_join(source,srcdir,L"branch");path_join(target,dstdir,L"branch");path_join(kept,srcdir,L"kept");CHECK(CreateDirectoryW(source,NULL)&&CreateDirectoryW(target,NULL));
        _snwprintf(rel,160,L"win_late\\case-%u\\source\\branch\\payload",op);make_file(rel,13,0);
        _snwprintf(rel,160,L"win_late\\case-%u\\outside\\payload",op);make_file(rel,29,0);
        win_job_test v={.engine=engine,.action=3,.source=source,.dest=target,.saved=kept,.outside=outside};win_test_run(&v,op?FPE_OP_MOVE:FPE_OP_COPY,source,dstdir,FPE_CONFLICT_OVERWRITE);
        CHECK(v.acted&&v.ok);CHECK(v.result.finals==1&&!v.result.last.errors&&v.result.last.skipped==1);
        path_join(payload,kept,L"payload");CHECK(win_test_contents(payload,13,'x'));path_join(payload,outside,L"payload");CHECK(win_test_contents(payload,29,'x'));
        path_join(payload,target,L"payload");CHECK(GetFileAttributesW(payload)==INVALID_FILE_ATTRIBUTES);CHECK(RemoveDirectoryW(source));CHECK(MoveFileExW(kept,source,0));
        _snwprintf(rel,160,L"win_late\\case-%u\\target\\branch\\payload",op);make_file(rel,7,0);
        v=(win_job_test){.engine=engine,.action=4,.source=source,.dest=target,.saved=kept};win_test_run(&v,op?FPE_OP_MOVE:FPE_OP_COPY,source,dstdir,FPE_CONFLICT_SKIP);
        if(!v.acted||!v.blocked_source||!v.blocked_dest)printf("  pin op=%u acted=%d src_blocked=%d dst_blocked=%d\n",op,v.acted,v.blocked_source,v.blocked_dest);CHECK(v.acted&&v.blocked_source&&v.blocked_dest);CHECK(v.result.finals==1&&!v.result.last.errors&&v.result.last.skipped==1);
        path_join(payload,source,L"payload");CHECK(win_test_contents(payload,13,'x'));path_join(payload,target,L"payload");CHECK(win_test_contents(payload,7,'x'));
    }
    /* An explicitly selected root alias resolves once; rebinding its link
     * after RUNNING must not redirect the accepted destination. */
    for(unsigned op=0;op<2;op++){
        wchar_t rel[160],source[MAX_PATH],alias[MAX_PATH],physical[MAX_PATH],outside[MAX_PATH],saved[MAX_PATH],output[MAX_PATH];
        _snwprintf(rel,160,L"win_late\\alias-source-%u",op);make_file(rel,11,0);path_join(source,g_root,rel);
        _snwprintf(rel,160,L"win_late\\physical-%u",op);make_dir(rel);path_join(physical,g_root,rel);
        _snwprintf(rel,160,L"win_late\\outside-%u",op);make_dir(rel);path_join(outside,g_root,rel);
        _snwprintf(rel,160,L"win_late\\alias-%u",op);path_join(alias,g_root,rel);_snwprintf(saved,MAX_PATH,L"%ls-kept",alias);CHECK(win_test_junction(alias,physical));
        win_job_test v={.engine=engine,.action=5,.source=alias,.saved=saved,.outside=outside};win_test_run(&v,op?FPE_OP_MOVE:FPE_OP_COPY,source,alias,FPE_CONFLICT_FAIL);
        CHECK(v.acted&&v.ok&&v.result.finals==1&&!v.result.last.errors);
        _snwprintf(rel,160,L"alias-source-%u",op);path_join(output,physical,rel);CHECK(win_test_contents(output,11,'x'));
        path_join(output,outside,rel);CHECK(GetFileAttributesW(output)==INVALID_FILE_ATTRIBUTES);
    }

}

/* Engine diagnostics and cancellation: real filesystem outcomes, including
 * a partial-success job and cancellation before any destination is written. */
typedef struct diagnostic_result {
    job_result result;
    fpe_engine *engine;
    int cancel_running, events;
    uint32_t kind, error;
    wchar_t path[MAX_PATH];
} diagnostic_result;
static void on_diagnostic(void *user, const fpe_job_event *v)
{
    diagnostic_result *r = user;
    r->events++; r->kind = v->kind; r->error = v->error;
    _snwprintf(r->path, MAX_PATH, L"%ls", v->path ? v->path : L""); r->path[MAX_PATH-1] = 0;
}
static void on_diagnostic_progress(void *user, const fpe_job_progress *v)
{
    diagnostic_result *r = user;
    if (r->cancel_running && v->state == FPE_JOB_RUNNING) fpe_job_cancel(r->engine, v->job);
    on_job(&r->result, v);
}
typedef struct blocked_job {
    job_result result;
    HANDLE entered, release;
} blocked_job;
static void on_blocked_job(void *user, const fpe_job_progress *v)
{
    blocked_job *b = user;
    if (v->state == FPE_JOB_RUNNING) {
        SetEvent(b->entered);
        if (WaitForSingleObject(b->release, 30000) != WAIT_OBJECT_0)
            test_wait_abort("queue blocker was not released", WAIT_TIMEOUT);
    }
    on_job(&b->result, v);
}
static void wait_blocked_job(fpe_engine *engine, uint64_t id, blocked_job *b)
{
    DWORD wait = id ? WaitForSingleObject(b->entered, 30000) : WAIT_FAILED;
    CHECK(wait == WAIT_OBJECT_0);
    if (wait != WAIT_OBJECT_0) {
        SetEvent(b->release);
        if (id) test_wait_job(engine, id, b->result.ev, 0);
        test_wait_abort("queue blocker did not enter", wait);
    }
}
static void test_job_diagnostics(fpe_engine *engine)
{
    printf("job diagnostics and cancellation\n");
    wchar_t a[MAX_PATH], missing[MAX_PATH], dest[MAX_PATH];
    path_join(a, g_root, L"B.TXT"); path_join(missing, g_root, L"missing.bin");
    path_join(dest, g_root, L"diagnostics"); CreateDirectoryW(dest, NULL);
    const wchar_t *sources[] = { missing, a };
    diagnostic_result r = { 0 }; r.engine = engine;
    r.result.ev = test_callback_event(FALSE);
    uint64_t id = fpe_job_submit_ex(engine, FPE_OP_COPY, sources, 2, dest, FPE_CONFLICT_RENAME,
                                   on_diagnostic_progress, on_diagnostic, &r);
    CHECK(test_wait_job(engine, id, r.result.ev, 30000) == WAIT_OBJECT_0);
    CHECK(r.result.finals == 1 && r.result.last.state == FPE_JOB_DONE);
    CHECK(r.events == 1 && r.kind == FPE_JOB_ERROR && r.error == ERROR_FILE_NOT_FOUND);
    CHECK(!wcscmp(r.path, missing)); CHECK(file_size(L"diagnostics\\B.TXT") == 2000);
    r.events = r.result.finals = 0;
    id = fpe_job_submit_ex(engine, FPE_OP_COPY, sources + 1, 1, dest, FPE_CONFLICT_SKIP,
                           on_diagnostic_progress, on_diagnostic, &r);
    CHECK(test_wait_job(engine, id, r.result.ev, 30000) == WAIT_OBJECT_0);
    CHECK(r.result.last.skipped == 1 && r.result.last.errors == 0);
    CHECK(r.events == 1 && r.kind == FPE_JOB_SKIPPED && r.error == ERROR_FILE_EXISTS);
    wchar_t target[MAX_PATH]; path_join(target, dest, L"B.TXT"); CHECK(!wcscmp(r.path, target));
    r.events = r.result.finals = 0; r.cancel_running = 1;
    id = fpe_job_submit_ex(engine, FPE_OP_COPY, sources + 1, 1, dest, FPE_CONFLICT_RENAME,
                           on_diagnostic_progress, on_diagnostic, &r);
    CHECK(test_wait_job(engine, id, r.result.ev, 30000) == WAIT_OBJECT_0);
    CHECK(r.result.finals == 1 && r.result.last.state == FPE_JOB_CANCELLED);
    CHECK(r.result.last.items_done == 0 && !exists(L"diagnostics\\B (2).TXT"));
    CHECK(r.events == 0);
    r.cancel_running = 0; r.result.finals = 0;
    id = fpe_job_submit_ex(engine, FPE_OP_COPY, sources + 1, 1, missing, FPE_CONFLICT_RENAME,
                           on_diagnostic_progress, on_diagnostic, &r);
    CHECK(test_wait_job(engine, id, r.result.ev, 30000) == WAIT_OBJECT_0);
    CHECK(r.result.finals == 1 && r.result.last.state == FPE_JOB_FAILED);
    CHECK(r.events == 1 && !wcscmp(r.path, missing));
    /* Freeze the first worker callback so the second job is definitely queued. */
    blocked_job b = { 0 };
    b.result.ev = test_callback_event(FALSE);
    b.entered = test_callback_event(TRUE);
    b.release = test_callback_event(TRUE);
    uint64_t first = fpe_job_submit(engine, FPE_OP_COPY, sources + 1, 1, dest, FPE_CONFLICT_RENAME, on_blocked_job, &b);
    wait_blocked_job(engine, first, &b);
    r.result.finals = 0;
    uint64_t queued = fpe_job_submit(engine, FPE_OP_COPY, sources + 1, 1, dest, FPE_CONFLICT_RENAME, on_diagnostic_progress, &r);
    CHECK(queued != 0); if (queued) fpe_job_cancel(engine, queued); SetEvent(b.release);
    CHECK(test_wait_job(engine, first, b.result.ev, 30000) == WAIT_OBJECT_0);
    CHECK(test_wait_job(engine, queued, r.result.ev, 30000) == WAIT_OBJECT_0);
    CHECK(b.result.last.state == FPE_JOB_DONE && r.result.last.state == FPE_JOB_CANCELLED);
    CHECK(r.result.finals == 1 && !exists(L"diagnostics\\B (3).TXT"));
    CloseHandle(b.result.ev); CloseHandle(b.entered); CloseHandle(b.release);
    CloseHandle(r.result.ev);
}

/* ---------------------------------------------------------------------- */

static void setup(void)
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    _snwprintf(g_root, MAX_PATH, L"%lsfpe_test_%lu", tmp, GetCurrentProcessId());
    CreateDirectoryW(g_root, NULL);

    make_file(L"a.txt", 100, 0);
    make_file(L"B.TXT", 2000, 0);
    make_file(L"file10.log", 5, 0);
    make_file(L"file2.log", 7, 0);
    make_file(L"hidden.txt", 1, FILE_ATTRIBUTE_HIDDEN);
    make_file(L"readme.md", 3, 0);
    make_dir(L"sub");
    make_dir(L"sub\\deep");
    make_file(L"sub\\deep\\inner.bin", 4096, 0);
    make_file(L"sub\\note.md", 10, 0);
    make_dir(L"empty");
}

/* ---- WSL ---------------------------------------------------------------- */

/* Runs `sh -c script` in the default WSL distro; its stdout, trimmed, in out.
 * 0 when WSL can't run it. The script may not contain double quotes. */
static int wsl_sh_as(int root, const char *script, char *out, DWORD cap)
{
    wchar_t cmd[2048];
    _snwprintf(cmd, 2048, root ? L"wsl.exe -u root --exec sh -c \"%hs\"" :
                    L"wsl.exe --exec sh -c \"%hs\"", script);
    cmd[2047] = 0;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE r, w;
    if (!CreatePipe(&r, &w, &sa, 0)) return 0;
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = { sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = w;
    si.hStdError = w;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(w);
    DWORD k = 0, got;
    while (ok && k + 1 < cap && ReadFile(r, out + k, cap - 1 - k, &got, NULL) && got) k += got;
    CloseHandle(r);
    out[k] = 0;
    while (k && (out[k - 1] == '\n' || out[k - 1] == '\r' || out[k - 1] == ' ')) out[--k] = 0;
    if (!ok) return 0;
    DWORD code = 1;
    WaitForSingleObject(pi.hProcess, 30000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}
static int wsl_sh(const char *script, char *out, DWORD cap)
{
    return wsl_sh_as(0,script,out,cap);
}

static const fpe_entry *find_entry(const fpe_dir *d, const wchar_t *name)
{
    const fpe_entry *e = fpe_dir_entries(d);
    for (uint32_t i = 0; i < fpe_dir_count(d); i++)
        if (!wcscmp(fpe_dir_name(d, &e[i]), name)) return &e[i];
    return NULL;
}

static void test_unix_modes(void)
{
    printf("unix modes\n");
    wchar_t m[11];
    fpe_unix_mode_string(0100754, m);
    CHECK(!wcscmp(m, L"-rwxr-xr--"));
    fpe_unix_mode_string(0104755, m);
    CHECK(!wcscmp(m, L"-rwsr-xr-x"));
    fpe_unix_mode_string(041777, m);
    CHECK(!wcscmp(m, L"drwxrwxrwt"));
    fpe_unix_mode_string(0120777, m);
    CHECK(!wcscmp(m, L"lrwxrwxrwx"));
    fpe_unix_mode_string(010644, m);
    CHECK(!wcscmp(m, L"prw-r--r--"));
    fpe_unix_mode_string(0102644, m);
    CHECK(!wcscmp(m, L"-rw-r-Sr--"));
}

typedef struct native_cancel_result { job_result result; fpe_engine *engine; uint64_t after_bytes; } native_cancel_result;
static void on_native_cancel(void *user,const fpe_job_progress *p)
{
    native_cancel_result *r=user;
    if(p->state==FPE_JOB_RUNNING&&p->bytes_done>=r->after_bytes)fpe_job_cancel(r->engine,p->job);
    on_job(&r->result,p);
}

static void test_wsl_jobs(fpe_engine *engine)
{
    printf("native WSL jobs: metadata, case, Windows bridge and mutations\n");
    DWORD pid=GetCurrentProcessId();char script[2048],distro[256],out[2048];
    snprintf(script,sizeof script,"D=/tmp/openpilot_jobs_%lu; mkdir -p $D/src/Tree $D/dst; printf linux > $D/src/Tree/run; chmod 754 $D/src/Tree/run; ln -s missing $D/src/Tree/broken; printf upper > $D/src/Foo; printf lower > $D/src/foo; echo $WSL_DISTRO_NAME",(unsigned long)pid);
    if(!wsl_sh(script,distro,sizeof distro)||!distro[0]){printf("  (skipped: no WSL distro)\n");return;}
    wchar_t base[MAX_PATH],source[MAX_PATH],dest[MAX_PATH],upper[MAX_PATH],lower[MAX_PATH],file[MAX_PATH],name[MAX_PATH];
    _snwprintf(base,MAX_PATH,L"\\\\wsl.localhost\\%hs\\tmp\\openpilot_jobs_%lu",distro,(unsigned long)pid);
    path_join(source,base,L"src\\Tree");path_join(dest,base,L"dst");
    const wchar_t *paths[]={source};job_result result=run(engine,FPE_OP_COPY,paths,1,dest,FPE_CONFLICT_RENAME);
    CHECK(result.last.state==FPE_JOB_DONE&&result.last.errors==0&&result.last.bytes_done==5);
    fpe_dir *dir=NULL;path_join(file,dest,L"Tree");CHECK(fpe_wsl_read(file,&dir)==0&&dir);
    if(dir){fpe_unix u;const fpe_entry *entry=find_entry(dir,L"run");CHECK(entry&&fpe_dir_unix(dir,entry,&u)&&(u.mode&07777)==0754);
        entry=find_entry(dir,L"broken");CHECK(entry&&fpe_dir_unix(dir,entry,&u)&&u.link&&!wcscmp(u.link,L"missing"));fpe_dir_release(dir);}
    load_result loaded={test_callback_event(FALSE),1,0};
    CHECK(fpe_engine_load(engine,source,on_load,&loaded)!=0);
    CHECK(test_wait_load(loaded.ev,5000)==WAIT_OBJECT_0&&loaded.err==0);
    dir=fpe_engine_cached(engine,source);CHECK(dir!=NULL);fpe_dir_release(dir);
    path_join(file,base,L"src\\tree");CHECK(fpe_engine_cached(engine,file)==NULL);
    CloseHandle(loaded.ev);
    path_join(upper,base,L"src\\Foo");path_join(lower,base,L"src\\foo");
    CHECK(!fpe_path_equal(upper,lower));CHECK(fpe_path_equal(L"C:\\Work",L"c:\\work"));
    const wchar_t *cases[]={upper,lower};result=run(engine,FPE_OP_COPY,cases,2,dest,FPE_CONFLICT_RENAME);
    CHECK(result.last.errors==0);CHECK(fpe_wsl_read(dest,&dir)==0&&dir);
    if(dir){fpe_view *view=fpe_view_create(dir,NULL);CHECK(view!=NULL);
        if(view){uint32_t a=fpe_view_find(view,L"Foo"),b=fpe_view_find(view,L"foo");CHECK(a!=UINT32_MAX&&b!=UINT32_MAX&&a!=b);fpe_view_destroy(view);}fpe_dir_release(dir);}
    fpe_wsl_config nohelper={0,0,60000};fpe_wsl_configure(&nohelper);dir=NULL;
    CHECK(fpe_dir_read(dest,&dir)==0&&dir&&!fpe_dir_is_linux(dir));
    if(dir){fpe_view *view=fpe_view_create(dir,NULL);CHECK(view!=NULL);
        if(view){CHECK(fpe_view_find(view,L"Foo")!=fpe_view_find(view,L"foo"));fpe_view_destroy(view);}fpe_dir_release(dir);}
    fpe_wsl_configure(NULL);
    result=run(engine,FPE_OP_COPY,cases,1,dest,FPE_CONFLICT_SKIP);CHECK(result.last.skipped==1&&result.last.errors==0);
    result=run(engine,FPE_OP_COPY,cases,1,dest,FPE_CONFLICT_OVERWRITE);CHECK(result.last.errors==0);
    native_cancel_result cancelled={.result={.ev=test_callback_event(FALSE)},.engine=engine};
    uint64_t cancel_id=fpe_job_submit(engine,FPE_OP_DELETE,cases,1,NULL,FPE_CONFLICT_FAIL,on_native_cancel,&cancelled);
    CHECK(cancel_id!=0);CHECK(test_wait_job(engine,cancel_id,cancelled.result.ev,5000)==WAIT_OBJECT_0);
    CHECK(cancelled.result.last.state==FPE_JOB_CANCELLED&&cancelled.result.last.errors==0);
    Sleep(5);CloseHandle(cancelled.result.ev);
    snprintf(script,sizeof script,"test -f /tmp/openpilot_jobs_%lu/src/Foo && printf retained",(unsigned long)pid);
    CHECK(wsl_sh(script,out,sizeof out)&&!strcmp(out,"retained"));
    path_join(file,base,L"new");fpe_err mkdirerr=fpe_wsl_create(file,1,NULL);if(mkdirerr)printf("  mkdir error %lu\n",(unsigned long)mkdirerr);CHECK(mkdirerr==0);
    path_join(file,base,L"new\\a\xF03A" L"b");CHECK(fpe_wsl_create(file,0,NULL)==0);CHECK(fpe_wsl_create(file,0,NULL)==ERROR_FILE_EXISTS);
    path_join(name,base,L"new\\Case");CHECK(fpe_wsl_rename(file,name)==0);
    path_join(file,base,L"new\\case");CHECK(fpe_wsl_rename(name,file)==0);
    /* Win32's usual normalization erases this literal Linux filename and
     * would delete the parent. Verify the submitted target remains exact. */
    wchar_t odd[MAX_PATH];path_join(odd,base,L"new\\ . ");
    CHECK(fpe_wsl_create(odd,0,NULL)==0);const wchar_t *oddpaths[]={odd};
    result=run(engine,FPE_OP_DELETE,oddpaths,1,NULL,FPE_CONFLICT_FAIL);
    CHECK(result.last.errors==0);path_join(odd,base,L"new");dir=NULL;
    CHECK(fpe_wsl_read(odd,&dir)==0&&dir&&find_entry(dir,L"case"));fpe_dir_release(dir);
    /* Mounted Windows drives bypass the 9P share for actual transfer bytes. */
    path_join(source,g_root,L"a.txt");paths[0]=source;
    result=run(engine,FPE_OP_COPY,paths,1,dest,FPE_CONFLICT_RENAME);CHECK(result.last.errors==0&&result.last.bytes_done==100);
    path_join(source,dest,L"a.txt");paths[0]=source;path_join(name,g_root,L"wsl_back");CreateDirectoryW(name,NULL);
    result=run(engine,FPE_OP_COPY,paths,1,name,FPE_CONFLICT_RENAME);if(result.last.errors)printf("  bridge copy error %lu\n",(unsigned long)result.last.last_error);CHECK(result.last.errors==0&&file_size(L"wsl_back\\a.txt")==100);
    result=run(engine,FPE_OP_MOVE,paths,1,name,FPE_CONFLICT_RENAME);if(result.last.errors)printf("  bridge move error %lu\n",(unsigned long)result.last.last_error);CHECK(result.last.errors==0&&file_size(L"wsl_back\\a (2).txt")==100);
    snprintf(script,sizeof script,"test ! -e /tmp/openpilot_jobs_%lu/dst/a.txt && printf moved",(unsigned long)pid);CHECK(wsl_sh(script,out,sizeof out)&&!strcmp(out,"moved"));
    snprintf(script,sizeof script,"dd if=/dev/zero of=/tmp/openpilot_jobs_%lu/src/medium bs=65536 count=3 status=none; touch -d @1700000000 /tmp/openpilot_jobs_%lu/src/medium",(unsigned long)pid,(unsigned long)pid);
    CHECK(wsl_sh(script,out,sizeof out));path_join(source,base,L"src\\medium");paths[0]=source;
    result=run(engine,FPE_OP_COPY,paths,1,name,FPE_CONFLICT_RENAME);
    CHECK(result.last.errors==0&&file_size(L"wsl_back\\medium")==196608);
    wchar_t copied[MAX_PATH];path_join(copied,name,L"medium");
    HANDLE metadata=CreateFileW(copied,FILE_READ_ATTRIBUTES,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);FILETIME modified={0};
    CHECK(metadata!=INVALID_HANDLE_VALUE&&GetFileTime(metadata,NULL,NULL,&modified));
    CHECK((((uint64_t)modified.dwHighDateTime<<32)|modified.dwLowDateTime)==133444736000000000ULL);
    if(metadata!=INVALID_HANDLE_VALUE)CloseHandle(metadata);
    /* Exercise the actual streamed Windows writer, including mid-file cancel
     * and a commit denied by a readonly overwrite destination. */
    snprintf(script,sizeof script,"truncate -s 268435456 /tmp/openpilot_jobs_%lu/src/big",(unsigned long)pid);
    CHECK(wsl_sh(script,out,sizeof out));
    path_join(source,base,L"src\\big");paths[0]=source;make_file(L"wsl_back\\big",3,0);
    cancelled=(native_cancel_result){.result={.ev=test_callback_event(FALSE)},.engine=engine,.after_bytes=1};
    cancel_id=fpe_job_submit(engine,FPE_OP_COPY,paths,1,name,FPE_CONFLICT_OVERWRITE,on_native_cancel,&cancelled);
    CHECK(cancel_id!=0);CHECK(test_wait_job(engine,cancel_id,cancelled.result.ev,10000)==WAIT_OBJECT_0);
    CHECK(cancelled.result.last.state==FPE_JOB_CANCELLED&&file_size(L"wsl_back\\big")==3);
    Sleep(5);CloseHandle(cancelled.result.ev);
    wchar_t pattern[MAX_PATH];path_join(pattern,name,L".openpilot-host-*.tmp");WIN32_FIND_DATAW found;
    HANDLE probe=FindFirstFileW(pattern,&found);CHECK(probe==INVALID_HANDLE_VALUE);if(probe!=INVALID_HANDLE_VALUE)FindClose(probe);
    make_file(L"wsl_back\\Foo",1,FILE_ATTRIBUTE_READONLY);paths[0]=upper;
    result=run(engine,FPE_OP_MOVE,paths,1,name,FPE_CONFLICT_OVERWRITE);
    CHECK(result.last.errors==1&&result.last.last_error==ERROR_ACCESS_DENIED&&file_size(L"wsl_back\\Foo")==1);
    path_join(pattern,name,L"Foo");SetFileAttributesW(pattern,FILE_ATTRIBUTE_NORMAL);
    snprintf(script,sizeof script,"test -f /tmp/openpilot_jobs_%lu/src/Foo && printf retained",(unsigned long)pid);
    CHECK(wsl_sh(script,out,sizeof out)&&!strcmp(out,"retained"));
    paths[0]=file;result=run(engine,FPE_OP_DELETE,paths,1,NULL,FPE_CONFLICT_FAIL);CHECK(result.last.errors==0);
    path_join(file,base,L"dst\\Tree\\broken");paths[0]=file;result=run(engine,FPE_OP_DELETE,paths,1,NULL,FPE_CONFLICT_FAIL);CHECK(result.last.errors==0);
    snprintf(script,sizeof script,"rm -rf /tmp/openpilot_jobs_%lu",(unsigned long)pid);wsl_sh(script,out,sizeof out);
}

static job_result queued_wsl_after_config(fpe_engine *engine, const wchar_t *root,
    const wchar_t *source, const wchar_t *dest, int tree, fpe_wsl_config after)
{
    wchar_t blocker_source[MAX_PATH],blocker_dest[MAX_PATH];
    path_join(blocker_source,g_root,L"B.TXT");
    path_join(blocker_dest,g_root,L"actor_block");CreateDirectoryW(blocker_dest,NULL);
    const wchar_t *blocker_paths[]={blocker_source};
    blocked_job blocker={0};blocker.result.ev=test_callback_event(FALSE);
    blocker.entered=test_callback_event(TRUE);
    blocker.release=test_callback_event(TRUE);
    uint64_t first=fpe_job_submit(engine,FPE_OP_COPY,blocker_paths,1,blocker_dest,
                                  FPE_CONFLICT_RENAME,on_blocked_job,&blocker);
    wait_blocked_job(engine,first,&blocker);
    job_result result={.ev=test_callback_event(FALSE)};
    const wchar_t *paths[]={tree?L"payload":source};
    uint64_t queued=tree?fpe_job_submit_tree(engine,root,paths,1,dest,FPE_CONFLICT_FAIL,on_job,NULL,&result)
                        :fpe_job_submit(engine,FPE_OP_COPY,paths,1,dest,FPE_CONFLICT_FAIL,on_job,&result);
    CHECK(queued!=0);
    fpe_wsl_configure(&after);
    SetEvent(blocker.release);
    CHECK(test_wait_job(engine,first,blocker.result.ev,30000)==WAIT_OBJECT_0);
    if(queued)CHECK(test_wait_job(engine,queued,result.ev,30000)==WAIT_OBJECT_0);
    CloseHandle(blocker.result.ev);CloseHandle(blocker.entered);CloseHandle(blocker.release);
    CloseHandle(result.ev);
    return result;
}

static void test_wsl_job_actor_drift(fpe_engine *engine)
{
    printf("queued WSL jobs: accepted actor, integration and tree context\n");
    DWORD pid=GetCurrentProcessId();char script[1024],distro[256],out[1024],uid[32];
    if(!wsl_sh("echo $WSL_DISTRO_NAME",distro,sizeof distro)||!distro[0]||
       !wsl_sh("id -u",uid,sizeof uid)||!strcmp(uid,"0")){
        printf("  (skipped: WSL default non-root user unavailable)\n");return;
    }
    snprintf(script,sizeof script,"D=/tmp/openpilot_actor_%lu; mkdir -p $D/src $D/locked/first $D/locked/second $D/locked/tree $D/open; printf actor > $D/src/payload",(unsigned long)pid);
    CHECK(wsl_sh(script,out,sizeof out));
    snprintf(script,sizeof script,"D=/tmp/openpilot_actor_%lu; chown -R root:root $D/locked; chmod 700 $D/locked $D/locked/first $D/locked/second $D/locked/tree",(unsigned long)pid);
    CHECK(wsl_sh_as(1,script,out,sizeof out));
    wchar_t base[MAX_PATH],source[MAX_PATH],srcdir[MAX_PATH],dest[MAX_PATH];
    _snwprintf(base,MAX_PATH,L"\\\\wsl.localhost\\%hs\\tmp\\openpilot_actor_%lu",distro,(unsigned long)pid);
    path_join(srcdir,base,L"src");path_join(source,srcdir,L"payload");
    fpe_wsl_config user={1,0,60000},admin={1,1,60000},off={0,0,60000};
    fpe_wsl_configure(&user);
    path_join(dest,base,L"locked\\first");
    job_result r=queued_wsl_after_config(engine,srcdir,source,dest,0,admin);
    CHECK(r.finals==1&&r.last.errors!=0);
    snprintf(script,sizeof script,"test ! -e /tmp/openpilot_actor_%lu/locked/first/payload",(unsigned long)pid);
    CHECK(wsl_sh_as(1,script,out,sizeof out));
    fpe_wsl_configure(&admin);
    path_join(dest,base,L"locked\\second");
    r=queued_wsl_after_config(engine,srcdir,source,dest,0,user);
    CHECK(r.finals==1&&r.last.state==FPE_JOB_DONE&&r.last.errors==0);
    snprintf(script,sizeof script,"test -f /tmp/openpilot_actor_%lu/locked/second/payload",(unsigned long)pid);
    CHECK(wsl_sh_as(1,script,out,sizeof out));
    fpe_wsl_configure(&admin);
    path_join(dest,base,L"locked\\tree");
    r=queued_wsl_after_config(engine,srcdir,source,dest,1,user);
    CHECK(r.finals==1&&r.last.state==FPE_JOB_DONE&&r.last.errors==0);
    snprintf(script,sizeof script,"test -f /tmp/openpilot_actor_%lu/locked/tree/payload",(unsigned long)pid);
    CHECK(wsl_sh_as(1,script,out,sizeof out));
    fpe_wsl_configure(&user);
    path_join(dest,base,L"open");
    r=queued_wsl_after_config(engine,srcdir,source,dest,0,off);
    CHECK(r.finals==1&&r.last.state==FPE_JOB_FAILED&&r.last.last_error==ERROR_NOT_SUPPORTED);
    snprintf(script,sizeof script,"test ! -e /tmp/openpilot_actor_%lu/open/payload",(unsigned long)pid);
    CHECK(wsl_sh_as(1,script,out,sizeof out));
    fpe_wsl_configure(NULL);
    snprintf(script,sizeof script,"rm -rf /tmp/openpilot_actor_%lu",(unsigned long)pid);
    CHECK(wsl_sh_as(1,script,out,sizeof out));
}

static void test_wsl(void)
{
    printf("WSL helper\n");
    char script[1024], distro[256];
    DWORD pid = GetCurrentProcessId();
    snprintf(script, sizeof script,
             "D=/tmp/fpe_wsl_%lu; rm -rf $D; mkdir -p $D/real && touch $D/real/inside && ln -s real $D/lnk && "
             "ln -s nowhere $D/broken && printf x > $D/'a:b' && printf 12345 > $D/run.sh && chmod 754 $D/run.sh && "
             "mkfifo $D/pipe && echo $WSL_DISTRO_NAME",
             (unsigned long)pid);
    if (!wsl_sh(script, distro, sizeof distro) || !distro[0] || strchr(distro, ' ')) {
        printf("  (skipped: no WSL distro to test with)\n");
        return;
    }
    wchar_t dir[MAX_PATH], sub[MAX_PATH];
    _snwprintf(dir, MAX_PATH, L"\\\\wsl.localhost\\%hs\\tmp\\fpe_wsl_%lu", distro, (unsigned long)pid);

    fpe_dir *d = NULL;
    fpe_err err = fpe_wsl_read(dir, &d);
    CHECK(err == 0 && d);
    if (d) {
        CHECK(fpe_dir_is_linux(d));
        CHECK(fpe_dir_count(d) == 6);
        fpe_unix u;
        /* ":" can't be in a Windows name: shown as U+F000 + ':' like \\wsl.localhost does */
        const fpe_entry *e = find_entry(d, L"a\xF03A" L"b");
        CHECK(e && e->size == 1 && !fpe_entry_is_dir(e));
        e = find_entry(d, L"run.sh");
        CHECK(e && fpe_dir_unix(d, e, &u) && (u.mode & 07777) == 0754 && e->size == 5 && !u.link);
        CHECK(e && u.owner && u.owner[0] && u.group && u.group[0]);
        e = find_entry(d, L"lnk"); /* a symlink to a folder opens as one */
        CHECK(e && fpe_entry_is_dir(e) && (e->attrs & FILE_ATTRIBUTE_REPARSE_POINT) && e->reparse_tag == 0xA000001Du);
        CHECK(e && fpe_dir_unix(d, e, &u) && u.link && !wcscmp(u.link, L"real"));
        e = find_entry(d, L"broken");
        CHECK(e && !fpe_entry_is_dir(e) && fpe_dir_unix(d, e, &u) && u.link && !wcscmp(u.link, L"nowhere"));
        e = find_entry(d, L"pipe");
        CHECK(e && e->reparse_tag == 0x80000024u);
        e = find_entry(d, L"real");
        CHECK(e && fpe_entry_is_dir(e) && fpe_dir_unix(d, e, &u) && !u.link && (u.mode & 0170000) == 0040000);
        fpe_dir_release(d);
    }

    /* through the symlink, which \\wsl.localhost can't follow */
    path_join(sub, dir, L"lnk");
    d = NULL;
    CHECK(fpe_wsl_read(sub, &d) == 0 && d && fpe_dir_count(d) == 1 && find_entry(d, L"inside"));
    fpe_dir_release(d);
    d = NULL;
    CHECK(fpe_dir_read(sub, &d) == 0 && d && fpe_dir_is_linux(d)); /* plain reads use the helper once it runs */
    fpe_dir_release(d);

    path_join(sub, dir, L"nope");
    d = NULL;
    CHECK(fpe_wsl_read(sub, &d) == ERROR_PATH_NOT_FOUND && !d);
    CHECK(fpe_wsl_read(g_root, &d) == ERROR_NOT_SUPPORTED && !d);
    CHECK(fpe_wsl_read(L"\\\\wsl.localhost\\no distro\\x", &d) == ERROR_NOT_SUPPORTED); /* not a name WSL allows */

    /* the same folder through a drive letter mapped to the distro (net use X: \\wsl.localhost\<distro>);
     * mapped for this test only and not remembered */
    wchar_t share[MAX_PATH], letter = 0;
    _snwprintf(share, MAX_PATH, L"\\\\wsl.localhost\\%hs", distro);
    DWORD used = GetLogicalDrives();
    for (wchar_t c = L'R'; c <= L'Y' && !letter; c++)
        if (!(used & (1u << (c - L'A')))) letter = c;
    wchar_t local[3] = { letter, L':', 0 };
    NETRESOURCEW nr = { 0 };
    nr.dwType = RESOURCETYPE_DISK;
    nr.lpLocalName = local;
    nr.lpRemoteName = share;
    if (letter && WNetAddConnection2W(&nr, NULL, NULL, 0) == NO_ERROR) {
        _snwprintf(sub, MAX_PATH, L"%lc:\\tmp\\fpe_wsl_%lu\\lnk", letter, (unsigned long)pid);
        d = NULL;
        CHECK(fpe_wsl_read(sub, &d) == 0 && d && fpe_dir_is_linux(d) && find_entry(d, L"inside"));
        CHECK(d && !wcsncmp(fpe_dir_path(d), local, 2)); /* shown as the user typed it */
        fpe_dir_release(d);
        WNetCancelConnection2W(local, 0, TRUE);
    } else {
        printf("  (drive letter mapping not tested: no free letter or mapping refused)\n");
    }

    /* inotify through the helper: \\wsl.localhost passes no change notifications */
    volatile LONG fired = 0;
    fpe_watch *w = NULL;
    CHECK(fpe_watch_start(dir, 30, on_watch, (void *)&fired, &w) == 0 && w);
    Sleep(300);
    fired = 0;
    snprintf(script, sizeof script, "touch /tmp/fpe_wsl_%lu/new", (unsigned long)pid);
    char ignore[64];
    wsl_sh(script, ignore, sizeof ignore);
    for (int i = 0; i < 100 && !fired; i++) Sleep(30);
    CHECK(fired > 0);
    fpe_watch_stop(w);

    fpe_wsl_stats st;
    fpe_wsl_get_stats(&st);
    CHECK(st.helpers >= 1 && st.starts >= 1 && st.failures == 0);
    printf("  helper started in %u ms\n", st.last_start_ms);

    /* with nothing open it stops by itself, and comes back when needed */
    fpe_wsl_config quick = { 1, 0, 200 };
    fpe_wsl_configure(&quick);
    d = NULL;
    CHECK(fpe_wsl_read(dir, &d) == 0);
    fpe_dir_release(d);
    for (int i = 0; i < 100; i++) {
        fpe_wsl_get_stats(&st);
        if (!st.helpers) break;
        Sleep(30);
    }
    CHECK(st.helpers == 0);
    d = NULL;
    CHECK(fpe_wsl_read(dir, &d) == 0 && d && fpe_dir_is_linux(d));
    fpe_dir_release(d);
    fpe_wsl_configure(NULL);

    snprintf(script, sizeof script, "rm -rf /tmp/fpe_wsl_%lu", (unsigned long)pid);
    wsl_sh(script, ignore, sizeof ignore);
}

static void teardown(fpe_engine *e)
{
    const wchar_t *root[] = { g_root };
    job_result r = run(e, FPE_OP_DELETE, root, 1, NULL, FPE_CONFLICT_RENAME);
    if (r.last.errors) printf("  warning: cleanup left files in %ls\n", g_root);
}


typedef struct size_probe {int calls,finals,stop;uint64_t bytes;int monotonic;} size_probe;
static int size_progress(void *user,const fpe_size_result *r)
{
    size_probe *p=user;p->calls++;if(r->state!=FPE_SIZE_RUNNING)p->finals++;
    if(r->bytes<p->bytes)p->monotonic=0;p->bytes=r->bytes;return !p->stop;
}
static void test_folder_sizes(void)
{
    printf("on-demand folder sizes\n");
    make_dir(L"sizes");make_dir(L"sizes\\empty");make_dir(L"sizes\\nested");
    make_file(L"sizes\\plain",17,0);make_file(L"sizes\\hidden",3,FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM);
    make_file(L"sizes\\nested\\inside",5,0);
    wchar_t root[MAX_PATH],file[MAX_PATH],alias[MAX_PATH];path_join(root,g_root,L"sizes");
    path_join(file,root,L"plain");path_join(alias,root,L"alias");CHECK(CreateHardLinkW(alias,file,NULL));
    fpe_size_result r;size_probe p={.monotonic=1};
    CHECK(!fpe_folder_size(root,0,size_progress,&p,&r));
    CHECK(r.state==FPE_SIZE_COMPLETE&&r.bytes==42&&r.files==4&&r.folders==3&&r.errors==0);
    CHECK(p.monotonic&&p.calls>=2&&p.finals==1);
    make_file(L"sizes\\plain",19,0);
    CHECK(!fpe_folder_size(root,0,NULL,NULL,&r)&&r.bytes==46); /* hard link shares changed contents */
    path_join(file,root,L"empty");CHECK(!fpe_folder_size(file,0,NULL,NULL,&r)&&r.bytes==0&&r.files==0&&r.folders==1);
    path_join(file,root,L"missing");CHECK(fpe_folder_size(file,0,NULL,NULL,&r)!=0&&r.state==FPE_SIZE_PARTIAL&&r.errors==1);
    path_join(file,root,L"plain");CHECK(fpe_folder_size(file,0,NULL,NULL,&r)!=0&&r.state==FPE_SIZE_PARTIAL&&r.files==0);
    p=(size_probe){.stop=1,.monotonic=1};
    CHECK(fpe_folder_size(root,0,size_progress,&p,&r)==ERROR_CANCELLED&&r.state==FPE_SIZE_CANCELLED&&r.bytes==0&&p.finals==1);
    CHECK(fpe_folder_size(NULL,0,NULL,NULL,&r)==ERROR_INVALID_PARAMETER&&r.state==FPE_SIZE_PARTIAL);
    CHECK(fpe_folder_size(root,0,NULL,NULL,NULL)==ERROR_INVALID_PARAMETER);
    /* Sharing denial on a nested directory preserves the measurable remainder. */
    path_join(file,root,L"nested");HANDLE locked=CreateFileW(file,GENERIC_READ,0,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
    CHECK(locked!=INVALID_HANDLE_VALUE);
    CHECK(fpe_folder_size(root,0,NULL,NULL,&r)==ERROR_SHARING_VIOLATION&&r.state==FPE_SIZE_PARTIAL&&r.bytes==41&&r.errors==1);
    if(locked!=INVALID_HANDLE_VALUE)CloseHandle(locked);
    /* A junction back to the root must be excluded, never recursively followed. */
    wchar_t cmd[3*MAX_PATH];path_join(alias,root,L"loop");_snwprintf(cmd,3*MAX_PATH,L"cmd /c mklink /J \"%ls\" \"%ls\" >nul",alias,root);
    CHECK(_wsystem(cmd)==0);
    CHECK(!fpe_folder_size(root,0,NULL,NULL,&r)&&r.bytes==46&&r.skipped==1&&r.folders==3);
    CHECK(!fpe_folder_size(alias,0,NULL,NULL,&r)&&r.skipped==1&&r.folders==0&&r.bytes==0);
    CHECK(RemoveDirectoryW(alias));
    /* Sparse files use logical length, including values beyond 32 bits. */
    path_join(file,root,L"sparse");HANDLE large=CreateFileW(file,GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);CHECK(large!=INVALID_HANDLE_VALUE);
    if(large!=INVALID_HANDLE_VALUE){
        DWORD got;CHECK(DeviceIoControl(large,0x900c4,NULL,0,NULL,0,&got,NULL)); /* FSCTL_SET_SPARSE */
        LARGE_INTEGER length;length.QuadPart=5LL*1024*1024*1024+7;
        CHECK(SetFilePointerEx(large,length,NULL,FILE_BEGIN)&&SetEndOfFile(large));CloseHandle(large);
        CHECK(!fpe_folder_size(root,0,NULL,NULL,&r)&&r.bytes==(uint64_t)length.QuadPart+46&&r.files==5);
        CHECK(DeleteFileW(file));
    }
    fpe_wsl_config off={0};fpe_wsl_configure(&off);
    CHECK(fpe_folder_size(L"\\\\wsl.localhost\\Ubuntu\\tmp",0,NULL,NULL,&r)==ERROR_NOT_SUPPORTED&&r.state==FPE_SIZE_PARTIAL&&r.errors==1);
    fpe_wsl_configure(NULL);
}


static void rename_row(fpe_rename_item *it,const wchar_t *from,const wchar_t *to)
{
    memset(it,0,sizeof *it);wcscpy(it->from,from);wcscpy(it->to,to);
}
typedef struct rename_stop {int phase;uint32_t after;} rename_stop;
static int rename_cancel(void *user,uint32_t completed,int applying)
{
    rename_stop *s=user;return applying!=s->phase||completed<s->after;
}
typedef struct rename_race {const wchar_t *path;int created;} rename_race;
static int rename_late_target(void *user,uint32_t completed,int applying)
{
    rename_race *r=user;if(applying&&completed==1&&!r->created){
        HANDLE f=CreateFileW(r->path,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
        if(f!=INVALID_HANDLE_VALUE){DWORD n;WriteFile(f,"keep",4,&n,NULL);CloseHandle(f);r->created=1;}
    }return 1;
}
static void test_bulk_rename(void)
{
    printf("bulk rename: Windows preview, conflicts, identity and cancellation\n");
    make_dir(L"rename");make_file(L"rename\\a.txt",5,0);make_file(L"rename\\b.txt",4,0);
    wchar_t dir[MAX_PATH],from[MAX_PATH],other[MAX_PATH];path_join(dir,g_root,L"rename");fpe_rename_item items[2];
    rename_row(&items[0],L"a.txt",L"first.txt");rename_row(&items[1],L"b.txt",L"second.txt");
    CHECK(!fpe_rename_batch(dir,0,items,2,0,NULL,NULL));
    CHECK(items[0].state==FPE_RENAME_READY&&items[1].state==FPE_RENAME_READY&&items[0].identity.inode);
    CHECK(exists(L"rename\\a.txt")&&!exists(L"rename\\first.txt"));
    CHECK(!fpe_rename_batch(dir,0,items,2,1,NULL,NULL));
    CHECK(items[0].state==FPE_RENAME_RENAMED&&items[1].state==FPE_RENAME_RENAMED);
    CHECK(!exists(L"rename\\a.txt")&&file_size(L"rename\\first.txt")==5&&file_size(L"rename\\second.txt")==4);
    rename_row(&items[0],L"first.txt",L"First.TXT");CHECK(!fpe_rename_batch(dir,0,items,1,0,NULL,NULL));
    CHECK(!fpe_rename_batch(dir,0,items,1,1,NULL,NULL)&&items[0].state==FPE_RENAME_RENAMED);
    fpe_dir *snapshot=NULL;CHECK(!fpe_dir_read(dir,&snapshot));
    int exact=0;if(snapshot){for(uint32_t i=0;i<fpe_dir_count(snapshot);i++)exact|=!wcscmp(fpe_dir_name(snapshot,fpe_dir_entries(snapshot)+i),L"First.TXT");fpe_dir_release(snapshot);}CHECK(exact);
    rename_row(&items[0],L"First.TXT",L"same.txt");rename_row(&items[1],L"second.txt",L"SAME.TXT");
    CHECK(fpe_rename_batch(dir,0,items,2,0,NULL,NULL)==ERROR_FILE_EXISTS&&items[0].state==FPE_RENAME_ERROR&&items[1].state==FPE_RENAME_ERROR);
    rename_row(&items[0],L"First.TXT",L"\u00c4.txt");rename_row(&items[1],L"second.txt",L"\u00e4.txt");
    CHECK(fpe_rename_batch(dir,0,items,2,0,NULL,NULL)==ERROR_FILE_EXISTS);
    rename_row(&items[0],L"First.TXT",L"second.txt");CHECK(fpe_rename_batch(dir,0,items,1,0,NULL,NULL)==ERROR_FILE_EXISTS);
    rename_row(&items[0],L"First.TXT",L"ready1");rename_row(&items[1],L"second.txt",L"ready2");
    CHECK(!fpe_rename_batch(dir,0,items,2,0,NULL,NULL));make_file(L"rename\\ready2",7,0);
    CHECK(fpe_rename_batch(dir,0,items,2,1,NULL,NULL)==ERROR_FILE_EXISTS);
    CHECK(items[0].state==FPE_RENAME_BLOCKED&&items[1].state==FPE_RENAME_ERROR&&exists(L"rename\\First.TXT")&&!exists(L"rename\\ready1"));
    CHECK(file_size(L"rename\\ready2")==7);
    rename_row(&items[0],L"First.TXT",L"fresh1");rename_row(&items[1],L"second.txt",L"fresh2");
    CHECK(!fpe_rename_batch(dir,0,items,2,0,NULL,NULL));
    path_join(from,dir,L"second.txt");path_join(other,dir,L"old-second");CHECK(MoveFileW(from,other));make_file(L"rename\\second.txt",11,0);
    CHECK(fpe_rename_batch(dir,0,items,2,1,NULL,NULL)==ERROR_FILE_INVALID);
    CHECK(items[0].state==FPE_RENAME_BLOCKED&&items[1].state==FPE_RENAME_ERROR&&file_size(L"rename\\second.txt")==11&&!exists(L"rename\\fresh1"));
    rename_row(&items[0],L"First.TXT",L"cancel1");rename_row(&items[1],L"second.txt",L"cancel2");
    rename_stop stop={0,0};CHECK(fpe_rename_batch(dir,0,items,2,0,rename_cancel,&stop)==ERROR_CANCELLED&&items[0].state==FPE_RENAME_STOPPED);
    CHECK(!fpe_rename_batch(dir,0,items,2,0,NULL,NULL));stop=(rename_stop){1,1};
    CHECK(fpe_rename_batch(dir,0,items,2,1,rename_cancel,&stop)==ERROR_CANCELLED);
    CHECK(items[0].state==FPE_RENAME_RENAMED&&items[1].state==FPE_RENAME_STOPPED&&exists(L"rename\\cancel1")&&exists(L"rename\\second.txt")&&!exists(L"rename\\cancel2"));
    rename_row(&items[0],L"cancel1",L"locked1");rename_row(&items[1],L"second.txt",L"locked2");CHECK(!fpe_rename_batch(dir,0,items,2,0,NULL,NULL));
    path_join(from,dir,L"second.txt");HANDLE lock=CreateFileW(from,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);CHECK(lock!=INVALID_HANDLE_VALUE);
    CHECK(fpe_rename_batch(dir,0,items,2,1,NULL,NULL)==ERROR_SHARING_VIOLATION);
    CHECK(items[0].state==FPE_RENAME_BLOCKED&&items[1].state==FPE_RENAME_ERROR&&!exists(L"rename\\locked1"));if(lock!=INVALID_HANDLE_VALUE)CloseHandle(lock);
    rename_row(&items[0],L"cancel1",L"late1");rename_row(&items[1],L"second.txt",L"late2");
    CHECK(!fpe_rename_batch(dir,0,items,2,0,NULL,NULL));path_join(other,dir,L"late2");rename_race race={other,0};
    CHECK(fpe_rename_batch(dir,0,items,2,1,rename_late_target,&race)==ERROR_FILE_EXISTS&&race.created);
    CHECK(items[0].state==FPE_RENAME_RENAMED&&items[1].state==FPE_RENAME_ERROR);
    CHECK(exists(L"rename\\late1")&&file_size(L"rename\\late2")==4&&exists(L"rename\\second.txt"));
    path_join(from,dir,L"late1");path_join(other,dir,L"cancel1");CHECK(MoveFileW(from,other));
    make_dir(L"rename\\folder.txt");make_file(L"rename\\folder.txt\\inside",6,0);
    rename_row(&items[0],L"folder.txt",L"folder-new");CHECK(!fpe_rename_batch(dir,0,items,1,0,NULL,NULL)&&(items[0].identity.attrs&FILE_ATTRIBUTE_DIRECTORY));
    CHECK(!fpe_rename_batch(dir,0,items,1,1,NULL,NULL)&&exists(L"rename\\folder-new\\inside"));
    rename_row(&items[0],L"cancel1",L"cancel1");CHECK(!fpe_rename_batch(dir,0,items,1,0,NULL,NULL)&&items[0].state==FPE_RENAME_UNCHANGED);
    CHECK(!fpe_rename_batch(dir,0,items,1,1,NULL,NULL)&&items[0].state==FPE_RENAME_UNCHANGED);
    const wchar_t *invalid[]={L"CON.txt",L"LPT1.log",L"a/b",L"..",L"trailing.",L"stream:name",L"NUL",L"COM\u00b9.txt"};
    for(unsigned i=0;i<sizeof invalid/sizeof *invalid;i++){
        rename_row(&items[0],L"cancel1",invalid[i]);CHECK(fpe_rename_batch(dir,0,items,1,0,NULL,NULL)==ERROR_INVALID_NAME);
    }
    rename_row(&items[0],L"cancel1",L"valid");for(int i=0;i<260;i++)items[0].from[i]=L'x';
    CHECK(fpe_rename_batch(dir,0,items,1,0,NULL,NULL)==ERROR_INVALID_NAME);
    CHECK(fpe_rename_batch(dir,0,items,0,0,NULL,NULL)==ERROR_INVALID_PARAMETER);
    CHECK(fpe_rename_batch(dir,0,items,257,0,NULL,NULL)==ERROR_INVALID_PARAMETER);
    rename_row(&items[0],L"cancel1",L"valid");CHECK(fpe_rename_batch(dir,0,items,1,1,NULL,NULL)==ERROR_INVALID_NAME); /* no preview */
    fpe_wsl_config cfg={0};fpe_wsl_configure(&cfg);rename_row(&items[0],L"unused",L"unused-new");
    CHECK(fpe_rename_batch(L"\\\\wsl.localhost\\Ubuntu\\tmp",0,items,1,0,NULL,NULL)==ERROR_NOT_SUPPORTED&&items[0].state==FPE_RENAME_ERROR);fpe_wsl_configure(NULL);
}
static void test_bulk_rename_wsl(void)
{
    printf("bulk rename: native WSL batch adapter\n");
    char script[1024],distro[256],value[64];DWORD pid=GetCurrentProcessId();
    snprintf(script,sizeof script,"D=/tmp/fpe_rename_%lu; mkdir -p $D; printf alpha >$D/'a:b'; printf beta >$D/b.txt; echo $WSL_DISTRO_NAME",(unsigned long)pid);
    CHECK(wsl_sh(script,distro,sizeof distro)&&distro[0]);if(!distro[0])return;
    wchar_t dir[MAX_PATH];_snwprintf(dir,MAX_PATH,L"\\\\wsl.localhost\\%hs\\tmp\\fpe_rename_%lu",distro,(unsigned long)pid);
    fpe_rename_item items[2];rename_row(&items[0],L"a\uf03a" L"b",L"changed\uf03a" L"b");rename_row(&items[1],L"b.txt",L"B.txt");
    CHECK(!fpe_rename_batch(dir,0,items,2,0,NULL,NULL)&&items[0].identity.inode&&items[0].state==FPE_RENAME_READY);
    CHECK(!fpe_rename_batch(dir,0,items,2,1,NULL,NULL)&&items[0].state==FPE_RENAME_RENAMED&&items[1].state==FPE_RENAME_RENAMED);
    snprintf(script,sizeof script,"cat /tmp/fpe_rename_%lu/'changed:b'",(unsigned long)pid);CHECK(wsl_sh(script,value,sizeof value)&&!strcmp(value,"alpha"));
    rename_row(&items[0],L"changed\uf03a" L"b",L"next");CHECK(!fpe_rename_batch(dir,0,items,1,0,NULL,NULL));
    snprintf(script,sizeof script,"D=/tmp/fpe_rename_%lu; mv $D/'changed:b' $D/old; printf replacement >$D/'changed:b'",(unsigned long)pid);CHECK(wsl_sh(script,value,sizeof value));
    CHECK(fpe_rename_batch(dir,0,items,1,1,NULL,NULL)==ERROR_FILE_INVALID&&items[0].state==FPE_RENAME_ERROR);
    rename_row(&items[0],L"changed\uf03a" L"b",L"next");rename_stop stop={0,0};CHECK(fpe_rename_batch(dir,0,items,1,0,rename_cancel,&stop)==ERROR_CANCELLED&&items[0].state==FPE_RENAME_STOPPED);
    snprintf(script,sizeof script,"rm -rf /tmp/fpe_rename_%lu",(unsigned long)pid);CHECK(wsl_sh(script,value,sizeof value));
}

static uint32_t compare_count(const fpe_comparison *r,uint32_t state)
{uint32_t n=0;for(uint32_t i=0;i<r->count;i++)n+=r->rows[i].state==state;return n;}
static int compare_stop(void *user){(*(int*)user)++;return 0;}
static void test_folder_compare(void)
{
    printf("folder comparison: Windows snapshots, SHA-256 and cancellation\n");
    make_dir(L"compareL");make_dir(L"compareR");make_dir(L"compareL\\folder");make_dir(L"compareR\\folder");make_dir(L"compareL\\type");make_file(L"compareR\\type",1,0);
    make_file(L"compareL\\left",1,0);make_file(L"compareR\\right",2,0);
    make_file(L"compareL\\size",2,0);make_file(L"compareR\\size",3,0);
    make_file(L"compareL\\Case",3,0);make_file(L"compareR\\case",3,0);
    make_file(L"compareL\\date",4,0);make_file(L"compareR\\date",4,0);
    make_file(L"compareL\\hidden",1,FILE_ATTRIBUTE_HIDDEN);make_file(L"compareL\\same",3,0);make_file(L"compareR\\same",3,0);
    make_file(L"compareL\\different",3,0);make_file(L"compareR\\different",3,0);
    wchar_t left[MAX_PATH],right[MAX_PATH],path[MAX_PATH];path_join(left,g_root,L"compareL");path_join(right,g_root,L"compareR");
    FILETIME fixed={.dwLowDateTime=0x12345600,.dwHighDateTime=0x01da0000};
    const wchar_t *names[]={L"same",L"different",L"Case",L"case"};
    for(int side=0;side<2;side++)for(int i=0;i<4;i++){
        path_join(path,side?right:left,names[i]);HANDLE h=CreateFileW(path,FILE_WRITE_ATTRIBUTES|GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
        if(h!=INVALID_HANDLE_VALUE){if(side&&i==1){DWORD wrote;WriteFile(h,"abc",3,&wrote,NULL);}SetFileTime(h,NULL,NULL,&fixed);CloseHandle(h);}
    }
    path_join(path,left,L"date");HANDLE file=CreateFileW(path,FILE_WRITE_ATTRIBUTES,0,NULL,OPEN_EXISTING,0,NULL);CHECK(file!=INVALID_HANDLE_VALUE);SetFileTime(file,NULL,NULL,&fixed);CloseHandle(file);
    fpe_comparison r;CHECK(!fpe_compare_folders(left,right,0,0,NULL,NULL,&r));
    CHECK(r.count==10&&r.completed==r.count);CHECK(compare_count(&r,FPE_COMPARE_LEFT)==2&&compare_count(&r,FPE_COMPARE_RIGHT)==1);
    CHECK(compare_count(&r,FPE_COMPARE_METADATA)==3);CHECK(compare_count(&r,FPE_COMPARE_DATE)==1);CHECK(compare_count(&r,FPE_COMPARE_SIZE)==1);
    CHECK(compare_count(&r,FPE_COMPARE_FOLDER)==1&&compare_count(&r,FPE_COMPARE_TYPE)==1);CHECK(!compare_count(&r,FPE_COMPARE_EQUAL));fpe_compare_free(&r);
    CHECK(!fpe_compare_folders(left,right,1,0,NULL,NULL,&r));CHECK(compare_count(&r,FPE_COMPARE_EQUAL)==3);CHECK(compare_count(&r,FPE_COMPARE_CONTENT)==1);CHECK(!compare_count(&r,FPE_COMPARE_ERROR));fpe_compare_free(&r);
    int calls=0;CHECK(fpe_compare_folders(left,right,1,0,compare_stop,&calls,&r)==ERROR_CANCELLED&&r.cancelled&&calls==1&&r.count==0);fpe_compare_free(&r);
    CHECK(fpe_compare_folders(left,L"Z:\\openpilot-definitely-absent",0,0,NULL,NULL,&r)!=0&&r.error&&r.count==0);fpe_compare_free(&r);
    path_join(path,left,L"same");fpe_hash hash;CHECK(!fpe_file_hash(path,0,NULL,NULL,&hash)&&hash.size==3);
    static const uint8_t expected[]={0xcd,0x2e,0xb0,0x83,0x7c,0x9b,0x4c,0x96,0x2c,0x22,0xd2,0xff,0x8b,0x54,0x41,0xb7,0xb4,0x58,0x05,0x88,0x7f,0x05,0x1d,0x39,0xbf,0x13,0x3b,0x58,0x3b,0xaf,0x68,0x60};
    CHECK(!memcmp(hash.sha256,expected,32));CHECK(fpe_file_hash(left,0,NULL,NULL,&hash)==ERROR_NOT_SUPPORTED&&hash.size==0);
    CHECK(fpe_file_hash(path,0,compare_stop,&calls,&hash)==ERROR_CANCELLED&&hash.size==0);CHECK(fpe_file_hash(NULL,0,NULL,NULL,&hash)==ERROR_INVALID_PARAMETER);
    file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);CHECK(file!=INVALID_HANDLE_VALUE);
    CHECK(!fpe_compare_folders(left,right,1,0,NULL,NULL,&r)&&compare_count(&r,FPE_COMPARE_ERROR)==1);fpe_compare_free(&r);if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
}
static void test_folder_compare_wsl(void)
{
    printf("folder comparison: mixed Windows/WSL exact names and native hashes\n");
    char script[1024],distro[256],value[64];DWORD pid=GetCurrentProcessId();
    snprintf(script,sizeof script,"D=/tmp/fpe_compare_%lu; mkdir -p $D/L $D/R; printf xxx >$D/L/Same; printf xxx >$D/R/Same; printf xxx >$D/R/same; printf abc >$D/R/'a:b'; ln -s Same $D/L/link; mkfifo $D/L/fifo; echo $WSL_DISTRO_NAME",(unsigned long)pid);
    CHECK(wsl_sh(script,distro,sizeof distro)&&distro[0]);if(!distro[0])return;
    wchar_t left[MAX_PATH],right[MAX_PATH],win[MAX_PATH],path[MAX_PATH];_snwprintf(left,MAX_PATH,L"\\\\wsl.localhost\\%hs\\tmp\\fpe_compare_%lu\\L",distro,(unsigned long)pid);_snwprintf(right,MAX_PATH,L"\\\\wsl.localhost\\%hs\\tmp\\fpe_compare_%lu\\R",distro,(unsigned long)pid);
    fpe_comparison r;CHECK(!fpe_compare_folders(left,right,1,0,NULL,NULL,&r));CHECK(r.count==5&&compare_count(&r,FPE_COMPARE_EQUAL)==1&&compare_count(&r,FPE_COMPARE_RIGHT)==2&&compare_count(&r,FPE_COMPARE_EXCLUDED)==2);fpe_compare_free(&r);
    make_dir(L"compareMixed");make_file(L"compareMixed\\Same",3,0);make_file(L"compareMixed\\CASE",1,0);path_join(win,g_root,L"compareMixed");
    CHECK(!fpe_compare_folders(win,right,1,0,NULL,NULL,&r));CHECK(compare_count(&r,FPE_COMPARE_EQUAL)==1&&compare_count(&r,FPE_COMPARE_LEFT)==1&&compare_count(&r,FPE_COMPARE_RIGHT)==2);fpe_compare_free(&r);
    path_join(path,right,L"a\uf03a" L"b");fpe_hash hash;CHECK(!fpe_file_hash(path,0,NULL,NULL,&hash)&&hash.size==3&&hash.sha256[0]==0xba&&hash.sha256[31]==0xad);
    path_join(path,left,L"fifo");CHECK(fpe_file_hash(path,0,NULL,NULL,&hash)!=0&&hash.size==0);
    fpe_wsl_config cfg={0};fpe_wsl_configure(&cfg);CHECK(fpe_file_hash(path,0,NULL,NULL,&hash)==ERROR_NOT_SUPPORTED);CHECK(fpe_compare_folders(left,right,0,0,NULL,NULL,&r)==ERROR_NOT_SUPPORTED&&r.error);fpe_compare_free(&r);fpe_wsl_configure(NULL);
    snprintf(script,sizeof script,"rm -rf /tmp/fpe_compare_%lu",(unsigned long)pid);CHECK(wsl_sh(script,value,sizeof value));
}

static uint32_t tree_state(const fpe_comparison *r,const wchar_t *name,int side)
{
    for(uint32_t i=0;i<r->count;i++){int n=side?r->rows[i].right:r->rows[i].left;
        if(n>=0&&!wcscmp(fpe_dir_name(r->dirs[side],fpe_dir_entries(r->dirs[side])+n),name))return r->rows[i].state;
    }return 0;
}
static job_result tree_run(fpe_engine *e,const wchar_t *root,const wchar_t *const *paths,uint32_t n,const wchar_t *dest,fpe_conflict policy)
{
    job_result result={0};result.ev=test_callback_event(FALSE);
    uint64_t id=fpe_job_submit_tree(e,root,paths,n,dest,policy,on_job,NULL,&result);CHECK(id!=0);
    if(id)CHECK(test_wait_job(e,id,result.ev,30000)==WAIT_OBJECT_0);Sleep(5);CloseHandle(result.ev);return result;
}
static void test_tree_compare(fpe_engine *engine)
{
    printf("recursive comparison: nested relative groups and selected copy jobs\n");
    make_dir(L"treeL");make_dir(L"treeR");make_dir(L"treeL\\a");make_dir(L"treeR\\a");make_dir(L"treeL\\only");make_dir(L"treeL\\only\\deep");
    make_file(L"treeL\\a\\same",3,0);make_file(L"treeR\\a\\same",3,0);
    make_file(L"treeL\\only\\deep\\pick",7,0);make_file(L"treeL\\only\\other",9,0);make_file(L"treeL\\a\\pick",5,0);
    make_dir(L"treeL\\conflict");make_file(L"treeL\\conflict\\secret",1,0);make_file(L"treeR\\conflict",2,0);
    wchar_t left[MAX_PATH],right[MAX_PATH],path[MAX_PATH];path_join(left,g_root,L"treeL");path_join(right,g_root,L"treeR");fpe_comparison result;
    CHECK(!fpe_compare_tree(left,right,1,0,NULL,NULL,&result));CHECK(result.count==8&&result.completed==result.count);
    CHECK(tree_state(&result,L"a\\same",0)==FPE_COMPARE_EQUAL);CHECK(tree_state(&result,L"only\\deep\\pick",0)==FPE_COMPARE_LEFT);
    CHECK(tree_state(&result,L"conflict",0)==FPE_COMPARE_TYPE&&!tree_state(&result,L"conflict\\secret",0));
    CHECK(compare_count(&result,FPE_COMPARE_FOLDER)==3);fpe_compare_free(&result);
    CHECK(!fpe_compare_tree(left,right,0,0,NULL,NULL,&result)&&tree_state(&result,L"only\\deep\\pick",0)==FPE_COMPARE_LEFT&&!compare_count(&result,FPE_COMPARE_EQUAL));fpe_compare_free(&result);
    make_dir(L"treeDepth");wchar_t deep[2048],deepRoot[MAX_PATH];path_join(deepRoot,g_root,L"treeDepth");wcscpy(deep,deepRoot);int created=1;
    for(unsigned i=0;i<129;i++){wcscat(deep,L"\\x");wchar_t nt[2056];_snwprintf(nt,2056,L"\\\\?\\%ls",deep);if(!CreateDirectoryW(nt,NULL))created=0;}
    CHECK(created);CHECK(fpe_compare_tree(deepRoot,right,0,0,NULL,NULL,&result)==ERROR_BUFFER_OVERFLOW&&result.error&&!result.cancelled);
    CHECK(compare_count(&result,FPE_COMPARE_ERROR)==1&&!tree_state(&result,L"x\\x\\unselected",0));fpe_compare_free(&result);
    int calls=0;CHECK(fpe_compare_tree(left,right,0,0,compare_stop,&calls,&result)==ERROR_CANCELLED&&result.cancelled&&!result.count);fpe_compare_free(&result);
    const wchar_t *selected[]={L"only\\deep\\pick",L"a\\pick"};job_result job=tree_run(engine,left,selected,2,right,FPE_CONFLICT_RENAME);
    CHECK(job.finals==1&&!job.last.errors&&job.last.items_total==2&&job.last.items_done==2&&job.last.bytes_done==12);
    path_join(path,right,L"only\\deep\\pick");CHECK(GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES);
    path_join(path,right,L"a\\pick");CHECK(GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES);
    path_join(path,right,L"only\\other");CHECK(GetFileAttributesW(path)==INVALID_FILE_ATTRIBUTES);
    CHECK(!fpe_compare_tree(left,right,1,0,NULL,NULL,&result)&&tree_state(&result,L"only\\deep\\pick",0)==FPE_COMPARE_EQUAL);fpe_compare_free(&result);
    const wchar_t *bad[]={L"..\\escape",L"a\\pick:stream",L"a\\NUL.txt",L"a\\x.bad:stream",L"\\absolute",L"a\\..\\escape",L"a\\trailing."};
    for(unsigned i=0;i<sizeof bad/sizeof *bad;i++)CHECK(!fpe_job_submit_tree(engine,left,&bad[i],1,right,FPE_CONFLICT_RENAME,NULL,NULL,NULL));
    const wchar_t *folder[]={L"only"};job=tree_run(engine,left,folder,1,right,FPE_CONFLICT_RENAME);CHECK(job.finals==1&&job.last.errors==1);
    path_join(path,right,L"only (2)");CHECK(GetFileAttributesW(path)==INVALID_FILE_ATTRIBUTES);
}
static void test_tree_compare_wsl(fpe_engine *engine)
{
    printf("recursive comparison: native pinned parents, literal paths and access boundaries\n");
    char script[2048],distro[256],value[256];DWORD pid=GetCurrentProcessId();
    snprintf(script,sizeof script,"D=/tmp/fpe_tree_%lu; mkdir -p $D/L/a/deep $D/R/a $D/L/denied; printf alpha >$D/L/a/deep/'a:b'; printf x >$D/L/a/Same; printf x >$D/R/a/same; ln -s / $D/L/link; mkfifo $D/L/fifo; chmod 000 $D/L/denied; echo $WSL_DISTRO_NAME",(unsigned long)pid);
    CHECK(wsl_sh(script,distro,sizeof distro)&&distro[0]);if(!distro[0])return;
    wchar_t left[MAX_PATH],right[MAX_PATH],win[MAX_PATH];_snwprintf(left,MAX_PATH,L"\\\\wsl.localhost\\%hs\\tmp\\fpe_tree_%lu\\L",distro,(unsigned long)pid);_snwprintf(right,MAX_PATH,L"\\\\wsl.localhost\\%hs\\tmp\\fpe_tree_%lu\\R",distro,(unsigned long)pid);
    fpe_comparison result;CHECK(fpe_compare_tree(left,right,1,0,NULL,NULL,&result)!=0&&result.error&&!result.cancelled);fpe_compare_free(&result);
    snprintf(script,sizeof script,"chmod 700 /tmp/fpe_tree_%lu/L/denied",(unsigned long)pid);CHECK(wsl_sh(script,value,sizeof value));
    CHECK(!fpe_compare_tree(left,right,1,0,NULL,NULL,&result));CHECK(compare_count(&result,FPE_COMPARE_EXCLUDED)==2);
    CHECK(tree_state(&result,L"a\\Same",0)==FPE_COMPARE_LEFT&&tree_state(&result,L"a\\same",1)==FPE_COMPARE_RIGHT);
    CHECK(tree_state(&result,L"a\\deep\\a\uf03a" L"b",0)==FPE_COMPARE_LEFT);fpe_compare_free(&result);
    const wchar_t *selected[]={L"a\\deep\\a\uf03a" L"b"};job_result job=tree_run(engine,left,selected,1,right,FPE_CONFLICT_RENAME);CHECK(job.finals==1&&!job.last.errors&&job.last.bytes_done==5);
    snprintf(script,sizeof script,"cat /tmp/fpe_tree_%lu/R/a/deep/'a:b'",(unsigned long)pid);CHECK(wsl_sh(script,value,sizeof value)&&!strcmp(value,"alpha"));
    make_dir(L"treeMixed");path_join(win,g_root,L"treeMixed");CHECK(!fpe_job_submit_tree(engine,left,selected,1,win,FPE_CONFLICT_RENAME,NULL,NULL,NULL));
    const wchar_t *mixed[]={L"a\\Same"};job=tree_run(engine,left,mixed,1,win,FPE_CONFLICT_RENAME);
    CHECK(job.finals==1&&!job.last.errors&&job.last.bytes_done==1);
    wchar_t imported[MAX_PATH];path_join(imported,win,L"a\\Same");CHECK(win_test_contents(imported,1,'x'));
    job=tree_run(engine,left,mixed,1,win,FPE_CONFLICT_OVERWRITE);CHECK(job.finals==1&&!job.last.errors&&win_test_contents(imported,1,'x'));
    HANDLE locked=CreateFileW(imported,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);CHECK(locked!=INVALID_HANDLE_VALUE);
    job=tree_run(engine,left,mixed,1,win,FPE_CONFLICT_OVERWRITE);CHECK(job.finals==1&&job.last.errors==1&&win_test_contents(imported,1,'x'));if(locked!=INVALID_HANDLE_VALUE)CloseHandle(locked);
    const wchar_t *links[]={L"link\\etc\\passwd"};job=tree_run(engine,left,links,1,right,FPE_CONFLICT_RENAME);CHECK(job.finals==1&&job.last.errors);
    snprintf(script,sizeof script,"test ! -e /tmp/fpe_tree_%lu/R/link/etc/passwd && echo safe",(unsigned long)pid);CHECK(wsl_sh(script,value,sizeof value)&&!strcmp(value,"safe"));
    fpe_wsl_config cfg={0};fpe_wsl_configure(&cfg);CHECK(fpe_compare_tree(left,right,0,0,NULL,NULL,&result)==ERROR_NOT_SUPPORTED&&result.error);fpe_compare_free(&result);fpe_wsl_configure(NULL);
    snprintf(script,sizeof script,"rm -rf /tmp/fpe_tree_%lu",(unsigned long)pid);CHECK(wsl_sh(script,value,sizeof value));
}

typedef struct search_probe {unsigned count,stop;wchar_t last[32768];} search_probe;
static int search_hit(void *user,const wchar_t *path,const fpe_entry *entry,const fpe_search_stats *stats)
{
    search_probe *p=user;if(p->stop)return 0;if(path){p->count++;wcscpy(p->last,path);}return 1;
}
static void test_folder_search(void)
{
    puts("live folder search: recursive names, filters, limits and cancellation");
    make_dir(L"search");make_dir(L"search\\deep");make_dir(L"search\\empty");
    make_file(L"search\\deep\\Report One.TXT",7,0);make_file(L"search\\other.pdf",33,0);make_file(L"search\\.hidden",1,0);
    wchar_t root[MAX_PATH];path_join(root,g_root,L"search");fpe_search_stats r;search_probe p={0};
    fpe_wsl_stats before,after;fpe_wsl_get_stats(&before);
    CHECK(!fpe_search_folder(root,L"report",search_hit,&p,&r)&&p.count==1&&r.matched==1&&wcsstr(p.last,L"deep\\Report One.TXT"));
    CHECK(r.scanned==5&&!r.errors&&!r.cancelled&&!r.limited);
    p=(search_probe){0};CHECK(!fpe_search_folder(root,L"\"report one\" ext:txt;md file: size:>=7",search_hit,&p,&r)&&p.count==1);
    p=(search_probe){0};CHECK(!fpe_search_folder(root,L"*.pdf size:<34",search_hit,&p,&r)&&p.count==1);
    CHECK(!fpe_search_folder(root,L"*.pdf size:>33",NULL,NULL,&r)&&!r.matched);
    CHECK(!fpe_search_folder(root,L"folder:",NULL,NULL,&r)&&r.matched==2);
    CHECK(!fpe_search_folder(root,L"file:",NULL,NULL,&r)&&r.matched==3);
    CHECK(!fpe_search_folder(root,L".hidden",NULL,NULL,&r)&&r.matched==1);
    CHECK(!fpe_search_folder(root,L"does-not-exist",NULL,NULL,&r)&&r.matched==0&&!r.errors);
    CHECK(!fpe_search_folder(root,L"dm:>=2020-01-01 dm:<2100-01-01 file:",NULL,NULL,&r)&&r.matched==3);
    p=(search_probe){.stop=1};CHECK(fpe_search_folder(root,L"*",search_hit,&p,&r)==ERROR_CANCELLED&&r.cancelled&&!r.scanned);
    CHECK(fpe_search_folder(root,L"regex:.*",NULL,NULL,&r)==ERROR_INVALID_PARAMETER&&r.errors);
    CHECK(fpe_search_folder(NULL,L"x",NULL,NULL,&r)==ERROR_INVALID_PARAMETER);
    fpe_wsl_get_stats(&after);CHECK(before.starts==after.starts&&before.helpers==after.helpers);
    make_file(L"search\\Ωmega.txt",1,0);CHECK(!fpe_search_folder(root,L"ωMEGA",NULL,NULL,&r)&&r.matched==1);
    CHECK(fpe_search_folder(L"Z:\\openpilot-missing-search-root",L"x",NULL,NULL,&r)!=0&&r.errors);
    make_dir(L"searchLimit");for(int i=0;i<305;i++){wchar_t name[100];swprintf(name,100,L"searchLimit\\item%03d",i);make_file(name,0,0);}
    path_join(root,g_root,L"searchLimit");CHECK(!fpe_search_folder(root,L"item",NULL,NULL,&r)&&r.limited&&r.matched==300);
    CHECK(fpe_search_folder(L"\\\\wsl.localhost\\OpenPilotMissingDistro-09b7241f\\tmp",L"*",NULL,NULL,&r)==ERROR_NOT_SUPPORTED);
}
static void test_folder_search_wsl(void)
{
    puts("native WSL search: streaming exact names, no symlink descent and disabled integration");
    DWORD pid=GetCurrentProcessId();char script[1600],distro[64],value[64];
    snprintf(script,sizeof script,"mkdir -p /tmp/fpe_search_%lu/deep; printf abc > '/tmp/fpe_search_%lu/deep/a:b'; printf xyz > '/tmp/fpe_search_%lu/deep/Case.TXT'; ln -s / '/tmp/fpe_search_%lu/link'; mkfifo '/tmp/fpe_search_%lu/fifo'; printf '%%s' \"$WSL_DISTRO_NAME\"",(unsigned long)pid,(unsigned long)pid,(unsigned long)pid,(unsigned long)pid,(unsigned long)pid);
    CHECK(wsl_sh(script,distro,sizeof distro)&&distro[0]);if(!distro[0])return;
    wchar_t root[MAX_PATH];swprintf(root,MAX_PATH,L"\\\\wsl.localhost\\%hs\\tmp\\fpe_search_%lu",distro,(unsigned long)pid);
    fpe_search_stats r;search_probe p={0};CHECK(fpe_wsl_native(root));
    CHECK(!fpe_search_folder(root,L"\"a:b\"",search_hit,&p,&r)&&p.count==1&&wcsstr(p.last,L"deep\\a\uf03a" L"b")&&r.scanned==5&&r.skipped==2);
    CHECK(!fpe_search_folder(root,L"ext:txt file: size:=3",NULL,NULL,&r)&&r.matched==1);
    CHECK(!fpe_search_folder(root,L"nothingmatches",NULL,NULL,&r)&&!r.matched&&!r.errors);
    p=(search_probe){.stop=1};CHECK(fpe_search_folder(root,L"*",search_hit,&p,&r)==ERROR_CANCELLED&&r.cancelled);
    fpe_wsl_config off={0};fpe_wsl_configure(&off);CHECK(!fpe_wsl_native(root));
    CHECK(fpe_search_folder(root,L"*",NULL,NULL,&r)==ERROR_NOT_SUPPORTED&&r.errors);fpe_wsl_configure(NULL);
    snprintf(script,sizeof script,"rm -rf /tmp/fpe_search_%lu",(unsigned long)pid);CHECK(wsl_sh(script,value,sizeof value));
}

static void test_wsl_index(void)
{
    puts("optional WSL index: selected roots, exact matcher, cancellation and live fallback");
    DWORD pid=GetCurrentProcessId();char script[1000],distro[64],value[64];
    snprintf(script,sizeof script,"mkdir -p /tmp/fpe_index_%lu/deep; printf '%%s' \"$WSL_DISTRO_NAME\"",(unsigned long)pid);
    CHECK(wsl_sh(script,distro,sizeof distro)&&distro[0]);if(!distro[0])return;
    wchar_t root[MAX_PATH],deep[MAX_PATH],path[MAX_PATH];swprintf(root,MAX_PATH,L"\\\\wsl.localhost\\%hs\\tmp\\fpe_index_%lu",distro,(unsigned long)pid);path_join(deep,root,L"deep");
    const wchar_t raw_name[]={L'n',L'e',L'e',L'd',L'l',L'e',L'R',L'a',L'w',0xdcff,0};
    const wchar_t *names[]={L"needleCase.TXT",L"Ωneedle.TXT",raw_name,L"Ωother",L"needle\uf03a" L"a",L"bracket[needle]"};
    for(unsigned i=0;i<6;i++){path_join(path,deep,names[i]);CHECK(!fpe_wsl_create(path,0,NULL));}
    fpe_index_status info;fpe_search_stats r;search_probe p={0};
    CHECK(fpe_wsl_available(root));CHECK(!fpe_wsl_available(L"C:\\Windows"));
    CHECK(!fpe_wsl_available(L"\\\\wsl.localhost\\OpenPilotMissingDistro-09b7241f\\tmp"));
    CHECK(fpe_search_index_control(g_root,FPE_INDEX_STATUS,NULL,NULL,&info)==ERROR_NOT_SUPPORTED);
    CHECK(!fpe_search_index_control(root,FPE_INDEX_STATUS,NULL,NULL,&info)&&info.state==FPE_INDEX_NONE);
    CHECK(!fpe_search_index_control(root,FPE_INDEX_ENABLE,NULL,NULL,&info)&&info.enabled&&info.state!=FPE_INDEX_CURRENT&&info.uid!=0);
    uint64_t until=GetTickCount64()+5000;while(info.state!=FPE_INDEX_CURRENT&&GetTickCount64()<until){fpe_search_index_control(root,FPE_INDEX_STATUS,NULL,NULL,&info);Sleep(10);}
    CHECK(info.state==FPE_INDEX_CURRENT&&info.entries==7&&fpe_path_equal(info.root,root));
    CHECK(!fpe_search_folder_indexed(root,L"needle",search_hit,&p,&r,&info)&&p.count==5&&r.scanned==6&&!r.errors&&!r.limited);
    p=(search_probe){0};CHECK(!fpe_search_folder_indexed(root,L"needle ω",search_hit,&p,&r,&info)&&p.count==1&&wcsstr(p.last,L"Ωneedle"));
    p=(search_probe){0};CHECK(!fpe_search_folder_indexed(deep,L"\"needle:a\"",search_hit,&p,&r,&info)&&p.count==1&&wcsstr(p.last,L"deep\\needle\uf03a" L"a"));
    p=(search_probe){0};CHECK(!fpe_search_folder_indexed(root,L"bracket[needle]",search_hit,&p,&r,&info)&&p.count==1);
    CHECK(fpe_search_folder_indexed(root,L"ne",NULL,NULL,&r,&info)==ERROR_NOT_SUPPORTED);
    CHECK(fpe_search_folder_indexed(root,L"ω",NULL,NULL,&r,&info)==ERROR_NOT_SUPPORTED);
    CHECK(fpe_search_folder_indexed(root,L"needle size:=0",NULL,NULL,&r,&info)==ERROR_NOT_SUPPORTED);
    p=(search_probe){.stop=1};CHECK(fpe_search_folder_indexed(root,L"needle",search_hit,&p,&r,&info)==ERROR_CANCELLED&&r.cancelled);
    CHECK(!fpe_search_index_control(root,FPE_INDEX_STATUS,NULL,NULL,&info)&&info.state==FPE_INDEX_CURRENT);
    CHECK(!fpe_search_index_control(root,FPE_INDEX_PAUSE,NULL,NULL,&info)&&info.state==FPE_INDEX_PAUSED);
    CHECK(fpe_search_folder_indexed(root,L"needle",NULL,NULL,&r,&info)==ERROR_RETRY&&info.state==FPE_INDEX_PAUSED);
    CHECK(!fpe_search_folder(root,L"needle",NULL,NULL,&r)&&r.matched==5);
    CHECK(!fpe_search_index_control(root,FPE_INDEX_RESUME,NULL,NULL,&info)&&info.state!=FPE_INDEX_CURRENT);
    until=GetTickCount64()+5000;while(info.state!=FPE_INDEX_CURRENT&&GetTickCount64()<until){fpe_search_index_control(root,FPE_INDEX_STATUS,NULL,NULL,&info);Sleep(10);}
    CHECK(info.state==FPE_INDEX_CURRENT);
    snprintf(script,sizeof script,"for i in $(seq 1 305); do touch /tmp/fpe_index_%lu/needle-limit-$i; done",(unsigned long)pid);CHECK(wsl_sh(script,value,sizeof value));
    fpe_search_index_control(root,FPE_INDEX_STATUS,NULL,NULL,&info);until=GetTickCount64()+5000;
    while((info.state!=FPE_INDEX_CURRENT||info.entries!=312)&&GetTickCount64()<until){fpe_search_index_control(root,FPE_INDEX_STATUS,NULL,NULL,&info);Sleep(10);}
    CHECK(info.state==FPE_INDEX_CURRENT&&info.entries==312);
    CHECK(!fpe_search_folder_indexed(root,L"needle",NULL,NULL,&r,&info)&&r.matched==300&&r.limited&&!r.errors);
    CHECK(!fpe_search_index_control(root,FPE_INDEX_STATUS,NULL,NULL,&info)&&info.state==FPE_INDEX_CURRENT);
    fpe_search_index_shutdown();CHECK(!fpe_search_index_control(root,FPE_INDEX_STATUS,NULL,NULL,&info)&&info.state!=FPE_INDEX_CURRENT);
    CHECK(!fpe_search_index_control(root,FPE_INDEX_REBUILD,NULL,NULL,&info)&&info.enabled);
    CHECK(!fpe_search_index_control(root,FPE_INDEX_REMOVE,NULL,NULL,&info)&&info.state==FPE_INDEX_NONE);
    fpe_wsl_config off={0};fpe_wsl_configure(&off);CHECK(!fpe_wsl_available(root));
    CHECK(fpe_search_index_control(root,FPE_INDEX_ENABLE,NULL,NULL,&info)==ERROR_NOT_SUPPORTED);fpe_search_index_shutdown();fpe_wsl_configure(NULL);
    snprintf(script,sizeof script,"rm -rf /tmp/fpe_index_%lu",(unsigned long)pid);CHECK(wsl_sh(script,value,sizeof value));
}

/* Python is an independent byte-name fixture/oracle; operations use the engine. */
static int name_python(const char *python,char *out,DWORD cap)
{
    size_t n=strlen(python);char command[2048],*p=command;
    if(n>850)return 0;p+=sprintf(p,"python3 -c 'import sys;exec(bytes.fromhex(sys.argv[1]))' ");
    static const char hex[]="0123456789abcdef";
    for(size_t i=0;i<n;i++){unsigned c=(unsigned char)python[i];*p++=hex[c>>4];*p++=hex[c&15];}*p=0;
    return wsl_sh(command,out,cap);
}
typedef struct name_hits {const wchar_t *a,*p;unsigned count,ascii,pua;} name_hits;
static int name_identity_hit(void *user,const wchar_t *path,const fpe_entry *entry,const fpe_search_stats *stats)
{
    (void)entry;(void)stats;name_hits *hits=user;if(path){hits->count++;hits->ascii+=fpe_path_equal(path,hits->a);hits->pua+=fpe_path_equal(path,hits->p);}return 1;
}
static void test_wsl_name_identity(fpe_engine *engine)
{
    puts("Linux name identity: real alias/PUA siblings, selected copy/move/delete, rename, chmod, links and indexes");
    char python[1800],reply[2048],base[128],distro[128];
    snprintf(base,sizeof base,"/tmp/fpe_names_%lu_%llu",GetCurrentProcessId(),(unsigned long long)GetTickCount64());
    CHECK(wsl_sh("printf '%s' $WSL_DISTRO_NAME",distro,sizeof distro)&&!strcmp(distro,"Ubuntu"));
    snprintf(python,sizeof python,"import os;b=b'%s';os.mkdir(b);[(os.mkdir(b+b'/'+x)) for x in [b'L',b'D',b'M',b'C']];cs=list(range(1,32))+list(map(ord,chr(92)+':*?'+chr(34)+'<>|'));[(open(b+b'/L/n-'+x+b'-end','wb').write(v)) for c in cs for x,v in [(bytes([c]),b'A'),(chr(0xf000+c).encode(),b'P')]];open(b+b'/L/n-'+chr(0xf080).encode()+chr(0xf03a).encode()+b'-end','wb').write(b'F');[(open(b+b'/L/'+x,'wb').write(b'P')) for x in [b'escape-'+chr(0xf080).encode(),b'raw-'+bytes([255]),chr(0xf03a).encode()*85]];[(os.mkdir(b+b'/L/d-'+x),open(b+b'/L/d-'+x+b'/inside','wb').write(v)) for x,v in [(b':',b'A'),(chr(0xf03a).encode(),b'P'),((chr(0xf080)+chr(0xf03a)).encode(),b'F')]];print('ready')",base);
    int created=name_python(python,reply,sizeof reply);CHECK(created&&!strcmp(reply,"ready"));if(!created)return;
    wchar_t root[512],left[512],dest[512],moved[512],a[512],pua[512],dirp[512],marker[512],created_dir[512];
    _snwprintf(root,512,L"\\\\wsl.localhost\\%hs%hs",distro,base);for(wchar_t *p=root;*p;p++)if(*p==L'/')*p=L'\\';
    _snwprintf(left,512,L"%ls\\L",root);_snwprintf(dest,512,L"%ls\\D",root);_snwprintf(moved,512,L"%ls\\M",root);_snwprintf(created_dir,512,L"%ls\\C",root);
    fpe_dir *dir=NULL;CHECK(!fpe_wsl_read(left,&dir)&&dir&&fpe_dir_count(dir)==85);
    wchar_t selected[78][512];const wchar_t *sources[78];unsigned count=0;
    if(dir)for(unsigned c=1;c<128;c++)if(fpe_lx_mapped(c)){
        wchar_t an[]={L'n',L'-',(wchar_t)(0xf000+c),L'-',L'e',L'n',L'd',0};
        wchar_t pn[]={L'n',L'-',0xf080,(wchar_t)(0xf000+c),L'-',L'e',L'n',L'd',0};
        const fpe_entry *ae=find_entry(dir,an),*pe=find_entry(dir,pn);CHECK(ae&&pe&&ae!=pe&&ae->size==1&&pe->size==1);
        _snwprintf(selected[count],512,L"%ls\\%ls",left,an);sources[count]=selected[count];count++;
        _snwprintf(selected[count],512,L"%ls\\%ls",left,pn);sources[count]=selected[count];count++;
    }
    CHECK(count==78);
    _snwprintf(a,512,L"%ls\\n-\uf03a-end",left);_snwprintf(pua,512,L"%ls\\n-\uf080\uf03a-end",left);
    _snwprintf(dirp,512,L"%ls\\d-\uf080\uf03a",left);_snwprintf(marker,512,L"%ls\\escape-\uf080\uf080",left);
    CHECK(!fpe_path_equal(a,pua));
    fpe_hash ah={0},ph={0};CHECK(!fpe_file_hash(a,0,NULL,NULL,&ah)&&!fpe_file_hash(pua,0,NULL,NULL,&ph)&&ah.size==1&&ph.size==1&&memcmp(ah.sha256,ph.sha256,32));
    if(dir){
        fpe_view *view=fpe_view_create(dir,NULL);CHECK(view!=NULL);
        if(view){CHECK(fpe_view_find(view,L"n-\uf03a-end")!=fpe_view_find(view,L"n-\uf080\uf03a-end"));
            CHECK(!fpe_view_set_filter(view,L"n-\uf03a-end")&&fpe_view_count(view)==1&&!wcscmp(fpe_view_name(view,0),L"n-\uf080\uf03a-end"));
            CHECK(!fpe_view_set_filter(view,L"n-:-end")&&fpe_view_count(view)==1&&!wcscmp(fpe_view_name(view,0),L"n-\uf03a-end"));fpe_view_destroy(view);}
        fpe_dir_release(dir);dir=NULL;
    }
    if(count==78){job_result copied=run(engine,FPE_OP_COPY,sources,count,dest,FPE_CONFLICT_FAIL);CHECK(copied.finals==1&&!copied.last.errors&&copied.last.bytes_done==78);}
    snprintf(python,sizeof python,"import os;b=b'%s';cs=list(range(1,32))+list(map(ord,chr(92)+':*?'+chr(34)+'<>|'));expected={b'n-'+x+b'-end':v for c in cs for x,v in [(bytes([c]),b'A'),(chr(0xf000+c).encode(),b'P')]};assert set(os.listdir(b+b'/D'))==set(expected);assert all(open(b+b'/D/'+n,'rb').read()==v for n,v in expected.items());print('exact')",base);
    CHECK(name_python(python,reply,sizeof reply)&&!strcmp(reply,"exact"));
    CHECK(!fpe_wsl_read(dirp,&dir)&&dir&&fpe_dir_count(dir)==1);if(dir){fpe_dir_release(dir);dir=NULL;}
    fpe_permissions before,after;int changed=0;CHECK(!fpe_wsl_permissions(pua,0,NULL,0,&before,NULL));
    CHECK(!fpe_wsl_permissions(pua,0,&before,0600,&after,&changed)&&changed&&(after.mode&0777)==0600);
    wchar_t rename[512],link[512];_snwprintf(rename,512,L"%ls\\renamed-P",left);_snwprintf(link,512,L"%ls\\link-P",root);
    CHECK(!fpe_wsl_symlink(pua,link,1));CHECK(!fpe_wsl_rename(pua,rename));CHECK(!fpe_wsl_rename(rename,pua));
    snprintf(python,sizeof python,"import os,stat;b=b'%s';p=b'/L/n-'+chr(0xf03a).encode()+b'-end';assert open(b+b'/L/n-:-end','rb').read()==b'A';assert open(b+p,'rb').read()==b'P';assert stat.S_IMODE(os.stat(b+p).st_mode)==0o600;assert stat.S_IMODE(os.stat(b+b'/L/n-:-end').st_mode)!=0o600;assert os.readlink(b+b'/link-P')==p[1:];assert open(b+b'/link-P','rb').read()==b'P';print('exact')",base);
    CHECK(name_python(python,reply,sizeof reply)&&!strcmp(reply,"exact"));
    wchar_t malformed[512];_snwprintf(malformed,512,L"%ls\\escape-\uf080",left);
    CHECK(fpe_file_hash(malformed,0,NULL,NULL,&ph)!=0);
    const wchar_t *bad_source[]={malformed};job_result bad=run(engine,FPE_OP_COPY,bad_source,1,created_dir,FPE_CONFLICT_FAIL);CHECK(bad.last.errors&&!bad.last.bytes_done);
    fpe_search_stats stats;name_hits hits={a,pua,0,0,0};
    CHECK(!fpe_search_folder(left,L"\"n-\uf03a-end\"",name_identity_hit,&hits,&stats)&&hits.count==1&&hits.pua==1&&!hits.ascii);
    hits=(name_hits){a,pua,0,0,0};CHECK(!fpe_search_folder(left,L"\"n-:-end\"",name_identity_hit,&hits,&stats)&&hits.count==1&&hits.ascii==1&&!hits.pua);
    fpe_index_status index;CHECK(!fpe_search_index_control(left,FPE_INDEX_ENABLE,NULL,NULL,&index));uint64_t until=GetTickCount64()+5000;
    while(index.state!=FPE_INDEX_CURRENT&&GetTickCount64()<until){fpe_search_index_control(left,FPE_INDEX_STATUS,NULL,NULL,&index);Sleep(10);}
    hits=(name_hits){a,pua,0,0,0};CHECK(index.state==FPE_INDEX_CURRENT&&!fpe_search_folder_indexed(left,L"-end",name_identity_hit,&hits,&stats,&index)&&hits.count==79&&hits.ascii==1&&hits.pua==1);
    CHECK(!fpe_search_index_control(left,FPE_INDEX_REMOVE,NULL,NULL,&index));fpe_search_index_shutdown();
    wchar_t created_a[512],created_p[512];_snwprintf(created_a,512,L"%ls\\\uf03a",created_dir);_snwprintf(created_p,512,L"%ls\\\uf080\uf03a",created_dir);
    CHECK(!fpe_wsl_create(created_a,0,NULL)&&!fpe_wsl_create(created_p,0,NULL));
    make_dir(L"name_windows");make_file(L"name_windows\\n-\uf03a-end",1,0);wchar_t win_source[512];path_join(win_source,g_root,L"name_windows\\n-\uf03a-end");
    const wchar_t *win_one[]={win_source};job_result win_copy=run(engine,FPE_OP_COPY,win_one,1,created_dir,FPE_CONFLICT_FAIL);CHECK(win_copy.finals==1&&!win_copy.last.errors);
    fpe_wsl_config off={0};fpe_wsl_configure(&off);CHECK(fpe_wsl_requires_native(pua)&&!fpe_wsl_native(pua));
    CHECK(fpe_dir_read(dirp,&dir)==ERROR_NOT_SUPPORTED&&dir==NULL);CHECK(fpe_dir_read(left,&dir)==ERROR_NOT_SUPPORTED&&dir==NULL);
    const wchar_t *one[]={pua};job_result denied=run(engine,FPE_OP_COPY,one,1,created_dir,FPE_CONFLICT_OVERWRITE);CHECK(denied.last.errors&&!denied.last.bytes_done);
    denied=run(engine,FPE_OP_COPY,win_one,1,created_dir,FPE_CONFLICT_OVERWRITE);CHECK(denied.last.errors&&win_test_contents(win_source,1,'x'));
    denied=run(engine,FPE_OP_MOVE,win_one,1,created_dir,FPE_CONFLICT_OVERWRITE);CHECK(denied.last.errors&&win_test_contents(win_source,1,'x'));
    fpe_wsl_configure(NULL);
    snprintf(python,sizeof python,"import os;b=b'%s';assert set(os.listdir(b+b'/C'))=={b':',chr(0xf03a).encode(),b'n-'+chr(0xf03a).encode()+b'-end'};assert open(b+b'/C/n-'+chr(0xf03a).encode()+b'-end','rb').read()==b'x';assert os.stat(b+b'/C/'+chr(0xf03a).encode()).st_size==0;assert open(b+b'/L/d-'+(chr(0xf080)+chr(0xf03a)).encode()+b'/inside','rb').read()==b'F';print('exact')",base);
    CHECK(name_python(python,reply,sizeof reply)&&!strcmp(reply,"exact"));
    const wchar_t *two[]={pua,marker};job_result move=run(engine,FPE_OP_MOVE,two,2,moved,FPE_CONFLICT_FAIL);CHECK(move.finals==1&&!move.last.errors);
    wchar_t moved_p[512],moved_marker[512];_snwprintf(moved_p,512,L"%ls\\n-\uf080\uf03a-end",moved);_snwprintf(moved_marker,512,L"%ls\\escape-\uf080\uf080",moved);
    two[0]=moved_p;two[1]=moved_marker;job_result deleted=run(engine,FPE_OP_DELETE,two,2,NULL,FPE_CONFLICT_FAIL);CHECK(deleted.finals==1&&!deleted.last.errors);
    snprintf(python,sizeof python,"import os;b=b'%s';assert os.listdir(b+b'/M')==[];assert open(b+b'/L/n-:-end','rb').read()==b'A';assert not os.path.exists(b+b'/L/n-'+chr(0xf03a).encode()+b'-end');assert open(b+b'/D/n-'+chr(0xf03a).encode()+b'-end','rb').read()==b'P';print('exact')",base);
    CHECK(name_python(python,reply,sizeof reply)&&!strcmp(reply,"exact"));
    snprintf(python,sizeof python,"import shutil;shutil.rmtree('%s');print('removed')",base);CHECK(name_python(python,reply,sizeof reply)&&!strcmp(reply,"removed"));
}

int wmain(void)
{
    setup();
    fpe_engine *e = fpe_engine_create(NULL);

    test_natural_compare();
    test_dir_read();
    test_view();
    test_view_sort_orders();
    test_view_order_snapshot();
    test_profiling();
    test_engine_cache(e);
    test_watch();
    test_jobs(e);
    test_windows_job_operands_refuse_literal_suffix_aliases(e);
    test_windows_overwrite_integrity(e);
    test_windows_junction_merge_integrity(e);
    test_windows_junction_replacement(e);
    test_job_diagnostics(e);
    test_unix_modes();
    test_wsl();
    test_wsl_name_identity(e);
    test_wsl_jobs(e);
    test_wsl_job_actor_drift(e);
    test_folder_sizes();
    test_folder_search();
    test_folder_search_wsl();
    test_wsl_index();
    test_bulk_rename();
    test_bulk_rename_wsl();
    test_folder_compare();
    test_folder_compare_wsl();
    test_tree_compare(e);
    test_tree_compare_wsl(e);

    teardown(e);
    fpe_engine_destroy(e);

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
