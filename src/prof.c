#include "internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static volatile LONG g_enabled;
static SRWLOCK g_format_lock = SRWLOCK_INIT; /* qsort comparators share g_sort_rows */
static fpe_zone *volatile g_zones;
static fpe_gauge *volatile g_gauges;
static volatile int64_t g_freq;

static double ticks_to_ms(int64_t t)
{
    if (!g_freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_freq = f.QuadPart;
    }
    return (double)t * 1000.0 / (double)g_freq;
}

/* Zones and gauges are pushed onto lock-free lists the first time they are
 * used; they are never removed, so readers can walk the lists at any time. */
static void register_zone(fpe_zone *z)
{
    if (InterlockedCompareExchange(&z->state, 1, 0) != 0) return;
    fpe_zone *head;
    do {
        head = g_zones;
        z->next = head;
    } while (InterlockedCompareExchangePointer((PVOID volatile *)&g_zones, z, head) != head);
    InterlockedExchange(&z->state, 2);
}

static void register_gauge(fpe_gauge *g)
{
    if (InterlockedCompareExchange(&g->state, 1, 0) != 0) return;
    fpe_gauge *head;
    do {
        head = g_gauges;
        g->next = head;
    } while (InterlockedCompareExchangePointer((PVOID volatile *)&g_gauges, g, head) != head);
    InterlockedExchange(&g->state, 2);
}

FPE_API void fpe_prof_enable(int on) { InterlockedExchange(&g_enabled, on ? 1 : 0); }
FPE_API int fpe_prof_enabled(void) { return g_enabled != 0; }

FPE_API int64_t fpe_prof_begin(void)
{
    if (!g_enabled) return 0;
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart ? c.QuadPart : 1;
}

FPE_API void fpe_prof_end(fpe_zone *z, int64_t t0)
{
    if (!t0) return;
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    int64_t d = c.QuadPart - t0;
    if (z->state != 2) register_zone(z);
    InterlockedIncrement64(&z->calls);
    InterlockedAdd64(&z->ticks, d);
    int64_t m = z->max_ticks;
    while (d > m) {
        int64_t prev = InterlockedCompareExchange64(&z->max_ticks, d, m);
        if (prev == m) break;
        m = prev;
    }
}

FPE_API void fpe_prof_count(fpe_zone *z)
{
    if (!g_enabled) return;
    if (z->state != 2) register_zone(z);
    InterlockedIncrement64(&z->calls);
}

FPE_API void fpe_gauge_add(fpe_gauge *g, int64_t delta)
{
    if (g->state != 2) register_gauge(g);
    int64_t v = InterlockedAdd64(&g->value, delta);
    int64_t p = g->peak;
    while (v > p) {
        int64_t prev = InterlockedCompareExchange64(&g->peak, v, p);
        if (prev == p) break;
        p = prev;
    }
}

FPE_API void fpe_prof_reset(void)
{
    for (fpe_zone *z = g_zones; z; z = z->next) {
        InterlockedExchange64(&z->calls, 0);
        InterlockedExchange64(&z->ticks, 0);
        InterlockedExchange64(&z->max_ticks, 0);
    }
    for (fpe_gauge *g = g_gauges; g; g = g->next) InterlockedExchange64(&g->peak, g->value);
}

FPE_API uint32_t fpe_prof_rows(fpe_prof_row *out, uint32_t max)
{
    uint32_t n = 0;
    for (fpe_zone *z = g_zones; z; z = z->next) {
        int64_t calls = z->calls;
        if (!calls) continue;
        double total = ticks_to_ms(z->ticks), mx = ticks_to_ms(z->max_ticks);
        uint32_t i = 0;
        while (i < n && (out[i].is_gauge || strcmp(out[i].name, z->name) != 0)) i++;
        if (i < n) {
            out[i].calls += calls;
            out[i].total_ms += total;
            if (mx > out[i].max_ms) out[i].max_ms = mx;
            continue;
        }
        if (n == max) break;
        memset(&out[n], 0, sizeof out[n]);
        out[n].name = z->name;
        out[n].calls = calls;
        out[n].total_ms = total;
        out[n].max_ms = mx;
        n++;
    }
    for (fpe_gauge *g = g_gauges; g && n < max; g = g->next) {
        memset(&out[n], 0, sizeof out[n]);
        out[n].name = g->name;
        out[n].is_gauge = 1;
        out[n].value = g->value;
        out[n].peak = g->peak;
        n++;
    }
    return n;
}

