#include "internal.h"

typedef struct cache_slot {
    wchar_t  *key; /* display path */
    uint32_t  hash;
    fpe_dir  *dir;
    uint64_t  bytes;
    uint64_t  stamp;
} cache_slot;

typedef struct load_req {
    struct load_req *next;
    uint64_t         ticket;
    wchar_t         *path;
    fpe_load_cb      cb;
    void            *user;
} load_req;

struct fpe_engine {
    SRWLOCK     lock;
    cache_slot *slots;
    uint32_t    nslots;
    uint32_t    max_slots;
    uint64_t    max_bytes;
    uint64_t    bytes;
    uint64_t    clock;

    /* One threadpool work object; each submit queues one request and one
     * callback pops it. WaitForThreadpoolWorkCallbacks then gives a clean
     * shutdown without per-request bookkeeping. */
    PTP_WORK    work;
    load_req   *qhead;
    load_req   *qtail;
    volatile LONG64 next_ticket;

    fpe_jobs    jobs;
};

static fpe_gauge g_cache_dirs = FPE_GAUGE_INIT("engine.cache.dirs");
static fpe_gauge g_cache_bytes = FPE_GAUGE_INIT("engine.cache.bytes");
static fpe_gauge g_queue = FPE_GAUGE_INIT("engine.load.queue");

fpe_jobs *fpe_engine_jobs(fpe_engine *e) { return &e->jobs; }

/* ---- cache (caller holds lock) --------------------------------------------- */

static int slot_find(fpe_engine *e, const wchar_t *key, uint32_t hash)
{
    for (uint32_t i = 0; i < e->nslots; i++) {
        cache_slot *s = &e->slots[i];
        if (s->hash == hash && fpe_path_equal(s->key,key)) return (int)i;
    }
    return -1;
}

static void slot_remove(fpe_engine *e, uint32_t i)
{
    cache_slot *s = &e->slots[i];
    e->bytes -= s->bytes;
    fpe_gauge_add(&g_cache_dirs, -1);
    fpe_gauge_add(&g_cache_bytes, -(int64_t)s->bytes);
    fpe_dir_release(s->dir);
    fpe_free(s->key);
    e->slots[i] = e->slots[--e->nslots];
}

static void cache_put(fpe_engine *e, const wchar_t *key, fpe_dir *dir)
{
    uint32_t hash = fpe_path_hash(key);
    fpe_dir_stats st;
    fpe_dir_get_stats(dir, &st);

    int i = slot_find(e, key, hash);
    if (i >= 0) {
        cache_slot *s = &e->slots[i];
        e->bytes -= s->bytes;
        fpe_gauge_add(&g_cache_bytes, (int64_t)st.memory_bytes - (int64_t)s->bytes);
        fpe_dir_release(s->dir);
        s->dir = fpe_dir_retain(dir);
        s->bytes = st.memory_bytes;
        s->stamp = ++e->clock;
        e->bytes += s->bytes;
    } else {
        wchar_t *k = fpe_wcsdup(key);
        if (!k) return;
        if (e->nslots == e->max_slots) {
            /* evict LRU to make room */
            uint32_t lru = 0;
            for (uint32_t j = 1; j < e->nslots; j++)
                if (e->slots[j].stamp < e->slots[lru].stamp) lru = j;
            slot_remove(e, lru);
        }
        cache_slot *s = &e->slots[e->nslots++];
        s->key = k;
        s->hash = hash;
        s->dir = fpe_dir_retain(dir);
        s->bytes = st.memory_bytes;
        s->stamp = ++e->clock;
        e->bytes += s->bytes;
        fpe_gauge_add(&g_cache_dirs, 1);
        fpe_gauge_add(&g_cache_bytes, (int64_t)s->bytes);
    }

    while (e->bytes > e->max_bytes && e->nslots > 1) {
        uint32_t lru = UINT32_MAX;
        for (uint32_t j = 0; j < e->nslots; j++) {
            if (e->slots[j].dir == dir) continue; /* keep what was just loaded */
            if (lru == UINT32_MAX || e->slots[j].stamp < e->slots[lru].stamp) lru = j;
        }
        if (lru == UINT32_MAX) break;
        slot_remove(e, lru);
    }
}

/* ---- async loads ------------------------------------------------------------- */

