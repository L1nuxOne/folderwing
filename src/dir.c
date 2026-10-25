#include "namecodec.h"
#include "internal.h"

struct fpe_dir {
    volatile LONG refs;
    wchar_t      *path;
    fpe_vbuf      entries;
    fpe_vbuf      names;
    uint32_t      count;
    uint32_t      dirs;
    uint64_t      file_bytes;
    uint64_t      read_us;
    int64_t       taken_at;
    int64_t       accounted; /* bytes reported to the snapshot gauge */
    /* Linux listings (WSL) only: one unix_rec per entry, and owner names */
    fpe_vbuf      unix;
    fpe_vbuf      owners;
    uint32_t      nowners;
};

typedef struct unix_rec {
    uint32_t mode, uid, gid;
    uint32_t link; /* symlink target: offset into the name pool, UINT32_MAX if none */
} unix_rec;

typedef struct owner_rec {
    uint32_t id, group;
    uint32_t name; /* offset into the name pool */
} owner_rec;

static fpe_gauge g_snapshots = FPE_GAUGE_INIT("dir.snapshots");
static fpe_gauge g_snapshot_bytes = FPE_GAUGE_INIT("dir.snapshots.bytes");

fpe_err fpe_enum(const wchar_t *nt_path, uint8_t *buf, uint32_t cap, fpe_enum_fn fn, void *ctx)
{
    int64_t t_open = fpe_prof_begin();
    HANDLE h = CreateFileW(nt_path, FILE_LIST_DIRECTORY,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    FPE_PROF_END("dir.read/open", t_open);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();

    /* One call returns as many entries as fit in buf, which is what makes
     * this much cheaper than FindFirstFile/FindNextFile per entry. */
    FILE_INFO_BY_HANDLE_CLASS cls = FileFullDirectoryRestartInfo;
    fpe_err err = 0;
    for (;;) {
        /* dir.read/query is pure filesystem time; the report shows dir.read's
         * "self" time, which is what the engine itself adds. */
        int64_t t_query = fpe_prof_begin();
        BOOL ok = GetFileInformationByHandleEx(h, cls, buf, cap);
        FPE_PROF_END("dir.read/query", t_query);
        if (!ok) {
            err = GetLastError();
            /* Some SMB servers reject query buffers above 64 KiB. */
            if (err == ERROR_INVALID_PARAMETER && cls == FileFullDirectoryRestartInfo && cap > 64u * 1024u) {
                cap = 64u * 1024u;
                err = 0;
                continue;
            }
            if (err == ERROR_NO_MORE_FILES || err == ERROR_FILE_NOT_FOUND) err = 0;
            break;
        }
        cls = FileFullDirectoryInfo;
        const uint8_t *p = buf;
        for (;;) {
            const FILE_FULL_DIR_INFO *fi = (const FILE_FULL_DIR_INFO *)p;
            if (!fn(ctx, fi)) goto done;
            if (!fi->NextEntryOffset) break;
            p += fi->NextEntryOffset;
        }
    }
done:
    CloseHandle(h);
    return err;
}

typedef struct read_ctx {
    fpe_dir *d;
    fpe_err  err;
    int linux;
} read_ctx;

/* ---- building a snapshot ------------------------------------------------------- */

fpe_dir *fpe_dir_new(const wchar_t *display, size_t reserve_hint)
{
    fpe_text_init();
    fpe_dir *d = fpe_zalloc(sizeof *d);
    if (!d) return NULL;
    d->refs = 1;
    d->path = fpe_wcsdup(display);
    d->read_us = fpe_now_us(); /* the start, until fpe_dir_finish */

    /* Reserving address space is free on x64 and avoids relocating (and
     * re-faulting) the buffers while a big folder streams in. */
    size_t ent_reserve = reserve_hint ? reserve_hint : 32u * 1024 * 1024;
    size_t name_reserve = reserve_hint ? reserve_hint : 64u * 1024 * 1024;
    if (!d->path || !fpe_vbuf_init(&d->entries, ent_reserve) || !fpe_vbuf_init(&d->names, name_reserve)) {
        fpe_dir_release(d);
        return NULL;
    }
    return d;
}

static uint32_t pool_add(fpe_dir *d, const wchar_t *s, uint32_t n)
{
    wchar_t *dst = fpe_vbuf_push(&d->names, (n + 1) * sizeof(wchar_t));
    if (!dst) return UINT32_MAX;
    memcpy(dst, s, n * sizeof(wchar_t));
    dst[n] = 0;
    return (uint32_t)(d->names.used / sizeof(wchar_t) - (n + 1));
}

fpe_err fpe_dir_add(fpe_dir *d, const wchar_t *nm, uint32_t n, uint64_t size, int64_t mtime, int64_t ctime,
                    uint32_t attrs, uint32_t reparse_tag)
{
    if (n == 0 || n > 0xFFFF) return 0;
    fpe_entry *e = fpe_vbuf_push(&d->entries, sizeof *e);
    uint32_t at = e ? pool_add(d, nm, n) : UINT32_MAX;
    if (at == UINT32_MAX) return ERROR_NOT_ENOUGH_MEMORY;

    int is_dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    uint32_t ext = n;
    if (!is_dir) {
        for (uint32_t i = n; i-- > 0;) {
            if (nm[i] == L'.') { ext = i + 1; break; }
        }
    }

    e->size = is_dir ? 0 : size;
    e->mtime = mtime;
    e->ctime = ctime;
    e->attrs = attrs;
    e->reparse_tag = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) ? reparse_tag : 0;
    e->name = at;
    e->name_len = (uint16_t)n;
    e->ext = (uint16_t)ext;
    e->flags = fpe_is_ascii(nm, n) ? FPE_ENTRY_ASCII : 0;
    e->reserved = 0;

    d->count++;
    if (is_dir) d->dirs++;
    else d->file_bytes += e->size;
    return 0;
}