/* ---- text report ------------------------------------------------------------ */

typedef struct outbuf {
    char  *p;
    size_t cap, len;
} outbuf;

static void put(outbuf *o, const char *fmt, ...)
{
    if (o->len + 1 >= o->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->p + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    o->len += (size_t)n < o->cap - o->len ? (size_t)n : o->cap - o->len - 1;
}

static size_t group_len(const char *name)
{
    const char *dot = strchr(name, '.');
    return dot ? (size_t)(dot - name) : strlen(name);
}

static void fmt_bytes(char *b, size_t cap, int64_t v)
{
    static const char *u[] = { "B", "KB", "MB", "GB" };
    double d = (double)(v < 0 ? -v : v);
    int i = 0;
    while (d >= 1024 && i < 3) { d /= 1024; i++; }
    if (i == 0) snprintf(b, cap, "%s%lld B", v < 0 ? "-" : "", (long long)(v < 0 ? -v : v));
    else snprintf(b, cap, "%s%.1f %s", v < 0 ? "-" : "", d, u[i]);
}

static int ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

typedef struct group {
    const char *name;
    size_t      len;
    double      total;
} group;

static const fpe_prof_row *g_sort_rows;

static int by_total_desc(const void *a, const void *b)
{
    double x = g_sort_rows[*(const uint32_t *)a].total_ms, y = g_sort_rows[*(const uint32_t *)b].total_ms;
    return (x < y) - (x > y);
}

static int group_by_total_desc(const void *a, const void *b)
{
    double x = ((const group *)a)->total, y = ((const group *)b)->total;
    return (x < y) - (x > y);
}

static int by_name(const void *a, const void *b)
{
    return strcmp(g_sort_rows[*(const uint32_t *)a].name, g_sort_rows[*(const uint32_t *)b].name);
}

/* "a.b/c" is a direct child of "a.b"; "a.b/c/d" is not. */
static int is_child(const char *name, const char *parent)
{
    size_t lp = strlen(parent);
    return strncmp(name, parent, lp) == 0 && name[lp] == '/' && !strchr(name + lp + 1, '/');
}

static void put_zone(outbuf *o, const fpe_prof_row *rows, uint32_t n, uint32_t at, int depth, uint32_t *scratch)
{
    const fpe_prof_row *r = &rows[at];
    uint32_t m = 0;
    double kids = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!rows[i].is_gauge && is_child(rows[i].name, r->name)) {
            scratch[m++] = i;
            kids += rows[i].total_ms;
        }

    char self[16] = "";
    if (m) snprintf(self, sizeof self, "%.2f", r->total_ms - kids > 0 ? r->total_ms - kids : 0.0);
    /* nested rows show only their own step ("/query"); indentation says whose */
    const char *shown = depth ? strrchr(r->name, '/') : r->name;
    int indent = 2 + depth * 2;
    if (r->total_ms == 0 && r->max_ms == 0) { /* fpe_prof_count: calls only */
        put(o, "%*s%-*.*s %8lld\n", indent, "", 30 - indent, 30 - indent, shown, (long long)r->calls);
        return;
    }
    put(o, "%*s%-*.*s %8lld %10.2f %10s %9.1f %9.1f\n", indent, "", 30 - indent, 30 - indent, shown,
        (long long)r->calls, r->total_ms, self, r->total_ms * 1000.0 / (double)r->calls, r->max_ms * 1000.0);

    if (!m) return;
    uint32_t *mine = malloc(m * sizeof *mine);
    if (!mine) return;
    memcpy(mine, scratch, m * sizeof *mine);
    qsort(mine, m, sizeof *mine, by_total_desc);
    for (uint32_t k = 0; k < m; k++) put_zone(o, rows, n, mine[k], depth + 1, scratch);
    free(mine);
}