static void CALLBACK load_work(PTP_CALLBACK_INSTANCE inst, PVOID ctx, PTP_WORK work)
{
    (void)inst; (void)work;
    fpe_engine *e = ctx;

    AcquireSRWLockExclusive(&e->lock);
    load_req *r = e->qhead;
    if (r) {
        e->qhead = r->next;
        if (!e->qhead) e->qtail = NULL;
    }
    ReleaseSRWLockExclusive(&e->lock);
    if (!r) return;
    fpe_gauge_add(&g_queue, -1);
    int64_t t = fpe_prof_begin();

    wchar_t *disp = NULL, *nt = NULL;
    fpe_dir *dir = NULL;
    fpe_err err = fpe_path_display(r->path, &disp);
    if (!err) err = fpe_path_nt(disp, &nt);
    if (!err) err = fpe_dir_load(disp, nt, &dir);

    if (disp) {
        AcquireSRWLockExclusive(&e->lock);
        if (!err) {
            cache_put(e, disp, dir);
        } else if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ||
                   err == ERROR_DIRECTORY || err == ERROR_INVALID_NAME) {
            int i = slot_find(e, disp, fpe_path_hash(disp));
            if (i >= 0) slot_remove(e, (uint32_t)i);
        }
        ReleaseSRWLockExclusive(&e->lock);
    }

    FPE_PROF_END("engine.load", t);
    if (r->cb) r->cb(r->user, r->ticket, err, dir);
    else fpe_dir_release(dir);

    fpe_free(disp);
    fpe_free(nt);
    fpe_free(r->path);
    fpe_free(r);
}

/* ---- public ------------------------------------------------------------------ */

FPE_API fpe_engine *fpe_engine_create(const fpe_engine_config *cfg)
{
    fpe_text_init();
    fpe_engine *e = fpe_zalloc(sizeof *e);
    if (!e) return NULL;
    InitializeSRWLock(&e->lock);
    e->max_slots = cfg && cfg->cache_dirs ? cfg->cache_dirs : 64;
    e->max_bytes = cfg && cfg->cache_bytes ? cfg->cache_bytes : 64ull * 1024 * 1024;
    e->slots = fpe_zalloc(e->max_slots * sizeof(cache_slot));
    e->work = CreateThreadpoolWork(load_work, e, NULL);
    if (!e->slots || !e->work) {
        if (e->work) CloseThreadpoolWork(e->work);
        fpe_free(e->slots);
        fpe_free(e);
        return NULL;
    }
    fpe_jobs_init(&e->jobs);
    return e;
}

FPE_API void fpe_engine_destroy(fpe_engine *e)
{
    if (!e) return;
    fpe_jobs_shutdown(&e->jobs);
    fpe_wsl_shutdown();
    WaitForThreadpoolWorkCallbacks(e->work, FALSE);
    CloseThreadpoolWork(e->work);
    while (e->nslots) slot_remove(e, e->nslots - 1);
    fpe_free(e->slots);
    fpe_free(e);
}

FPE_API fpe_dir *fpe_engine_cached(fpe_engine *e, const wchar_t *path)
{
    int64_t t = fpe_prof_begin();
    wchar_t *disp = NULL;
    if (fpe_path_display(path, &disp)) return NULL;
    fpe_dir *d = NULL;
    AcquireSRWLockExclusive(&e->lock);
    int i = slot_find(e, disp, fpe_path_hash(disp));
    if (i >= 0) {
        e->slots[i].stamp = ++e->clock;
        d = fpe_dir_retain(e->slots[i].dir);
    }
    ReleaseSRWLockExclusive(&e->lock);
    fpe_free(disp);
    if (d) FPE_PROF_END("engine.cache.hit", t);
    else FPE_PROF_END("engine.cache.miss", t);
    return d;
}

FPE_API uint64_t fpe_engine_load(fpe_engine *e, const wchar_t *path, fpe_load_cb cb, void *user)
{
    if (!e || !path || !path[0]) return 0;
    load_req *r = fpe_zalloc(sizeof *r);
    if (!r) return 0;
    r->path = fpe_wcsdup(path);
    if (!r->path) { fpe_free(r); return 0; }
    r->ticket = (uint64_t)InterlockedIncrement64(&e->next_ticket);
    r->cb = cb;
    r->user = user;
    uint64_t ticket = r->ticket;

    AcquireSRWLockExclusive(&e->lock);
    if (e->qtail) e->qtail->next = r;
    else e->qhead = r;
    e->qtail = r;
    ReleaseSRWLockExclusive(&e->lock);
    fpe_gauge_add(&g_queue, 1);

    SubmitThreadpoolWork(e->work);
    return ticket;
}

FPE_API void fpe_engine_invalidate(fpe_engine *e, const wchar_t *path)
{
    wchar_t *disp = NULL;
    if (fpe_path_display(path, &disp)) return;
    AcquireSRWLockExclusive(&e->lock);
    int i = slot_find(e, disp, fpe_path_hash(disp));
    if (i >= 0) slot_remove(e, (uint32_t)i);
    ReleaseSRWLockExclusive(&e->lock);
    fpe_free(disp);
}