fpe_err fpe_dir_add_unix(fpe_dir *d, uint32_t mode, uint32_t uid, uint32_t gid, const wchar_t *link, uint32_t link_len)
{
    if (!d->unix.base && !fpe_vbuf_init(&d->unix, 16u * 1024 * 1024)) return ERROR_NOT_ENOUGH_MEMORY;
    /* one record per entry, in step with the entries */
    while (d->unix.used / sizeof(unix_rec) + 1 < d->count) {
        unix_rec *gap = fpe_vbuf_push(&d->unix, sizeof *gap);
        if (!gap) return ERROR_NOT_ENOUGH_MEMORY;
        *gap = (unix_rec){ 0, 0, 0, UINT32_MAX };
    }
    unix_rec *r = fpe_vbuf_push(&d->unix, sizeof *r);
    if (!r) return ERROR_NOT_ENOUGH_MEMORY;
    r->mode = mode;
    r->uid = uid;
    r->gid = gid;
    r->link = link ? pool_add(d, link, link_len) : UINT32_MAX;
    return 0;
}

fpe_err fpe_dir_add_owner(fpe_dir *d, uint32_t id, int group, const wchar_t *name, uint32_t n)
{
    if (!d->owners.base && !fpe_vbuf_init(&d->owners, 1u << 20)) return ERROR_NOT_ENOUGH_MEMORY;
    owner_rec *o = fpe_vbuf_push(&d->owners, sizeof *o);
    uint32_t at = o ? pool_add(d, name, n) : UINT32_MAX;
    if (at == UINT32_MAX) return ERROR_NOT_ENOUGH_MEMORY;
    *o = (owner_rec){ id, group ? 1u : 0u, at };
    d->nowners++;
    return 0;
}

