#include "namecodec.h"
#include "internal.h"

struct fpe_view {
    fpe_dir      *dir;
    uint64_t revision;
    fpe_view_opts opts;
    uint32_t     *order; /* every entry, sorted */
    uint32_t     *vis;   /* rows that pass hidden/filter checks, in sort order */
    uint32_t     *tmp;
    uint32_t      n;
    uint32_t      nvis;
    wchar_t      *needle; /* folded */
    uint32_t      needle_len;
    int64_t       accounted; /* bytes reported to the view gauge */
};

static volatile LONG64 g_revision;
static void revised(fpe_view *v) { v->revision=(uint64_t)InterlockedIncrement64(&g_revision); }
static fpe_gauge g_views = FPE_GAUGE_INIT("view.live");
static fpe_gauge g_view_bytes = FPE_GAUGE_INIT("view.bytes");

/* ---- sorting --------------------------------------------------------------- */

typedef struct sort_ctx {
    const fpe_entry *e;
    const wchar_t   *names;
    fpe_sort_key     key;
    int              desc;
    int              dirs_first;
} sort_ctx;

static inline int cmp3_u64(uint64_t a, uint64_t b) { return (a > b) - (a < b); }
static inline int cmp3_i64(int64_t a, int64_t b) { return (a > b) - (a < b); }

static int cmp_entries(const sort_ctx *c, uint32_t ia, uint32_t ib)
{
    const fpe_entry *a = c->e + ia, *b = c->e + ib;

    if (c->dirs_first) {
        int da = fpe_entry_is_dir(a), db = fpe_entry_is_dir(b);
        if (da != db) return db - da;
    }

    int ascii = (a->flags & b->flags & FPE_ENTRY_ASCII) != 0;
    int r = 0;
    switch (c->key) {
    case FPE_SORT_EXT:
        r = fpe_name_cmp(c->names + a->name + a->ext, (uint32_t)(a->name_len - a->ext), ascii,
                         c->names + b->name + b->ext, (uint32_t)(b->name_len - b->ext), ascii);
        break;
    case FPE_SORT_SIZE:  r = cmp3_u64(a->size, b->size); break;
    case FPE_SORT_MTIME: r = cmp3_i64(a->mtime, b->mtime); break;
    case FPE_SORT_CTIME: r = cmp3_i64(a->ctime, b->ctime); break;
    default: break;
    }
    if (r) return c->desc ? -r : r;

    r = fpe_name_cmp(c->names + a->name, a->name_len, (a->flags & FPE_ENTRY_ASCII) != 0,
                     c->names + b->name, b->name_len, (b->flags & FPE_ENTRY_ASCII) != 0);
    if (c->key == FPE_SORT_NAME && c->desc) r = -r;
    if (r) return r;
    return (ia > ib) - (ia < ib);
}

static void insertion_sort(uint32_t *a, uint32_t n, const sort_ctx *c)
{
    for (uint32_t i = 1; i < n; i++) {
        uint32_t v = a[i], j = i;
        while (j > 0 && cmp_entries(c, a[j - 1], v) > 0) { a[j] = a[j - 1]; j--; }
        a[j] = v;
    }
}

/* Strictly descending runs can be reversed without breaking stability. */
static int reverse_if_descending(uint32_t *a, uint32_t n, const sort_ctx *c)
{
    for (uint32_t i = 1; i < n; i++)
        if (cmp_entries(c, a[i - 1], a[i]) <= 0) return 0;
    for (uint32_t i = 0, j = n - 1; i < j; i++, j--) {
        uint32_t t = a[i]; a[i] = a[j]; a[j] = t;
    }
    return 1;
}

/* Bottom-up merge sort over indices: stable, O(n log n) worst case, no
 * recursion, and the entries themselves never move. Directory listings
 * arrive close to name order, so input that is already sorted either way
 * (name ascending or descending) takes about n comparisons. */
static void sort_indices(uint32_t *a, uint32_t *tmp, uint32_t n, const sort_ctx *c)
{
    const uint32_t run = 24;
    for (uint32_t s = 0; s < n; s += run) {
        uint32_t len = n - s < run ? n - s : run;
        if (!reverse_if_descending(a + s, len, c)) insertion_sort(a + s, len, c);
    }

    uint32_t *src = a, *dst = tmp;
    for (uint64_t w = run; w < n; w *= 2) {
        for (uint64_t lo = 0; lo < n; lo += 2 * w) {
            uint32_t mid = (uint32_t)(lo + w < n ? lo + w : n);
            uint32_t hi = (uint32_t)(lo + 2 * w < n ? lo + 2 * w : n);
            uint32_t i = (uint32_t)lo, j = mid, k = (uint32_t)lo;
            if (mid < hi && cmp_entries(c, src[mid - 1], src[mid]) <= 0) {
                memcpy(dst + lo, src + lo, (hi - lo) * sizeof(uint32_t));
                continue;
            }
            if (mid < hi && cmp_entries(c, src[hi - 1], src[lo]) < 0) { /* whole right half goes first */
                memcpy(dst + lo, src + mid, (hi - mid) * sizeof(uint32_t));
                memcpy(dst + lo + (hi - mid), src + lo, (mid - lo) * sizeof(uint32_t));
                continue;
            }
            while (i < mid && j < hi) dst[k++] = cmp_entries(c, src[j], src[i]) < 0 ? src[j++] : src[i++];
            while (i < mid) dst[k++] = src[i++];
            while (j < hi) dst[k++] = src[j++];
        }
        uint32_t *t = src; src = dst; dst = t;
    }
    if (src != a) memcpy(a, src, n * sizeof(uint32_t));
}