FPE_API size_t fpe_prof_format(char *buf, size_t cap)
{
    if (!buf || !cap) return 0;
    buf[0] = 0;
    enum { MAXR = 512, MAXG = 64 };
    fpe_prof_row *rows = malloc(MAXR * sizeof *rows);
    uint32_t *idx = malloc(MAXR * sizeof *idx);
    group *groups = malloc(MAXG * sizeof *groups);
    outbuf o = { buf, cap, 0 };
    if (!rows || !idx || !groups) {
        free(rows); free(idx); free(groups);
        return 0;
    }

    AcquireSRWLockExclusive(&g_format_lock);
    uint32_t n = fpe_prof_rows(rows, MAXR), ng = 0, nz = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (rows[i].is_gauge) continue;
        size_t gl = group_len(rows[i].name);
        uint32_t k = 0;
        while (k < ng && !(groups[k].len == gl && strncmp(groups[k].name, rows[i].name, gl) == 0)) k++;
        if (k == ng && ng < MAXG) {
            groups[ng].name = rows[i].name;
            groups[ng].len = gl;
            groups[ng].total = 0;
            ng++;
        }
        /* sub-steps are already inside their parent's time */
        if (k < ng && !strchr(rows[i].name, '/')) groups[k].total += rows[i].total_ms;
        nz++;
    }
    qsort(groups, ng, sizeof *groups, group_by_total_desc);

    uint32_t *scratch = malloc(MAXR * sizeof *scratch);
    put(&o, "profiling %s   (times inclusive; self = total minus nested sub-steps)\n", g_enabled ? "on" : "off");
    put(&o, "%-30s %8s %10s %10s %9s %9s\n", "zone", "calls", "total ms", "self ms", "avg us", "max us");
    if (!nz) put(&o, "  (no zones recorded)\n");
    g_sort_rows = rows;
    for (uint32_t gi = 0; gi < ng && scratch; gi++) {
        int gl = groups[gi].len > 30 ? 30 : (int)groups[gi].len;
        put(&o, "%-.*s%*s %8s %10.2f\n", gl, groups[gi].name, 30 - gl, "", "", groups[gi].total);
        uint32_t m = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (rows[i].is_gauge || group_len(rows[i].name) != groups[gi].len ||
                strncmp(rows[i].name, groups[gi].name, groups[gi].len) != 0)
                continue;
            /* top level: no '/', or a sub-step whose parent was never recorded */
            const char *slash = strrchr(rows[i].name, '/');
            int orphan = 0;
            if (slash) {
                orphan = 1;
                size_t pl = (size_t)(slash - rows[i].name);
                for (uint32_t p = 0; p < n && orphan; p++)
                    if (!rows[p].is_gauge && strlen(rows[p].name) == pl && strncmp(rows[p].name, rows[i].name, pl) == 0)
                        orphan = 0;
            }
            if (!slash || orphan) idx[m++] = i;
        }
        qsort(idx, m, sizeof *idx, by_total_desc);
        for (uint32_t k = 0; k < m; k++) put_zone(&o, rows, n, idx[k], 0, scratch);
    }
    free(scratch);

    uint32_t m = 0;
    for (uint32_t i = 0; i < n; i++)
        if (rows[i].is_gauge) idx[m++] = i;
    qsort(idx, m, sizeof *idx, by_name);
    put(&o, "\n%-30s %12s %12s\n", "gauge", "now", "peak");
    for (uint32_t k = 0; k < m; k++) {
        const fpe_prof_row *r = &rows[idx[k]];
        char v[32], pk[32];
        if (ends_with(r->name, ".bytes")) {
            fmt_bytes(v, sizeof v, r->value);
            fmt_bytes(pk, sizeof pk, r->peak);
        } else {
            snprintf(v, sizeof v, "%lld", (long long)r->value);
            snprintf(pk, sizeof pk, "%lld", (long long)r->peak);
        }
        put(&o, "  %-28s %12s %12s\n", r->name, v, pk);
    }
    ReleaseSRWLockExclusive(&g_format_lock);

    free(rows);
    free(idx);
    free(groups);
    return o.len;
}