fpe_err fpe_dir_finish(fpe_dir *d)
{
    /* Guarantee a valid (empty) name pool for empty folders. */
    if (!d->names.base || !d->names.committed) {
        wchar_t *z = fpe_vbuf_push(&d->names, sizeof(wchar_t));
        if (!z) return ERROR_NOT_ENOUGH_MEMORY;
        *z = 0;
    }
    fpe_vbuf_trim(&d->entries);
    fpe_vbuf_trim(&d->names);
    if (d->unix.base) fpe_vbuf_trim(&d->unix);
    if (d->owners.base) fpe_vbuf_trim(&d->owners);

    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    d->taken_at = ((int64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    d->read_us = fpe_now_us() - d->read_us;

    fpe_dir_stats st;
    fpe_dir_get_stats(d, &st);
    d->accounted = (int64_t)st.memory_bytes;
    fpe_gauge_add(&g_snapshots, 1);
    fpe_gauge_add(&g_snapshot_bytes, d->accounted);
    return 0;
}

/* ---- reading a Windows folder ---------------------------------------------------- */

static int on_entry(void *p, const FILE_FULL_DIR_INFO *fi)
{
    read_ctx *c = p;
    uint32_t n = fi->FileNameLength / sizeof(WCHAR);
    const WCHAR *nm = fi->FileName;
    if (nm[0] == L'.' && (n == 1 || (n == 2 && nm[1] == L'.'))) return 1;
    if(c->linux)for(uint32_t i=0;i<n;i++)if(fpe_lx_quoted((uint16_t)nm[i])){c->err=ERROR_NOT_SUPPORTED;return 0;}
    /* For reparse points the EA size field carries the reparse tag. */
    c->err = fpe_dir_add(c->d, nm, n, (uint64_t)fi->EndOfFile.QuadPart, fi->LastWriteTime.QuadPart,
                         fi->CreationTime.QuadPart, fi->FileAttributes, fi->EaSize);
    return !c->err;
}

fpe_err fpe_dir_read_nt(const wchar_t *display, const wchar_t *nt, uint8_t *buf,
                        size_t reserve_hint, fpe_dir **out)
{
    *out = NULL;
    if(fpe_wsl_requires_native(display))return ERROR_NOT_SUPPORTED;
    int64_t t_read = fpe_prof_begin();
    fpe_dir *d = fpe_dir_new(display, reserve_hint);
    if (!d) return ERROR_NOT_ENOUGH_MEMORY;

    uint8_t *own = NULL;
    if (!buf) {
        own = VirtualAlloc(NULL, FPE_ENUM_BUF, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!own) { fpe_dir_release(d); return ERROR_NOT_ENOUGH_MEMORY; }
        buf = own;
    }

    read_ctx c = { d,0,fpe_wsl_path_kind(display) };
    fpe_err err = fpe_enum(nt, buf, FPE_ENUM_BUF, on_entry, &c);
    if (!err) err = c.err;
    if (own) VirtualFree(own, 0, MEM_RELEASE);
    if (!err) err = fpe_dir_finish(d);
    if (err) {
        fpe_dir_release(d);
        FPE_PROF_END("dir.read_failed", t_read);
        return err;
    }
    *out = d;
    FPE_PROF_END("dir.read", t_read);
    return 0;
}

FPE_API fpe_err fpe_dir_read(const wchar_t *path, fpe_dir **out)
{
    if (!out) return ERROR_INVALID_PARAMETER;
    *out = NULL;
    wchar_t *disp = NULL, *nt = NULL;
    fpe_err err = fpe_path_display(path, &disp);
    if (!err) err = fpe_path_nt(disp, &nt);
    if (!err) err = fpe_dir_load(disp, nt, out);
    fpe_free(disp);
    fpe_free(nt);
    return err;
}

fpe_err fpe_dir_load(const wchar_t *display, const wchar_t *nt, fpe_dir **out)
{
    *out = NULL;
    if (!fpe_wsl_owns(display)) return fpe_dir_read_nt(display, nt, NULL, 0, out);
    fpe_err err = fpe_wsl_list(display, 0, out);
    if (err != FPE_WSL_NOT_READY) return err;
    if(fpe_wsl_requires_native(display))return fpe_wsl_list(display,1,out);
    /* The helper is starting: \\wsl.localhost answers meanwhile. It can't
     * follow symlinked folders, so those wait for the helper. */
    err = fpe_dir_read_nt(display, nt, NULL, 0, out);
    if (err == ERROR_PATH_NOT_FOUND || err == ERROR_FILE_NOT_FOUND || err == ERROR_DIRECTORY ||
        err == ERROR_CANT_ACCESS_FILE || err == ERROR_NOT_SUPPORTED) {
        fpe_dir *d = NULL;
        fpe_err werr = fpe_wsl_list(display, 1, &d);
        if (!werr) {
            *out = d;
            err = 0;
        }
    }
    return err;
}

FPE_API fpe_dir *fpe_dir_retain(fpe_dir *d)
{
    if (d) InterlockedIncrement(&d->refs);
    return d;
}

FPE_API void fpe_dir_release(fpe_dir *d)
{
    if (!d || InterlockedDecrement(&d->refs) != 0) return;
    if (d->accounted) {
        fpe_gauge_add(&g_snapshots, -1);
        fpe_gauge_add(&g_snapshot_bytes, -d->accounted);
    }
    fpe_vbuf_free(&d->entries);
    fpe_vbuf_free(&d->names);
    fpe_vbuf_free(&d->unix);
    fpe_vbuf_free(&d->owners);
    fpe_free(d->path);
    fpe_free(d);
}

FPE_API int fpe_dir_is_linux(const fpe_dir *d) { return d && d->unix.base != NULL; }

FPE_API void fpe_unix_mode_string(uint32_t mode, wchar_t out[11])
{
    static const wchar_t types[] = L"?pc?d?b?-?l?s???";
    out[0] = types[(mode >> 12) & 15];
    for (int k = 0; k < 3; k++) {
        uint32_t bits = mode >> (6 - 3 * k);
        out[1 + 3 * k] = (bits & 4) ? L'r' : L'-';
        out[2 + 3 * k] = (bits & 2) ? L'w' : L'-';
        out[3 + 3 * k] = (bits & 1) ? L'x' : L'-';
    }
    if (mode & 04000) out[3] = (mode & 0100) ? L's' : L'S';
    if (mode & 02000) out[6] = (mode & 0010) ? L's' : L'S';
    if (mode & 01000) out[9] = (mode & 0001) ? L't' : L'T';
    out[10] = 0;
}

static const wchar_t *owner_name(const fpe_dir *d, uint32_t id, uint32_t group)
{
    const owner_rec *o = (const owner_rec *)d->owners.base;
    for (uint32_t i = 0; i < d->nowners; i++)
        if (o[i].id == id && o[i].group == group) return (const wchar_t *)d->names.base + o[i].name;
    return NULL;
}

FPE_API int fpe_dir_unix(const fpe_dir *d, const fpe_entry *e, fpe_unix *out)
{
    memset(out, 0, sizeof *out);
    if (!d || !e || !d->unix.base) return 0;
    size_t i = (size_t)(e - (const fpe_entry *)d->entries.base);
    if (e < (const fpe_entry *)d->entries.base || i >= d->unix.used / sizeof(unix_rec)) return 0;
    const unix_rec *r = (const unix_rec *)d->unix.base + i;
    out->mode = r->mode;
    out->uid = r->uid;
    out->gid = r->gid;
    out->owner = owner_name(d, r->uid, 0);
    out->group = owner_name(d, r->gid, 1);
    out->link = r->link == UINT32_MAX ? NULL : (const wchar_t *)d->names.base + r->link;
    return 1;
}

FPE_API uint32_t fpe_dir_count(const fpe_dir *d) { return d->count; }
FPE_API const fpe_entry *fpe_dir_entries(const fpe_dir *d) { return (const fpe_entry *)d->entries.base; }
FPE_API const wchar_t *fpe_dir_path(const fpe_dir *d) { return d->path; }

const wchar_t *fpe_dir_names(const fpe_dir *d) { return (const wchar_t *)d->names.base; }

FPE_API const wchar_t *fpe_dir_name(const fpe_dir *d, const fpe_entry *e)
{
    return (const wchar_t *)d->names.base + e->name;
}

FPE_API void fpe_dir_get_stats(const fpe_dir *d, fpe_dir_stats *s)
{
    s->entries = d->count;
    s->dirs = d->dirs;
    s->files = d->count - d->dirs;
    s->file_bytes = d->file_bytes;
    s->memory_bytes = d->entries.committed + d->names.committed + d->unix.committed + d->owners.committed + sizeof *d +
                      (wcslen(d->path) + 1) * sizeof(wchar_t);
    s->read_us = d->read_us;
    s->taken_at = d->taken_at;
}