/* ---- filtering ------------------------------------------------------------- */

static int passes_hidden(const fpe_view *v, const fpe_entry *e)
{
    if (!(e->attrs & FILE_ATTRIBUTE_HIDDEN)) return 1;
    if (!v->opts.show_hidden) return 0;
    if ((e->attrs & FILE_ATTRIBUTE_SYSTEM) && !v->opts.show_system) return 0;
    return 1;
}

static void rebuild_vis(fpe_view *v, int narrowing)
{
    int64_t t = fpe_prof_begin();
    const fpe_entry *ents = fpe_dir_entries(v->dir);
    const wchar_t *names = fpe_dir_names(v->dir);
    const uint32_t *src = narrowing ? v->vis : v->order;
    uint32_t n = narrowing ? v->nvis : v->n, k = 0;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = src[i];
        const fpe_entry *e = ents + idx;
        if (!narrowing && !passes_hidden(v, e)) continue;
        if(v->needle_len){
            const wchar_t *name=names+e->name;size_t length=e->name_len;wchar_t literal[256];
            if(fpe_dir_is_linux(v->dir)&&!(e->flags&FPE_ENTRY_ASCII)){
                if(!fpe_lx_literal(name,literal,256,0))continue;name=literal;length=wcslen(literal);
            }
            if(!fpe_contains_folded(name,(uint32_t)length,v->needle,v->needle_len))continue;
        }
        v->vis[k++] = idx; /* k <= i, so filtering in place is safe */
    }
    v->nvis = k;
    FPE_PROF_END("view.filter", t);
}

static void rebuild_order(fpe_view *v)
{
    int64_t t = fpe_prof_begin();
    for (uint32_t i = 0; i < v->n; i++) v->order[i] = i;
    sort_ctx c = {
        fpe_dir_entries(v->dir),
        fpe_dir_names(v->dir),
        v->opts.key,
        v->opts.descending != 0,
        v->opts.dirs_first != 0,
    };
    /* Ties are broken by entry index, so the starting order never changes the
     * result, only the speed: start reversed when the input mostly runs the
     * other way (a descending sort of a name-ordered listing). */
    uint32_t up = 0, down = 0, step = v->n / 64 + 1;
    for (uint32_t i = 0; i + 1 < v->n; i += step) {
        if (cmp_entries(&c, i, i + 1) <= 0) up++;
        else down++;
    }
    if (down > up)
        for (uint32_t i = 0; i < v->n; i++) v->order[i] = v->n - 1 - i;
    sort_indices(v->order, v->tmp, v->n, &c);
    FPE_PROF_END("view.sort", t);
}

/* ---- public ------------------------------------------------------------------ */

static fpe_err alloc_arrays(fpe_view *v, uint32_t n)
{
    size_t bytes = (size_t)(n ? n : 1) * sizeof(uint32_t);
    uint32_t *o = fpe_realloc(v->order, bytes);
    if (!o) return ERROR_NOT_ENOUGH_MEMORY;
    v->order = o;
    uint32_t *vi = fpe_realloc(v->vis, bytes);
    if (!vi) return ERROR_NOT_ENOUGH_MEMORY;
    v->vis = vi;
    uint32_t *t = fpe_realloc(v->tmp, bytes);
    if (!t) return ERROR_NOT_ENOUGH_MEMORY;
    v->tmp = t;
    int64_t now = (int64_t)bytes * 3;
    fpe_gauge_add(&g_view_bytes, now - v->accounted);
    v->accounted = now;
    return 0;
}

FPE_API fpe_view *fpe_view_create(fpe_dir *dir, const fpe_view_opts *opts)
{
    if (!dir) return NULL;
    fpe_text_init();
    fpe_view *v = fpe_zalloc(sizeof *v);
    if (!v) return NULL;
    if (opts) {
        v->opts = *opts;
    } else {
        v->opts.key = FPE_SORT_NAME;
        v->opts.dirs_first = 1;
    }
    v->dir = fpe_dir_retain(dir);
    v->n = fpe_dir_count(dir);
    fpe_gauge_add(&g_views, 1);
    if (alloc_arrays(v, v->n)) { fpe_view_destroy(v); return NULL; }
    rebuild_order(v);
    rebuild_vis(v, 0);
    revised(v);
    return v;
}

FPE_API void fpe_view_destroy(fpe_view *v)
{
    if (!v) return;
    fpe_gauge_add(&g_views, -1);
    fpe_gauge_add(&g_view_bytes, -v->accounted);
    fpe_dir_release(v->dir);
    fpe_free(v->order);
    fpe_free(v->vis);
    fpe_free(v->tmp);
    fpe_free(v->needle);
    fpe_free(v);
}

FPE_API fpe_err fpe_view_set_dir(fpe_view *v, fpe_dir *dir)
{
    if (!v || !dir) return ERROR_INVALID_PARAMETER;
    uint32_t n = fpe_dir_count(dir);
    fpe_err err = alloc_arrays(v, n);
    if (err) return err;
    fpe_dir_retain(dir);
    fpe_dir_release(v->dir);
    v->dir = dir;
    v->n = n;
    rebuild_order(v);
    rebuild_vis(v, 0);
    revised(v);
    return 0;
}

FPE_API void fpe_view_set_opts(fpe_view *v, const fpe_view_opts *o)
{
    int resort = o->key != v->opts.key || !!o->descending != !!v->opts.descending ||
                 !!o->dirs_first != !!v->opts.dirs_first;
    int refilter = !!o->show_hidden != !!v->opts.show_hidden ||
                   !!o->show_system != !!v->opts.show_system;
    v->opts = *o;
    if (resort) rebuild_order(v);
    if (resort || refilter) { rebuild_vis(v, 0); revised(v); }
}

FPE_API void fpe_view_get_opts(const fpe_view *v, fpe_view_opts *out) { *out = v->opts; }

FPE_API fpe_err fpe_view_set_filter(fpe_view *v, const wchar_t *needle)
{
    uint32_t nn = needle ? (uint32_t)wcslen(needle) : 0;
    wchar_t *folded = NULL;
    if (nn) {
        folded = fpe_alloc((nn + 1) * sizeof(wchar_t));
        if (!folded) return ERROR_NOT_ENOUGH_MEMORY;
        for (uint32_t i = 0; i < nn; i++) folded[i] = fpe_fold(needle[i]);
        folded[nn] = 0;
    }

    /* Anything matching the new needle also matched the old one if the new
     * needle contains the old, so only the current rows need rescanning. */
    int narrowing = nn > 0 && (v->needle_len == 0 ||
                               fpe_contains_folded(folded, nn, v->needle, v->needle_len));

    if (nn == v->needle_len && (nn == 0 || memcmp(folded, v->needle, nn * sizeof(wchar_t)) == 0)) {
        fpe_free(folded);
        return 0;
    }

    fpe_free(v->needle);
    v->needle = folded;
    v->needle_len = nn;
    rebuild_vis(v, narrowing);
    revised(v);
    return 0;
}

FPE_API fpe_dir *fpe_view_dir(const fpe_view *v) { return v->dir; }
FPE_API uint32_t fpe_view_count(const fpe_view *v) { return v->nvis; }

FPE_API const fpe_entry *fpe_view_at(const fpe_view *v, uint32_t i)
{
    return i < v->nvis ? fpe_dir_entries(v->dir) + v->vis[i] : NULL;
}

FPE_API uint32_t fpe_view_entry_index(const fpe_view *v, uint32_t i)
{
    return i < v->nvis ? v->vis[i] : UINT32_MAX;
}

FPE_API const wchar_t *fpe_view_name(const fpe_view *v, uint32_t i)
{
    const fpe_entry *e = fpe_view_at(v, i);
    return e ? fpe_dir_name(v->dir, e) : NULL;
}

FPE_API uint64_t fpe_view_revision(const fpe_view *v) { return v?v->revision:0; }
FPE_API fpe_err fpe_view_copy_order(const fpe_view *v,uint32_t *indices,uint32_t capacity)
{
    if(!v||(!indices&&v->nvis))return ERROR_INVALID_PARAMETER;
    if(capacity<v->nvis)return ERROR_INSUFFICIENT_BUFFER;
    if(v->nvis)memcpy(indices,v->vis,(size_t)v->nvis*sizeof *indices);
    return 0;
}

FPE_API uint32_t fpe_view_find(const fpe_view *v, const wchar_t *name)
{
    if (!name) return UINT32_MAX;
    int64_t t = fpe_prof_begin();
    uint32_t nn = (uint32_t)wcslen(name), found = UINT32_MAX;
    int exact=fpe_dir_is_linux(v->dir)||fpe_wsl_path_kind(fpe_dir_path(v->dir));
    const fpe_entry *ents = fpe_dir_entries(v->dir);
    for (uint32_t i = 0; i < v->nvis; i++) {
        const fpe_entry *e = ents + v->vis[i];
        if (e->name_len == nn && (exact?!wmemcmp(fpe_dir_name(v->dir,e),name,nn):fpe_equal_nocase(fpe_dir_name(v->dir, e), e->name_len, name, nn))) {
            found = i;
            break;
        }
    }
    FPE_PROF_END("view.find", t);
    return found;
}
