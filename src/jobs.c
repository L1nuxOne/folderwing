#include "namecodec.h"
#include "internal.h"
#include "winsecurity.h"
#include "winstreams.h"

#include <objbase.h>
#include <shellapi.h>
#include <stddef.h>
#include <limits.h>
#include <winternl.h>
#include "wslproto.h"

#define JOB_STACK (4u * 1024u * 1024u)
#define NO_BUFFERING_THRESHOLD (256ull * 1024 * 1024)
#define WIN_COPY_BATCH_FILES 32
#define WIN_COPY_BATCH_SMALL (1024u * 1024u)
#define COPY_ATTRS (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | \
                    FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED)

struct fpe_job {
    fpe_job         *next;
    uint64_t         id;
    fpe_op           op;
    fpe_conflict     policy;
    wchar_t        **src; /* display paths */
    uint32_t         nsrc;
    wchar_t         *dest; /* display path */
    wchar_t         *tree_root; /* src entries are literal relative paths */
    wchar_t        **wsl_src_endpoints;
    wchar_t         *wsl_dest_endpoint;
    wchar_t         *wsl_tree_endpoint;
    int              wsl_native;
    int              wsl_as_root;
    int              wsl_context_checked; /* tree children use their parent's guarded endpoints */
    int regular_only;volatile LONG *parent_stop;
    fpe_job_cb       cb;
    fpe_job_event_cb event_cb;
    void            *user;
    volatile LONG    cancel; /* also handed to CopyFileExW as its cancel flag */
    fpe_job_progress p;
    uint64_t         last_report;
    uint64_t         file_base;
    const wchar_t   *cur;
    uint8_t         *buf;
    struct win_dir_lock *directory_locks;
    DWORD namespace_error;
    DWORD durability_error;unsigned copy_depth;
    struct win_copy_batch *copy_batch;
    fpe_win_ancestry *ancestry; /* shared with regular-only selected children */
    int defer_final,final_report;
};

static fpe_gauge g_jobs_queued = FPE_GAUGE_INIT("job.pending");

typedef struct item_info {
    DWORD    attrs;
    DWORD    tag;
    uint64_t size;
} item_info;

/* ---- path buffer ------------------------------------------------------------- */

typedef struct pbuf {
    wchar_t *s;
    uint32_t len;
} pbuf;

#define PB_CAP (FPE_MAX_PATH + 32)
#define PB_FAIL UINT32_MAX

static int pb_init(pbuf *b) { b->len = 0; b->s = fpe_alloc(PB_CAP * sizeof(wchar_t)); if (b->s) b->s[0] = 0; return b->s != NULL; }

static int pb_set(pbuf *b, const wchar_t *s)
{
    size_t n = wcslen(s);
    if (n + 1 > PB_CAP) return 0;
    memcpy(b->s, s, (n + 1) * sizeof(wchar_t));
    b->len = (uint32_t)n;
    return 1;
}

static uint32_t pb_push(pbuf *b, const wchar_t *name, uint32_t n)
{
    uint32_t old = b->len;
    int sep = b->len && b->s[b->len - 1] != L'\\';
    if ((size_t)b->len + sep + n + 1 > FPE_MAX_PATH) return PB_FAIL;
    if (sep) b->s[b->len++] = L'\\';
    memcpy(b->s + b->len, name, n * sizeof(wchar_t));
    b->len += n;
    b->s[b->len] = 0;
    return old;
}

static void pb_pop(pbuf *b, uint32_t old)
{
    b->len = old;
    b->s[old] = 0;
}

static const wchar_t *shown(const wchar_t *nt) { return wcsncmp(nt, L"\\\\?\\", 4) == 0 ? nt + 4 : nt; }

/* ---- reporting --------------------------------------------------------------- */

static int cancelled(fpe_job *j) { return j->namespace_error || j->durability_error || j->cancel != 0 || (j->parent_stop&&*j->parent_stop); }

static void report(fpe_job *j, int force)
{
    if(j->defer_final&&j->p.state>=FPE_JOB_DONE){j->final_report=1;return;}
    uint64_t now = GetTickCount64();
    if (!force && now - j->last_report < 50) return;
    j->last_report = now;
    if (j->p.items_done > j->p.items_total) j->p.items_total = j->p.items_done;
    if (j->p.bytes_done > j->p.bytes_total) j->p.bytes_total = j->p.bytes_done;
    wchar_t *unc = NULL;
    if (j->cur && !wcsncmp(j->cur, L"\\\\?\\UNC\\", 8)) fpe_path_display(j->cur, &unc);
    j->p.current = unc ? unc : j->cur ? shown(j->cur) : NULL;
    if (j->cb) j->cb(j->user, &j->p);
    fpe_free(unc);
    j->p.current = NULL;
}

static void diagnostic(fpe_job *j, uint32_t kind, DWORD err, const wchar_t *path)
{
    if (j->event_cb) {
        wchar_t *unc = NULL;
        if (path && !wcsncmp(path, L"\\\\?\\UNC\\", 8)) fpe_path_display(path, &unc);
        fpe_job_event event = { j->id, kind, err, unc ? unc : path ? shown(path) : NULL };
        j->event_cb(j->user, &event);
        fpe_free(unc);
    }
}

static void skipped(fpe_job *j, DWORD why, const wchar_t *path)
{
    j->p.skipped++;
    diagnostic(j, FPE_JOB_SKIPPED, why, path);
}

static void fail(fpe_job *j, DWORD err, const wchar_t *path)
{
    diagnostic(j, FPE_JOB_ERROR, err, path);
    j->p.errors++;
    j->p.last_error = err;
    j->cur = path;
    report(j, 1);
}

/* ---- helpers --------------------------------------------------------------- */

static DWORD get_info(const wchar_t *nt, item_info *ii)
{
    /* Ordinary entries need one metadata query. Ask for a reparse tag only
     * when necessary; retain the conservative fallback for unknown tags. */
    WIN32_FILE_ATTRIBUTE_DATA ad;
    int have_attrs=GetFileAttributesExW(nt,GetFileExInfoStandard,&ad);
    if(have_attrs&&!(ad.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){
        ii->attrs=ad.dwFileAttributes;ii->tag=0;
        ii->size=(ii->attrs&FILE_ATTRIBUTE_DIRECTORY)?0:((uint64_t)ad.nFileSizeHigh<<32|ad.nFileSizeLow);
        return 0;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(nt, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    if (h != INVALID_HANDLE_VALUE) {
        FindClose(h);
        ii->attrs = fd.dwFileAttributes;
        ii->tag = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? fd.dwReserved0 : 0;
        ii->size = (ii->attrs & FILE_ATTRIBUTE_DIRECTORY) ? 0 : ((uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow);
        return 0;
    }
    if(!have_attrs&&!GetFileAttributesExW(nt,GetFileExInfoStandard,&ad))return GetLastError();
    ii->attrs = ad.dwFileAttributes;
    /* Unknown tag: assume a link so trees are never walked through it. */
    ii->tag = (ad.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? 0x20000000u : 0;
    ii->size = (ii->attrs & FILE_ATTRIBUTE_DIRECTORY) ? 0 : ((uint64_t)ad.nFileSizeHigh << 32 | ad.nFileSizeLow);
    return 0;
}

static int is_link_dir(DWORD attrs, DWORD tag)
{
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) &&
           FPE_IS_NAME_SURROGATE(tag);
}

/* child equals parent, or lies somewhere below it */
static int path_within(const wchar_t *child, const wchar_t *parent)
{
    uint32_t nc = (uint32_t)wcslen(child), np = (uint32_t)wcslen(parent);
    while (np > 0 && parent[np - 1] == L'\\') np--;
    if (nc < np || !fpe_equal_nocase(child, np, parent, np)) return 0;
    return nc == np || child[np] == L'\\';
}

/* Directory namespaces are pinned for the whole walk, not just checked before
 * it. Explicit root aliases resolve once; subsequent work uses the captured
 * physical path. Every canonical ancestor and visited directory is opened
 * without following its final reparse point, denying write/delete sharing. */
typedef struct win_id { DWORD volume, high, low; } win_id;
typedef struct win_dir_lock {HANDLE name,barrier;win_id id;wchar_t *path;struct win_dir_lock *next;} win_dir_lock;
typedef struct win_chain { HANDLE *items; unsigned count; win_id id; fpe_job *job; } win_chain;
static win_id win_file_id(const BY_HANDLE_FILE_INFORMATION *s)
{return (win_id){s->dwVolumeSerialNumber,s->nFileIndexHigh,s->nFileIndexLow};}
static int win_same_id(win_id a,win_id b)
{return a.volume==b.volume&&a.high==b.high&&a.low==b.low;}
static void win_directory_close(fpe_job *j,HANDLE h)
{
    win_dir_lock **at=&j->directory_locks;
    while(*at&&(*at)->name!=h)at=&(*at)->next;
    if(*at){win_dir_lock *g=*at;*at=g->next;if(g->barrier)CloseHandle(g->barrier);CloseHandle(g->name);fpe_free(g->path);fpe_free(g);}
}
static void win_chain_close(win_chain *g)
{while(g->count)win_directory_close(g->job,g->items[--g->count]);fpe_free(g->items);memset(g,0,sizeof *g);}
static DWORD win_directory_kind(HANDLE h,win_id *identity)
{
    BY_HANDLE_FILE_INFORMATION info;
    if(!GetFileInformationByHandle(h,&info))return GetLastError();
    if(!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))return ERROR_DIRECTORY;
    if(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT){
        FILE_ATTRIBUTE_TAG_INFO tag;
        if(!GetFileInformationByHandleEx(h,FileAttributeTagInfo,&tag,sizeof tag)||FPE_IS_NAME_SURROGATE(tag.ReparseTag))return ERROR_CANT_ACCESS_FILE;
    }
    if(identity)*identity=win_file_id(&info);return 0;
}
static DWORD win_sync_directory(fpe_job *j,HANDLE directory)
{
    win_id id,after;DWORD error=win_directory_kind(directory,&id);int found=0;
    if(error)goto done;
    for(win_dir_lock *g=j->directory_locks;g;g=g->next)if(win_same_id(g->id,id)){
        if(!g->barrier){error=ERROR_INVALID_HANDLE;goto done;}found=1;
    }
    if(!found){error=ERROR_INVALID_HANDLE;goto done;}
    error=fpe_win_ancestry_exchange(j->ancestry,directory,0);if(error)goto done;
    for(win_dir_lock *g=j->directory_locks;g;g=g->next)if(win_same_id(g->id,id)){CloseHandle(g->barrier);g->barrier=NULL;}
    /* Name pins keep denying rename/delete throughout. Reopen only the held
     * object, then restore every matching read barrier before any callback or
     * pathname operation. Never reopen a flush target through its pathname. */
    HANDLE write=fpe_win_reopen_directory(directory,GENERIC_WRITE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE);
    error=write==INVALID_HANDLE_VALUE?GetLastError():win_directory_kind(write,&after);
    if(!error&&!win_same_id(id,after))error=ERROR_FILE_INVALID;
    if(!error&&!FlushFileBuffers(write))error=GetLastError();
    if(write!=INVALID_HANDLE_VALUE)CloseHandle(write);
    for(win_dir_lock *g=j->directory_locks;g;g=g->next)if(win_same_id(g->id,id)){
        HANDLE barrier=fpe_win_reopen_directory(g->name,FILE_TRAVERSE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ);
        DWORD e=barrier==INVALID_HANDLE_VALUE?GetLastError():win_directory_kind(barrier,&after);
        if(barrier!=INVALID_HANDLE_VALUE){g->barrier=barrier;if(!e&&!win_same_id(id,after))e=ERROR_FILE_INVALID;}
        if(e){if(!j->namespace_error)j->namespace_error=e;if(!error)error=e;}
    }
    {DWORD e=fpe_win_ancestry_exchange(j->ancestry,directory,1);if(e){if(!j->namespace_error)j->namespace_error=e;if(!error)error=e;}}
done:
    if(error&&!j->durability_error)j->durability_error=error;return error;
}
static DWORD win_sync_pair(fpe_job *j,HANDLE first,HANDLE second)
{
    win_id a,b;DWORD error=win_directory_kind(first,&a),other=win_directory_kind(second,&b);
    if(error||other){error=error?error:other;if(!j->durability_error)j->durability_error=error;return error;}
    error=win_sync_directory(j,first);other=win_same_id(a,b)?0:win_sync_directory(j,second);return error?error:other;
}
static HANDLE win_directory(fpe_job *j,const wchar_t *path,win_id *identity)
{
    HANDLE name=CreateFileW(path,FILE_TRAVERSE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(name==INVALID_HANDLE_VALUE)return name;
    HANDLE barrier=CreateFileW(path,FILE_TRAVERSE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    DWORD error=barrier==INVALID_HANDLE_VALUE?GetLastError():0;win_id id={0};
    if(!error)error=win_directory_kind(name,&id);
    win_dir_lock *g=error?NULL:fpe_zalloc(sizeof *g);wchar_t *saved=error?NULL:fpe_wcsdup(path);if(!error&&(!g||!saved))error=ERROR_NOT_ENOUGH_MEMORY;
    if(error){if(barrier!=INVALID_HANDLE_VALUE)CloseHandle(barrier);CloseHandle(name);fpe_free(g);fpe_free(saved);SetLastError(error);return INVALID_HANDLE_VALUE;}
    *g=(win_dir_lock){name,barrier,id,saved,j->directory_locks};j->directory_locks=g;
    if(identity)*identity=id;return name;
}
static DWORD win_chain_open(fpe_job *j,const wchar_t *path,win_chain *g)
{
    memset(g,0,sizeof *g);g->job=j;size_t n=wcslen(path),root=0;
    if(n>=7&&!wcsncmp(path,L"\\\\?\\",4)&&path[5]==L':'&&path[6]==L'\\')root=7;
    else if(n>=8&&!wcsncmp(path,L"\\\\?\\UNC\\",8)){
        const wchar_t *server=wcschr(path+8,L'\\'),*share=server?wcschr(server+1,L'\\'):NULL;
        if(server&&server[1])root=share?(size_t)(share-path)+1:n;
    }else if(n>4&&!wcsncmp(path,L"\\\\?\\Volume{",11)){
        const wchar_t *end=wcschr(path+4,L'\\');if(end)root=(size_t)(end-path)+1;
    }
    if(!root)return ERROR_INVALID_NAME;
    unsigned cap=2;for(size_t i=root;i<n;i++)if(path[i]==L'\\')cap++;
    g->items=fpe_zalloc(cap*sizeof *g->items);wchar_t *p=fpe_wcsdup(path);DWORD error=0;
    if(!g->items||!p){error=ERROR_NOT_ENOUGH_MEMORY;goto end;}
    for(size_t at=root;;){
        wchar_t saved=p[at];p[at]=0;HANDLE h=win_directory(j,p,&g->id);p[at]=saved;
        if(h==INVALID_HANDLE_VALUE){error=GetLastError();break;}g->items[g->count++]=h;
        if(at==n)break;
        size_t next=at;while(next<n&&p[next]==L'\\')next++;while(next<n&&p[next]!=L'\\')next++;
        if(next==at){error=ERROR_INVALID_NAME;break;}at=next;
    }
end:
    fpe_free(p);if(error)win_chain_close(g);return error;
}
static DWORD win_resolve_directory(fpe_job *j,const wchar_t *path,win_chain *g,wchar_t **canonical)
{
    *canonical=NULL;memset(g,0,sizeof *g);
    HANDLE h=CreateFileW(path,FILE_TRAVERSE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
    if(h==INVALID_HANDLE_VALUE)return GetLastError();
    DWORD error=0;BY_HANDLE_FILE_INFORMATION info;
    if(!GetFileInformationByHandle(h,&info))error=GetLastError();
    else if(!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))error=ERROR_DIRECTORY;
    DWORD n=error?0:GetFinalPathNameByHandleW(h,NULL,0,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    DWORD final_error=!error&&!n?GetLastError():0;
    int literal=final_error==ERROR_INVALID_FUNCTION||final_error==ERROR_NOT_SUPPORTED;
    if(final_error&&!literal)error=final_error;
    wchar_t *p=error?NULL:literal?fpe_wcsdup(path):fpe_alloc(((size_t)n+1)*sizeof *p);
    if(!error&&!p)error=ERROR_NOT_ENOUGH_MEMORY;
    if(!error&&!literal){DWORD got=GetFinalPathNameByHandleW(h,p,n+1,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);if(!got||got>n)error=got?ERROR_FILENAME_EXCED_RANGE:GetLastError();}
    if(!error)error=win_chain_open(j,p,g);
    if(!error&&!win_same_id(g->id,win_file_id(&info)))error=ERROR_FILE_INVALID;
    CloseHandle(h);
    if(error){fpe_free(p);win_chain_close(g);}else *canonical=p;return error;
}
static DWORD win_source_parent(fpe_job *j,pbuf *source,win_chain *g)
{
    memset(g,0,sizeof *g);
    const wchar_t *slash=wcsrchr(source->s,L'\\');if(!slash||!slash[1])return ERROR_INVALID_NAME;
    size_t at=(size_t)(slash-source->s);wchar_t *name=fpe_wcsdup(slash+1),*canonical=NULL;
    if(!name)return ERROR_NOT_ENOUGH_MEMORY;
    size_t cut=at==6?at+1:at;wchar_t saved=source->s[cut];source->s[cut]=0;
    DWORD error=win_resolve_directory(j,source->s,g,&canonical);source->s[cut]=saved;
    if(!error&&(!pb_set(source,canonical)||pb_push(source,name,(uint32_t)wcslen(name))==PB_FAIL))error=ERROR_FILENAME_EXCED_RANGE;
    fpe_free(name);fpe_free(canonical);if(error)win_chain_close(g);return error;
}

static int same_volume(const wchar_t *a, const wchar_t *b)
{
    wchar_t va[MAX_PATH + 1], vb[MAX_PATH + 1];
    if (!GetVolumePathNameW(a, va, MAX_PATH) || !GetVolumePathNameW(b, vb, MAX_PATH)) return 0;
    return fpe_equal_nocase(va, (uint32_t)wcslen(va), vb, (uint32_t)wcslen(vb));
}

static fpe_err read_children(fpe_job *j, const wchar_t *nt, fpe_dir **out)
{
    return fpe_dir_read_nt(nt, nt, j->buf, 64 * 1024, out);
}

/* Rewrites the last component of d to "stem (N).ext" until it is free. */
static int unique_name(pbuf *d, int is_dir)
{
    uint32_t start = d->len;
    while (start > 0 && d->s[start - 1] != L'\\') start--;
    uint32_t dot = d->len;
    if (!is_dir) {
        for (uint32_t i = d->len; i-- > start + 1;) {
            if (d->s[i] == L'.') { dot = i; break; }
        }
    }
    wchar_t ext[300];
    uint32_t ext_len = d->len - dot;
    if (ext_len >= 300) { dot = d->len; ext_len = 0; }
    memcpy(ext, d->s + dot, ext_len * sizeof(wchar_t));

    for (uint32_t n = 2; n < 100000; n++) {
        wchar_t num[24];
        int nl = _snwprintf(num, 24, L" (%u)", n);
        if (nl <= 0 || (size_t)dot + (size_t)nl + ext_len + 1 > FPE_MAX_PATH) return 0;
        memcpy(d->s + dot, num, (size_t)nl * sizeof(wchar_t));
        memcpy(d->s + dot + nl, ext, ext_len * sizeof(wchar_t));
        d->len = dot + (uint32_t)nl + ext_len;
        d->s[d->len] = 0;
        if (GetFileAttributesW(d->s) == INVALID_FILE_ATTRIBUTES) {
            DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return 1;
        }
    }
    return 0;
}

enum { RESOLVE_SKIP = 0, RESOLVE_CREATE, RESOLVE_REPLACE, RESOLVE_MERGE };

static void win_batch_commit(fpe_job *j);
static int resolve_conflict(fpe_job *j, pbuf *d, int src_is_dir, fpe_conflict policy)
{
    DWORD a = GetFileAttributesW(d->s);
    if (a == INVALID_FILE_ATTRIBUTES) return RESOLVE_CREATE;
    /* Settle earlier stages before accounting a skipped/replaced target.
     * Otherwise its byte count could be lost when a later batch fence fails. */
    win_batch_commit(j);if(cancelled(j))return RESOLVE_SKIP;
    int dst_is_dir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;

    switch (policy) {
    case FPE_CONFLICT_RENAME:
        if (unique_name(d, src_is_dir)) return RESOLVE_CREATE;
        fail(j, ERROR_FILE_EXISTS, d->s);
        return RESOLVE_SKIP;
    case FPE_CONFLICT_SKIP:
        if (src_is_dir && dst_is_dir) return RESOLVE_MERGE;
        skipped(j, ERROR_FILE_EXISTS, d->s);
        return RESOLVE_SKIP;
    case FPE_CONFLICT_OVERWRITE:
        if (src_is_dir != dst_is_dir) { fail(j, ERROR_ALREADY_EXISTS, d->s); return RESOLVE_SKIP; }
        if (src_is_dir) return RESOLVE_MERGE;
        return RESOLVE_REPLACE;
    default:
        fail(j, ERROR_FILE_EXISTS, d->s);
        return RESOLVE_SKIP;
    }
}

static BOOL remove_dir(const wchar_t *nt)
{
    if (RemoveDirectoryW(nt)) return TRUE;
    DWORD err = GetLastError();
    DWORD a = GetFileAttributesW(nt);
    if (err == ERROR_ACCESS_DENIED && a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesW(nt, a & ~FILE_ATTRIBUTE_READONLY);
        if (RemoveDirectoryW(nt)) return TRUE;
        err = GetLastError();
    }
    SetLastError(err);
    return FALSE;
}

static BOOL delete_file(const wchar_t *nt)
{
    if (DeleteFileW(nt)) return TRUE;
    DWORD err = GetLastError();
    DWORD a = GetFileAttributesW(nt);
    if (err == ERROR_ACCESS_DENIED && a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesW(nt, a & ~FILE_ATTRIBUTE_READONLY);
        if (DeleteFileW(nt)) return TRUE;
        err = GetLastError();
    }
    SetLastError(err);
    return FALSE;
}

/* ---- scan -------------------------------------------------------------------- */

static void scan_tree(fpe_job *j, pbuf *s)
{
    HANDLE held=win_directory(j,s->s,NULL);if(held==INVALID_HANDLE_VALUE)return;
    fpe_dir *kids;
    if(read_children(j,s->s,&kids)){win_directory_close(j,held);return;}
    const fpe_entry *ents = fpe_dir_entries(kids);
    uint32_t n = fpe_dir_count(kids);
    for (uint32_t i = 0; i < n && !cancelled(j); i++) {
        const fpe_entry *e = ents + i;
        j->p.items_total++;
        if (!fpe_entry_is_dir(e)) { j->p.bytes_total += e->size; continue; }
        if (is_link_dir(e->attrs, e->reparse_tag)) continue;
        uint32_t old = pb_push(s, fpe_dir_name(kids, e), e->name_len);
        if (old == PB_FAIL) continue;
        scan_tree(j, s);
        pb_pop(s, old);
    }
    fpe_dir_release(kids);win_directory_close(j,held);
    report(j, 0);
}

static void scan_item(fpe_job *j, pbuf *s, const item_info *ii)
{
    j->p.items_total++;
    if (!(ii->attrs & FILE_ATTRIBUTE_DIRECTORY)) j->p.bytes_total += ii->size;
    else if (!is_link_dir(ii->attrs, ii->tag)) scan_tree(j, s);
}

/* ---- copy -------------------------------------------------------------------- */

typedef struct copy_state {
    fpe_job *job;
    const wchar_t *temp,*source_path;
    HANDLE file,source,removal,stream;
    DWORD error;
    int move,link,extra_streams;
    BY_HANDLE_FILE_INFORMATION source_info;FILE_BASIC_INFO source_basic;
} copy_state;
static DWORD win_discard(HANDLE file)
{
    FILE_BASIC_INFO basic;FILE_DISPOSITION_INFO remove={TRUE};
    if(!GetFileInformationByHandleEx(file,FileBasicInfo,&basic,sizeof basic))return GetLastError();
    if(basic.FileAttributes&FILE_ATTRIBUTE_READONLY){
        FILE_BASIC_INFO update={0};update.FileAttributes=basic.FileAttributes&~FILE_ATTRIBUTE_READONLY;
        if(!update.FileAttributes)update.FileAttributes=FILE_ATTRIBUTE_NORMAL;
        if(!SetFileInformationByHandle(file,FileBasicInfo,&update,sizeof update))return GetLastError();
    }
    return SetFileInformationByHandle(file,FileDispositionInfo,&remove,sizeof remove)?0:GetLastError();
}
/* The publication anchor and current named stream have distinct lifetimes.
 * A stream switch means the preceding stream has finished; CopyFileEx returns
 * after the final stream. Never reopen a stream by its staging pathname. */
static void copy_stream_finish(copy_state *c,int flush)
{
    if(!c->stream)return;
    DWORD error=0;
    if(flush&&!FlushFileBuffers(c->stream)){error=GetLastError();if(!c->job->durability_error)c->job->durability_error=error;}
    if(!CloseHandle(c->stream)&&!error)error=GetLastError();c->stream=NULL;
    if(!c->error)c->error=error;
}
static DWORD CALLBACK copy_progress(LARGE_INTEGER total,LARGE_INTEGER done,LARGE_INTEGER stream_size,
                                    LARGE_INTEGER stream_done,DWORD stream,DWORD reason,HANDLE hs,HANDLE hd,LPVOID data)
{
    (void)total;(void)stream_size;(void)stream_done;
    copy_state *c=data;fpe_job *j=c->job;
    if(stream>1)c->extra_streams=1;
    if(!c->file&&stream==1){
        HANDLE process=GetCurrentProcess();BY_HANDLE_FILE_INFORMATION a;
        /* Keep the actual CopyFileEx object and its exclusive sharing alive
         * through publication, including additional streams. No second name
         * lookup is needed: publication and cleanup use this held object. */
        if(!DuplicateHandle(process,hd,process,&c->file,GENERIC_WRITE|DELETE|WRITE_DAC|FILE_READ_ATTRIBUTES,FALSE,0))c->error=GetLastError();
        if(!c->error){FILE_ATTRIBUTE_TAG_INFO tag;if(!GetFileInformationByHandleEx(c->file,FileAttributeTagInfo,&tag,sizeof tag))c->error=GetLastError();else c->link=(tag.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&FPE_IS_NAME_SURROGATE(tag.ReparseTag);}
        if(!c->error&&c->move){
            /* A distinct attribute-only file object anchors identity without
             * retaining CopyFileEx's read-sharing restriction after it exits.
             * The move acquires its destructive handle after copying, before
             * publication, and compares it to this original object/version. */
            c->source=CreateFileW(c->source_path,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,NULL);
            if(c->source==INVALID_HANDLE_VALUE){c->source=NULL;c->error=GetLastError();}
            else if(!GetFileInformationByHandle(hs,&c->source_info)||!GetFileInformationByHandle(c->source,&a)||
                    !GetFileInformationByHandleEx(hs,FileBasicInfo,&c->source_basic,sizeof c->source_basic))c->error=GetLastError();
            else if(!win_same_id(win_file_id(&a),win_file_id(&c->source_info)))c->error=ERROR_FILE_INVALID;
        }
    }
    if(!c->error&&!cancelled(j)&&reason==CALLBACK_STREAM_SWITCH&&stream>1){
        if(c->stream)copy_stream_finish(c,1);
        else if(!c->file)c->error=ERROR_INVALID_DATA;
        else if(!FlushFileBuffers(c->file)){c->error=GetLastError();if(!j->durability_error)j->durability_error=c->error;}
        if(!c->error&&!cancelled(j)){
            /* Named callback handles need only their original write rights;
             * asking for attribute access can be denied on real NTFS. */
            HANDLE process=GetCurrentProcess();
            if(!DuplicateHandle(process,hd,process,&c->stream,0,FALSE,DUPLICATE_SAME_ACCESS))c->error=GetLastError();
        }
    }
    j->p.bytes_done=j->file_base+(uint64_t)done.QuadPart;report(j,0);
    if(c->error)return c->file?PROGRESS_STOP:PROGRESS_CANCEL;
    return cancelled(j)?PROGRESS_STOP:PROGRESS_CONTINUE;
}
static void copy_item(fpe_job *j,pbuf *s,pbuf *d,HANDLE source_parent,HANDLE parent,const item_info *ii,int move,fpe_conflict policy);

typedef struct job_rename_info { DWORD flags;HANDLE root;DWORD bytes;wchar_t name[1]; } job_rename_info;
typedef NTSTATUS (NTAPI *job_nt_set)(HANDLE,PIO_STATUS_BLOCK,PVOID,ULONG,FILE_INFORMATION_CLASS);
typedef ULONG (WINAPI *job_nt_error)(NTSTATUS);
DWORD fpe_win_rename_handle(HANDLE file,HANDLE parent,const wchar_t *name,int replace,int readonly)
{
    size_t bytes=wcslen(name)*sizeof(wchar_t),size=offsetof(job_rename_info,name)+bytes;
    /* A one-unit name ends before the structure's trailing padding on x64.
     * Native validation still requires the complete fixed structure. */
    if(size<sizeof(job_rename_info))size=sizeof(job_rename_info);
    job_rename_info *r=fpe_zalloc(size+sizeof(wchar_t));if(!r)return ERROR_NOT_ENOUGH_MEMORY;
    r->flags=replace?1u:0;r->root=parent;r->bytes=(DWORD)bytes;memcpy(r->name,name,bytes);
    /* The native interface accepts a captured directory handle, or a simple
     * filename to rename within the staging file's existing directory.
     * Win32 SetFileInformationByHandle rejects the non-NULL root case. */
    HMODULE ntdll=GetModuleHandleW(L"ntdll.dll");job_nt_set set=(job_nt_set)GetProcAddress(ntdll,"NtSetInformationFile");
    job_nt_error dos=(job_nt_error)GetProcAddress(ntdll,"RtlNtStatusToDosError");DWORD error=ERROR_NOT_SUPPORTED;
    if(set&&dos){
        IO_STATUS_BLOCK io={0};if(replace)r->flags|=2u;if(readonly)r->flags|=0x40u; /* POSIX replacement after acquiring target DELETE rights; ignore readonly only when requested. */
        NTSTATUS status=set(file,&io,r,(ULONG)size,(FILE_INFORMATION_CLASS)(replace?65:10)); /* FileRenameInformation[Ex]. */
        error=status>=0?0:dos(status);
    }
    fpe_free(r);return error;
}
static DWORD win_move_handle(fpe_job *j,HANDLE file,HANDLE parent,const wchar_t *name,int replace,int readonly)
{
    BY_HANDLE_FILE_INFORMATION info;if(!GetFileInformationByHandle(parent,&info))return GetLastError();win_id id=win_file_id(&info);
    DWORD exchanged=fpe_win_ancestry_exchange(j->ancestry,parent,0);if(exchanged)return exchanged;
    for(win_dir_lock *g=j->directory_locks;g;g=g->next)if(win_same_id(g->id,id)&&g->barrier){CloseHandle(g->barrier);g->barrier=NULL;}
    /* No path-based operation or user callback is allowed in this window.
     * The directory name remains pinned, all its ancestors stay guarded,
     * and rename uses the captured source object and destination handle.
     * An in-place reparse conversion fails the rename/restore, never retries
     * through the new target. Re-lock and validate before walking again. */
    DWORD error=fpe_win_rename_handle(file,parent,name,replace,readonly),guard_error=0;
    for(win_dir_lock *g=j->directory_locks;g;g=g->next)if(win_same_id(g->id,id)&&!g->barrier){
        g->barrier=fpe_win_reopen_directory(g->name,FILE_TRAVERSE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ);
        win_id restored;DWORD e=g->barrier==INVALID_HANDLE_VALUE?GetLastError():win_directory_kind(g->barrier,&restored);
        if(!e&&!win_same_id(g->id,restored))e=ERROR_FILE_INVALID;
        if(g->barrier==INVALID_HANDLE_VALUE)g->barrier=NULL;if(e&&!guard_error)guard_error=e;
    }
    {DWORD e=fpe_win_ancestry_exchange(j->ancestry,parent,1);if(e&&!guard_error)guard_error=e;}
    if(guard_error&&!j->namespace_error){j->namespace_error=guard_error;fail(j,guard_error,j->dest);}
    return error;
}
static int win_same_file(const BY_HANDLE_FILE_INFORMATION *a,const BY_HANDLE_FILE_INFORMATION *b)
{
    return win_same_id(win_file_id(a),win_file_id(b))&&a->dwFileAttributes==b->dwFileAttributes&&
        a->nFileSizeHigh==b->nFileSizeHigh&&a->nFileSizeLow==b->nFileSizeLow&&
        !CompareFileTime(&a->ftCreationTime,&b->ftCreationTime)&&!CompareFileTime(&a->ftLastWriteTime,&b->ftLastWriteTime);
}
static int win_source_stable(copy_state *c)
{
    BY_HANDLE_FILE_INFORMATION now;FILE_BASIC_INFO basic;
    return c->source&&GetFileInformationByHandle(c->source,&now)&&GetFileInformationByHandleEx(c->source,FileBasicInfo,&basic,sizeof basic)&&
        win_same_file(&c->source_info,&now)&&c->source_basic.ChangeTime.QuadPart==basic.ChangeTime.QuadPart;
}

/* NTFS group commit: first drain every held stage's data AND metadata to the
 * storage writeback cache, then fully flush one held file before ANY rename.
 * That device fence hardens all preceding writeouts on this same volume.
 * Renamed files get a second writeout/full-fence phase. Directory completion
 * still requires its original held-directory barriers. No source is removed.
 * Other filesystems, endpoints, moves, replacements and extra streams retain
 * the ordinary per-file full-flush path. No workers or volume handles needed. */
typedef NTSTATUS (NTAPI *job_nt_flush)(HANDLE,ULONG,PVOID,ULONG,PIO_STATUS_BLOCK);
typedef struct win_copy_slot {
    HANDLE file;
    wchar_t *source,*leaf;
    uint64_t size;
    int published;
} win_copy_slot;
typedef struct win_copy_batch {
    job_nt_flush writeout;
    job_nt_error dos;
    DWORD volume;
    unsigned count;
    uint64_t bytes_base;
    win_copy_slot slots[WIN_COPY_BATCH_FILES];
} win_copy_batch;
static int win_batch_supported(const wchar_t *source,const wchar_t *dest,HANDLE parent,win_copy_batch *b)
{
    /* Captured local drive paths only: no UNC/WSL/network filesystem policy. */
    if(wcsncmp(source,L"\\\\?\\",4)||wcsncmp(dest,L"\\\\?\\",4)||
       !source[4]||source[5]!=L':'||!dest[4]||dest[5]!=L':')return 0;
    wchar_t source_drive[]={source[4],L':',L'\\',0},dest_drive[]={dest[4],L':',L'\\',0};
    if(GetDriveTypeW(source_drive)!=DRIVE_FIXED||GetDriveTypeW(dest_drive)!=DRIVE_FIXED)return 0;
    wchar_t fs[32];BY_HANDLE_FILE_INFORMATION info;
    if(!GetVolumeInformationByHandleW(parent,NULL,0,NULL,NULL,NULL,fs,32)||wcscmp(fs,L"NTFS")||
       !GetFileInformationByHandle(parent,&info))return 0;
    HMODULE ntdll=GetModuleHandleW(L"ntdll.dll");
    b->writeout=(job_nt_flush)GetProcAddress(ntdll,"NtFlushBuffersFileEx");
    b->dos=(job_nt_error)GetProcAddress(ntdll,"RtlNtStatusToDosError");
    b->volume=info.dwVolumeSerialNumber;return b->writeout&&b->dos;
}
static DWORD win_batch_writeout(win_copy_batch *b,HANDLE file)
{
    IO_STATUS_BLOCK io={0};
    /* FLUSH_FLAGS_NO_SYNC (2): data + metadata, not FILE_DATA_ONLY. A checked
     * full FlushFileBuffers MUST follow before publication/completion. */
    int64_t started=fpe_prof_begin();
    NTSTATUS status=b->writeout(file,2,NULL,0,&io);
    if(status==(NTSTATUS)0x103){ /* STATUS_PENDING is not completed writeout. */
        DWORD wait=WaitForSingleObject(file,INFINITE);
        if(wait!=WAIT_OBJECT_0){DWORD error=wait==WAIT_FAILED?GetLastError():ERROR_GEN_FAILURE;FPE_PROF_END("job.batch_writeout",started);return error;}
        status=io.Status;if(status==(NTSTATUS)0x103)status=(NTSTATUS)0xc0000001;
    }
    FPE_PROF_END("job.batch_writeout",started);return status>=0?0:b->dos(status);
}
static DWORD win_batch_fence(HANDLE file)
{int64_t started=fpe_prof_begin();DWORD error=FlushFileBuffers(file)?0:GetLastError();FPE_PROF_END("job.batch_fence",started);return error;}
static void win_batch_error(fpe_job *j,DWORD error,const wchar_t *source)
{if(!j->durability_error)j->durability_error=error;fail(j,error,source);}
static void win_batch_commit(fpe_job *j)
{
    win_copy_batch *b=j->copy_batch;if(!b||!b->count)return;
    const wchar_t *current=j->cur;DWORD error=0;unsigned published=0;
    if(!cancelled(j))error=win_batch_fence(b->slots[0].file);
    if(error)win_batch_error(j,error,b->slots[0].source);
    for(unsigned i=0;i<b->count&&!cancelled(j);i++){
        win_copy_slot *s=b->slots+i;
        error=fpe_win_rename_handle(s->file,NULL,s->leaf,0,0);
        if(!error){s->published=1;published++;error=win_batch_writeout(b,s->file);}
        if(error){win_batch_error(j,error,s->source);break;}
    }
    /* A published prefix always gets its full post-publication barrier, even
     * if cancellation or a later writeout/rename failed. Never roll it back. */
    if(published){DWORD e=win_batch_fence(b->slots[0].file);if(e&&!error){error=e;win_batch_error(j,e,b->slots[0].source);}}
    j->p.bytes_done=b->bytes_base;
    for(unsigned i=0;i<b->count;i++){
        win_copy_slot *s=b->slots+i;
        if(s->published){j->p.bytes_done+=s->size;j->p.items_done++;}
        else{DWORD e=win_discard(s->file);if(e&&!error){error=e;win_batch_error(j,e,s->source);}}
        CloseHandle(s->file);fpe_free(s->source);fpe_free(s->leaf);memset(s,0,sizeof *s);
    }
    b->count=0;j->cur=current;report(j,0);
}
/* 1 transfers ownership, 0 keeps the synchronous path, -1 reports failure. */
static int win_batch_stage(fpe_job *j,HANDLE file,const wchar_t *source,const wchar_t *leaf,uint64_t size)
{
    win_copy_batch *b=j->copy_batch;BY_HANDLE_FILE_INFORMATION info;
    if(!GetFileInformationByHandle(file,&info)){j->durability_error=GetLastError();return -1;}
    if(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DEVICE))return 0;
    if(((uint64_t)info.nFileSizeHigh<<32|info.nFileSizeLow)>WIN_COPY_BATCH_SMALL)return 0;
    if(info.dwVolumeSerialNumber!=b->volume){j->durability_error=ERROR_NOT_SAME_DEVICE;return -1;}
    wchar_t *saved=fpe_wcsdup(source),*name=fpe_wcsdup(leaf);
    if(!saved||!name){fpe_free(saved);fpe_free(name);return 0;}
    DWORD error=win_batch_writeout(b,file);
    if(error){fpe_free(saved);fpe_free(name);j->durability_error=error;return -1;}
    if(!b->count)b->bytes_base=j->file_base;
    b->slots[b->count++]=(win_copy_slot){file,saved,name,size,0};return 1;
}
static void do_copy_file(fpe_job *j,pbuf *s,pbuf *d,HANDLE source_parent,HANDLE parent,uint64_t size,int replace,int move)
{
    int batch=j->copy_batch&&!move&&!replace&&!j->regular_only&&size<=WIN_COPY_BATCH_SMALL;
    if(j->copy_batch&&(!batch||j->copy_batch->count==WIN_COPY_BATCH_FILES))win_batch_commit(j);
    if(cancelled(j))return;
    DWORD error=0;HANDLE previous=INVALID_HANDLE_VALUE,target_lock=INVALID_HANDLE_VALUE;BY_HANDLE_FILE_INFORMATION old={0};FILE_BASIC_INFO old_basic={0};
    pbuf temp={0};copy_state c={.job=j,.move=move,.source_path=s->s};int published=0,copied=0,removed=0;
    j->cur=s->s;j->file_base=j->p.bytes_done;
    if(replace){
        previous=CreateFileW(d->s,READ_CONTROL|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,NULL);
        if(previous==INVALID_HANDLE_VALUE){error=GetLastError();goto end;}
        if(!GetFileInformationByHandle(previous,&old)||!GetFileInformationByHandleEx(previous,FileBasicInfo,&old_basic,sizeof old_basic)){error=GetLastError();goto end;}
        if(old.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){error=ERROR_DIRECTORY;goto end;}
    }
    const wchar_t *slash=wcsrchr(d->s,L'\\');if(!slash){error=ERROR_INVALID_NAME;goto end;}
    if(!pb_init(&temp)){error=ERROR_NOT_ENOUGH_MEMORY;goto end;}
    GUID guid;if(FAILED(CoCreateGuid(&guid))){error=ERROR_GEN_FAILURE;goto end;}
    wchar_t name[90];_snwprintf(name,90,L".openpilot-copy-%08lx%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x.tmp",
        (unsigned long)guid.Data1,guid.Data2,guid.Data3,guid.Data4[0],guid.Data4[1],guid.Data4[2],guid.Data4[3],guid.Data4[4],guid.Data4[5],guid.Data4[6],guid.Data4[7]);
    size_t n=(size_t)(slash-d->s)+1;if(n+wcslen(name)>=FPE_MAX_PATH){error=ERROR_FILENAME_EXCED_RANGE;goto end;}
    memcpy(temp.s,d->s,n*sizeof(wchar_t));wcscpy(temp.s+n,name);c.temp=temp.s;
    DWORD flags=COPY_FILE_COPY_SYMLINK|COPY_FILE_FAIL_IF_EXISTS;if(size>=NO_BUFFERING_THRESHOLD)flags|=COPY_FILE_NO_BUFFERING;
    int64_t t=fpe_prof_begin();copied=CopyFileExW(s->s,temp.s,copy_progress,&c,(LPBOOL)(LONG *)&j->cancel,flags);
    error=c.error?c.error:copied?0:GetLastError();FPE_PROF_END("job.copy_file",t);
    /* Preserve CopyFileEx's error before flush/close can replace LastError.
     * On failure/cancellation close the retained stream without committing it. */
    if(!c.error)c.error=error;
    copy_stream_finish(&c,copied&&!c.error&&!cancelled(j));error=c.error;
    if(cancelled(j))goto end;
    if(!error&&!c.file)error=ERROR_NOT_SUPPORTED; /* No unanchored publication. */
    if(!error&&move){
        c.removal=CreateFileW(s->s,DELETE|FILE_READ_ATTRIBUTES|((c.source_info.dwFileAttributes&FILE_ATTRIBUTE_READONLY)?FILE_WRITE_ATTRIBUTES:0),FILE_SHARE_READ,NULL,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,NULL);
        BY_HANDLE_FILE_INFORMATION current;
        if(c.removal==INVALID_HANDLE_VALUE){c.removal=NULL;error=GetLastError();}
        else if(!GetFileInformationByHandle(c.removal,&current))error=GetLastError();
        else if(!win_same_id(win_file_id(&current),win_file_id(&c.source_info)))error=ERROR_FILE_INVALID;
    }
    if(!error&&move&&!win_source_stable(&c))error=ERROR_FILE_INVALID;
    if(!error&&replace){
        BY_HANDLE_FILE_INFORMATION current;FILE_BASIC_INFO basic;HANDLE h=CreateFileW(d->s,DELETE|FILE_READ_ATTRIBUTES|((old.dwFileAttributes&FILE_ATTRIBUTE_READONLY)?FILE_WRITE_ATTRIBUTES:0),FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,NULL);
        if(h==INVALID_HANDLE_VALUE)error=GetLastError();
        else{if(!GetFileInformationByHandle(h,&current)||!GetFileInformationByHandleEx(h,FileBasicInfo,&basic,sizeof basic))error=GetLastError();else if(!win_same_file(&old,&current)||old_basic.ChangeTime.QuadPart!=basic.ChangeTime.QuadPart)error=ERROR_FILE_INVALID;target_lock=h;}
        if(!error)error=win_copy_dacl(previous,c.file);
    }
    if(!error&&!cancelled(j)&&batch&&!c.link&&!c.extra_streams){
        int queued=win_batch_stage(j,c.file,s->s,slash+1,size);
        if(queued==1){c.file=NULL;fpe_free(temp.s);j->p.bytes_done=j->file_base+size;return;}
        if(queued<0){error=j->durability_error;goto end;}
    }
    /* A special/unsupported leaf flushes the preceding batch before using
     * the ordinary writer. Its already-copied stage remains exclusively held. */
    if(j->copy_batch&&j->copy_batch->count){
        win_batch_commit(j);j->file_base=j->p.bytes_done;
        if(cancelled(j))goto end;
    }
    if(!error&&!cancelled(j)){
        if(c.link)error=win_sync_directory(j,parent);
        else if(!FlushFileBuffers(c.file)){error=GetLastError();j->durability_error=error;}
    }
    /* The persistence call can block. Repeat the source/version and target
     * guards afterward, before publishing the still-exclusive held object. */
    if(!error&&!cancelled(j)&&move&&!win_source_stable(&c))error=ERROR_FILE_INVALID;
    if(!error&&!cancelled(j)&&replace){
        BY_HANDLE_FILE_INFORMATION current;FILE_BASIC_INFO basic;
        HANDLE now=CreateFileW(d->s,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,NULL);
        if(now==INVALID_HANDLE_VALUE)error=GetLastError();
        else{if(!GetFileInformationByHandle(now,&current)||!GetFileInformationByHandleEx(now,FileBasicInfo,&basic,sizeof basic))error=GetLastError();else if(!win_same_file(&old,&current)||old_basic.ChangeTime.QuadPart!=basic.ChangeTime.QuadPart)error=ERROR_FILE_INVALID;CloseHandle(now);}
    }
    if(!error&&!cancelled(j)){error=fpe_win_rename_handle(c.file,NULL,slash+1,replace,replace&&(old.dwFileAttributes&FILE_ATTRIBUTE_READONLY));published=!error;}
    if(published){
        if(!c.link&&!FlushFileBuffers(c.file)){error=GetLastError();j->durability_error=error;}
        /* Regular-only children have an owning selected-tree directory fence.
         * Plain folder copies batch entries until the containing dir's final
         * fence; moves must persist each destination before source removal. */
        if(!error&&!j->regular_only&&(move||!j->copy_depth||c.link))error=win_sync_directory(j,parent);
    }
    if(published&&!error&&move&&!cancelled(j)){if(!win_source_stable(&c))error=ERROR_FILE_INVALID;else{error=win_discard(c.removal);removed=!error;}}
end:
    copy_stream_finish(&c,0);if(!error)error=c.error;
    if(c.file){if(!published){DWORD cleanup=win_discard(c.file);if(!error)error=cleanup;}CloseHandle(c.file);}
    if(c.removal&&!CloseHandle(c.removal)&&!error)error=GetLastError();if(c.source&&!CloseHandle(c.source)&&!error)error=GetLastError();
    if(removed){DWORD e=win_sync_directory(j,source_parent);if(!error)error=e;}
    if(target_lock!=INVALID_HANDLE_VALUE)CloseHandle(target_lock);if(previous!=INVALID_HANDLE_VALUE)CloseHandle(previous);fpe_free(temp.s);
    if(!published)j->p.bytes_done=j->file_base;
    if(cancelled(j)&&!error)return;
    if(cancelled(j)){if(error&&error!=ERROR_REQUEST_ABORTED)fail(j,error,s->s);return;}
    if(error)fail(j,error,s->s);
    if(published)j->p.bytes_done=j->file_base+size;
    j->p.items_done++;report(j,0);
}

static DWORD win_remove_copied_directory(fpe_job *j,pbuf *s,HANDLE held,HANDLE parent)
{
    BY_HANDLE_FILE_INFORMATION before,now;DWORD error=win_require_unnamed_streams(held);
    if(!error&&!GetFileInformationByHandle(held,&before))error=GetLastError();
    win_directory_close(j,held);if(error)return error;
    /* Ancestors remain pinned. Open the final leaf without following reparses,
     * verify the captured directory identity, then remove this exact object. */
    HANDLE remove=CreateFileW(s->s,DELETE|FILE_READ_ATTRIBUTES|((before.dwFileAttributes&FILE_ATTRIBUTE_READONLY)?FILE_WRITE_ATTRIBUTES:0),FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,NULL);
    if(remove==INVALID_HANDLE_VALUE)return GetLastError();
    error=GetFileInformationByHandle(remove,&now)?0:GetLastError();
    if(!error&&(!win_same_id(win_file_id(&before),win_file_id(&now))||!(now.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(now.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)))error=ERROR_FILE_INVALID;
    if(!error)error=win_require_unnamed_streams(remove);
    int removed=0;
    if(!error){error=win_discard(remove);removed=!error;if(error==ERROR_DIR_NOT_EMPTY)error=0;}
    if(!CloseHandle(remove)&&!error)error=GetLastError();
    if(removed){DWORD sync=win_sync_directory(j,parent);if(!error)error=sync;}return error;
}
static void do_copy_dir(fpe_job *j,pbuf *s,pbuf *d,HANDLE source_parent,HANDLE parent,DWORD attrs,int merge,int move)
{
    j->cur = s->s;j->p.items_done++;
    HANDLE sh=win_directory(j,s->s,NULL),dh=INVALID_HANDLE_VALUE;
    if(sh==INVALID_HANDLE_VALUE){fail(j,GetLastError(),s->s);return;}
    /* Directory streams have no lossless representation in this writer.
     * Refuse before creating the target or moving any children. */
    DWORD streams=win_require_unnamed_streams(sh);
    if(streams){win_directory_close(j,sh);fail(j,streams,s->s);return;}
    if(!merge&&!CreateDirectoryW(d->s,NULL)){
        DWORD error=GetLastError();win_directory_close(j,sh);fail(j,error,d->s);return;
    }
    dh=win_directory(j,d->s,NULL);
    if(dh==INVALID_HANDLE_VALUE){DWORD error=GetLastError();win_directory_close(j,sh);fail(j,error,d->s);return;}
    DWORD barrier=win_sync_pair(j,dh,parent);
    if(barrier||cancelled(j)){win_directory_close(j,dh);win_directory_close(j,sh);if(barrier)fail(j,barrier,d->s);return;}
    fpe_dir *kids;
    fpe_err err=read_children(j,s->s,&kids);
    if(err){win_directory_close(j,dh);win_directory_close(j,sh);fail(j,err,s->s);return;}
    const fpe_entry *ents = fpe_dir_entries(kids);
    uint32_t n = fpe_dir_count(kids);
    win_copy_batch batch={0},*outer=j->copy_batch;
    j->copy_batch=!move&&n>=WIN_COPY_BATCH_FILES&&win_batch_supported(s->s,d->s,dh,&batch)?&batch:NULL;
    j->copy_depth++;
    for (uint32_t i = 0; i < n && !cancelled(j); i++) {
        const fpe_entry *e = ents + i;
        const wchar_t *nm = fpe_dir_name(kids, e);
        uint32_t os = pb_push(s, nm, e->name_len);
        uint32_t od = pb_push(d, nm, e->name_len);
        if (os == PB_FAIL || od == PB_FAIL) {
            if (os != PB_FAIL) pb_pop(s, os);
            if (od != PB_FAIL) pb_pop(d, od);
            fail(j, ERROR_FILENAME_EXCED_RANGE, s->s);
            continue;
        }
        item_info ci = { e->attrs, e->reparse_tag, e->size };
        copy_item(j,s,d,sh,dh,&ci,move,j->policy);
        pb_pop(s, os);
        pb_pop(d, od);
    }
    fpe_dir_release(kids);
    win_batch_commit(j);j->copy_batch=outer;
    j->copy_depth--;

    if(!merge&&!cancelled(j)&&!SetFileAttributesW(d->s,(attrs&COPY_ATTRS)|FILE_ATTRIBUTE_DIRECTORY)){fail(j,GetLastError(),d->s);win_directory_close(j,dh);win_directory_close(j,sh);return;}
    if(!cancelled(j)){barrier=win_sync_pair(j,dh,parent);if(barrier)fail(j,barrier,d->s);}
    win_directory_close(j,dh);
    streams=cancelled(j)?0:win_require_unnamed_streams(sh);
    if(streams||barrier){win_directory_close(j,sh);if(streams)fail(j,streams,s->s);return;}
    /* Not empty means something inside was skipped or failed: keep it. */
    if(move&&!cancelled(j)){DWORD error=win_remove_copied_directory(j,s,sh,source_parent);if(error)fail(j,error,s->s);}
    else win_directory_close(j,sh);
}

/* Windows source names are literal UTF-16. A legacy WSL destination would
 * reinterpret its PUA aliases. Only the native transport can copy those names. */
static int win_cross_name_unsupported(const pbuf *s,const pbuf *d)
{
    const wchar_t *name=wcsrchr(s->s,L'\\');name=name?name+1:s->s;
    for(;*name;name++)if(fpe_lx_quoted((uint16_t)*name))
        return !fpe_wsl_path_kind(s->s)&&fpe_wsl_path_kind(d->s);
    return 0;
}
static void copy_item(fpe_job *j,pbuf *s,pbuf *d,HANDLE source_parent,HANDLE parent,const item_info *ii,int move,fpe_conflict policy)
{
    if(win_cross_name_unsupported(s,d)){fail(j,ERROR_NOT_SUPPORTED,s->s);j->p.items_done++;return;}
    item_info current;DWORD fresh=get_info(s->s,&current);if(fresh){fail(j,fresh,s->s);j->p.items_done++;return;}ii=&current;
    int is_dir = (ii->attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    j->cur = s->s;
    if (is_dir && is_link_dir(ii->attrs, ii->tag)) {
        skipped(j, ERROR_NOT_SUPPORTED, s->s);
        j->p.items_done++;
        return;
    }
    int r = resolve_conflict(j, d, is_dir, policy);
    if(cancelled(j))return;
    if (r == RESOLVE_SKIP) {
        j->p.items_done++;
        j->p.bytes_done += ii->size;
        return;
    }
    if(is_dir){win_batch_commit(j);if(cancelled(j))return;}
    if (is_dir) do_copy_dir(j,s,d,source_parent,parent,ii->attrs,r==RESOLVE_MERGE,move);
    else do_copy_file(j,s,d,source_parent,parent,ii->size,r==RESOLVE_REPLACE,move);
}

/* ---- move -------------------------------------------------------------------- */

static void move_item(fpe_job *j,pbuf *s,pbuf *d,HANDLE source_parent,HANDLE parent,const item_info *ii)
{
    if(win_cross_name_unsupported(s,d)){fail(j,ERROR_NOT_SUPPORTED,s->s);j->p.items_done++;return;}
    item_info current;DWORD fresh=get_info(s->s,&current);if(fresh){fail(j,fresh,s->s);j->p.items_done++;return;}ii=&current;
    int is_dir = (ii->attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    j->cur = s->s;

    if (fpe_equal_nocase(s->s, s->len, d->s, d->len)) {
        skipped(j, ERROR_ALREADY_EXISTS, s->s);
        j->p.items_done++;
        return;
    }

    int r = resolve_conflict(j, d, is_dir, j->policy);
    if (r == RESOLVE_SKIP) {
        j->p.items_done++;
        j->p.bytes_done += ii->size;
        return;
    }

    if(r==RESOLVE_MERGE&&is_link_dir(ii->attrs,ii->tag)){
        skipped(j,ERROR_NOT_SUPPORTED,s->s);j->p.items_done++;return;
    }
    if (r == RESOLVE_MERGE) {
        HANDLE sh=win_directory(j,s->s,NULL),dh=INVALID_HANDLE_VALUE;
        if(sh==INVALID_HANDLE_VALUE){fail(j,GetLastError(),s->s);return;}
        DWORD streams=win_require_unnamed_streams(sh);
        if(streams){win_directory_close(j,sh);fail(j,streams,s->s);return;}
        dh=win_directory(j,d->s,NULL);
        if(dh==INVALID_HANDLE_VALUE){DWORD error=GetLastError();win_directory_close(j,sh);fail(j,error,d->s);return;}
        DWORD barrier=win_sync_pair(j,dh,parent);
        if(barrier||cancelled(j)){win_directory_close(j,dh);win_directory_close(j,sh);if(barrier)fail(j,barrier,d->s);return;}
        fpe_dir *kids;
        fpe_err err = read_children(j, s->s, &kids);
        if(err){win_directory_close(j,dh);win_directory_close(j,sh);fail(j,err,s->s);return;}
        const fpe_entry *ents = fpe_dir_entries(kids);
        uint32_t n = fpe_dir_count(kids);
        for (uint32_t i = 0; i < n && !cancelled(j); i++) {
            const fpe_entry *e = ents + i;
            const wchar_t *nm = fpe_dir_name(kids, e);
            uint32_t os = pb_push(s, nm, e->name_len);
            uint32_t od = pb_push(d, nm, e->name_len);
            if (os == PB_FAIL || od == PB_FAIL) {
                if (os != PB_FAIL) pb_pop(s, os);
                if (od != PB_FAIL) pb_pop(d, od);
                fail(j, ERROR_FILENAME_EXCED_RANGE, s->s);
                continue;
            }
            item_info ci = { e->attrs, e->reparse_tag, e->size };
            move_item(j,s,d,sh,dh,&ci);
            pb_pop(s, os);
            pb_pop(d, od);
        }
        fpe_dir_release(kids);
        if(!cancelled(j)){barrier=win_sync_pair(j,dh,parent);if(barrier)fail(j,barrier,d->s);}
        win_directory_close(j,dh);
        streams=cancelled(j)?0:win_require_unnamed_streams(sh);
        if(streams||barrier){win_directory_close(j,sh);if(streams)fail(j,streams,s->s);return;}
        if(!cancelled(j)){DWORD error=win_remove_copied_directory(j,s,sh,source_parent);if(error)fail(j,error,s->s);}
        else win_directory_close(j,sh);
        j->p.items_done++;
        return;
    }

    HANDLE held=CreateFileW(s->s,DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,NULL);
    DWORD err=held==INVALID_HANDLE_VALUE?GetLastError():0;int renamed=0;BY_HANDLE_FILE_INFORMATION before,after;FILE_BASIC_INFO original,basic;
    if(!err&&(!GetFileInformationByHandle(held,&before)||!GetFileInformationByHandleEx(held,FileBasicInfo,&original,sizeof original)))err=GetLastError();
    if(!err)err=win_sync_pair(j,parent,source_parent);
    if(!err&&!cancelled(j)){
        if(!GetFileInformationByHandle(held,&after)||!GetFileInformationByHandleEx(held,FileBasicInfo,&basic,sizeof basic))err=GetLastError();
        else if(!win_same_file(&before,&after)||original.ChangeTime.QuadPart!=basic.ChangeTime.QuadPart)err=ERROR_FILE_INVALID;
    }
    if(!err){
        DWORD target=GetFileAttributesW(d->s);const wchar_t *name=wcsrchr(d->s,L'\\');
        int readonly=r==RESOLVE_REPLACE&&target!=INVALID_FILE_ATTRIBUTES&&(target&FILE_ATTRIBUTE_READONLY);
        HANDLE locked=INVALID_HANDLE_VALUE;
        if(r==RESOLVE_REPLACE){
            locked=CreateFileW(d->s,DELETE|(readonly?FILE_WRITE_ATTRIBUTES:0),FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,NULL);
            if(locked==INVALID_HANDLE_VALUE)err=GetLastError();
        }
        if(!err&&!cancelled(j)){err=win_move_handle(j,held,parent,name?name+1:d->s,r==RESOLVE_REPLACE,readonly);renamed=!err;}
        if(renamed&&!j->namespace_error)err=win_sync_pair(j,parent,source_parent);
        if(locked!=INVALID_HANDLE_VALUE)CloseHandle(locked);
    }
    if(held!=INVALID_HANDLE_VALUE)CloseHandle(held);
    if(renamed){if(err)fail(j,err,d->s);j->p.items_done++;j->p.bytes_done+=ii->size;report(j,0);return;}
    if(!err&&cancelled(j))return;
    if(j->durability_error){if(err)fail(j,err,s->s);return;}
    if(j->namespace_error)return;
    if (err != ERROR_NOT_SAME_DEVICE) {
        fail(j, err, s->s);
        j->p.items_done++;
        return;
    }
    if (is_dir && is_link_dir(ii->attrs, ii->tag)) {
        skipped(j, ERROR_NOT_SUPPORTED, s->s);
        j->p.items_done++;
        return;
    }
    if (is_dir) do_copy_dir(j,s,d,source_parent,parent,ii->attrs,0,1);
    else do_copy_file(j,s,d,source_parent,parent,ii->size,r==RESOLVE_REPLACE,1);
}

/* ---- delete ------------------------------------------------------------------ */

/* A delete plan records identities, not authority to follow names later. Each
 * descent opens one literal component relative to a held parent, without
 * following reparse points. The handle denies write/delete sharing through
 * enumeration and disposition. New children are never added to an old plan. */
typedef struct win_delete_node {
    struct win_delete_node *next,*children;
    wchar_t *name;
    item_info info;
    FILE_ID_INFO identity;
    FILETIME created;
    LARGE_INTEGER enumerated_id;
    int enumerated;
} win_delete_node;
static void win_delete_free(win_delete_node *n)
{
    while(n){win_delete_node *next=n->next;win_delete_free(n->children);fpe_free(n->name);fpe_free(n);n=next;}
}
static HANDLE win_delete_open(HANDLE parent,const wchar_t *name,DWORD access)
{
    typedef NTSTATUS (NTAPI *create_fn)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,PLARGE_INTEGER,ULONG,ULONG,ULONG,ULONG,PVOID,ULONG);
    typedef ULONG (WINAPI *error_fn)(NTSTATUS);
    HMODULE dll=GetModuleHandleW(L"ntdll.dll");create_fn open=(create_fn)GetProcAddress(dll,"NtCreateFile");error_fn error=(error_fn)GetProcAddress(dll,"RtlNtStatusToDosError");
    if(!open||!error){SetLastError(ERROR_NOT_SUPPORTED);return INVALID_HANDLE_VALUE;}
    size_t chars=wcslen(name);
    if(!chars||chars>USHRT_MAX/sizeof(wchar_t)||wcschr(name,L'\\')||wcschr(name,L'/')||!wcscmp(name,L".")||!wcscmp(name,L"..")){
        SetLastError(ERROR_INVALID_NAME);return INVALID_HANDLE_VALUE;
    }
    UNICODE_STRING leaf={(USHORT)(chars*sizeof(wchar_t)),(USHORT)(chars*sizeof(wchar_t)),(wchar_t *)name};
    /* Exact enumerated spelling also respects case-sensitive NTFS directories. */
    OBJECT_ATTRIBUTES object={.Length=sizeof object,.RootDirectory=parent,.ObjectName=&leaf};IO_STATUS_BLOCK io;HANDLE h=NULL;
    /* FILE_OPEN_REPARSE_POINT | FILE_OPEN_FOR_BACKUP_INTENT |
     * FILE_SYNCHRONOUS_IO_NONALERT. No pathname fallback on unsupported FS. */
    NTSTATUS status=open(&h,access|SYNCHRONIZE,&object,&io,NULL,0,FILE_SHARE_READ,1,0x204020,NULL,0);
    if(status<0){SetLastError(error(status));return INVALID_HANDLE_VALUE;}return h;
}
static HANDLE win_delete_handle(HANDLE parent,const win_delete_node *n,const wchar_t *path,DWORD access)
{
    /* Explicit operands retain Win32 case lookup. The canonical ancestor chain
     * is held by the caller. Enumerated children use exact relative names. */
    return n->enumerated?win_delete_open(parent,n->name,access):
        CreateFileW(path,access,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
}
static DWORD win_delete_identity(HANDLE h,win_delete_node *n,int scan)
{
    BY_HANDLE_FILE_INFORMATION basic;FILE_ATTRIBUTE_TAG_INFO tag;FILE_ID_INFO id;
    if(!GetFileInformationByHandle(h,&basic)||!GetFileInformationByHandleEx(h,FileAttributeTagInfo,&tag,sizeof tag)||
       !GetFileInformationByHandleEx(h,FileIdInfo,&id,sizeof id))return GetLastError();
    DWORD kind=FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT;
    if((basic.dwFileAttributes&kind)!=(n->info.attrs&kind))return ERROR_FILE_INVALID;
    if(scan){
        if(n->enumerated&&(((uint64_t)basic.nFileIndexHigh<<32|basic.nFileIndexLow)!=(uint64_t)n->enumerated_id.QuadPart||
            CompareFileTime(&n->created,&basic.ftCreationTime)))return ERROR_FILE_INVALID;
        n->identity=id;n->created=basic.ftCreationTime;
        n->info=(item_info){basic.dwFileAttributes,(basic.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)?tag.ReparseTag:0,(uint64_t)basic.nFileSizeHigh<<32|basic.nFileSizeLow};
    }else if(id.VolumeSerialNumber!=n->identity.VolumeSerialNumber||memcmp(&id.FileId,&n->identity.FileId,sizeof id.FileId)||
             CompareFileTime(&n->created,&basic.ftCreationTime)||((basic.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)?tag.ReparseTag:0)!=n->info.tag)return ERROR_FILE_INVALID;
    return 0;
}
static DWORD win_delete_children(fpe_job *j,HANDLE directory,win_delete_node **out)
{
    FILE_INFO_BY_HANDLE_CLASS cls=FileIdBothDirectoryRestartInfo;win_delete_node **tail=out;
    for(;;){
        if(cancelled(j))return ERROR_CANCELLED;
        if(!GetFileInformationByHandleEx(directory,cls,j->buf,64*1024)){
            DWORD err=GetLastError();return err==ERROR_NO_MORE_FILES?0:err;
        }
        cls=FileIdBothDirectoryInfo;
        for(uint8_t *at=j->buf;;){
            FILE_ID_BOTH_DIR_INFO *e=(FILE_ID_BOTH_DIR_INFO *)at;
            unsigned chars=e->FileNameLength/sizeof(wchar_t);
            if(chars&&!(chars==1&&e->FileName[0]==L'.')&&!(chars==2&&e->FileName[0]==L'.'&&e->FileName[1]==L'.')){
                win_delete_node *n=fpe_zalloc(sizeof *n);if(!n)return ERROR_NOT_ENOUGH_MEMORY;
                *tail=n;tail=&n->next;n->name=fpe_alloc(((size_t)chars+1)*sizeof(wchar_t));if(!n->name)return ERROR_NOT_ENOUGH_MEMORY;
                memcpy(n->name,e->FileName,e->FileNameLength);n->name[chars]=0;
                n->info.attrs=e->FileAttributes;n->created=(FILETIME){e->CreationTime.LowPart,(DWORD)e->CreationTime.HighPart};
                n->enumerated_id=e->FileId;n->enumerated=1;
            }
            if(!e->NextEntryOffset)break;at+=e->NextEntryOffset;
        }
    }
}
static DWORD win_delete_scan(fpe_job *j,pbuf *s,HANDLE parent,win_delete_node *n)
{
    if(cancelled(j))return ERROR_CANCELLED;
    DWORD access=FILE_READ_ATTRIBUTES|((n->info.attrs&FILE_ATTRIBUTE_DIRECTORY)?FILE_LIST_DIRECTORY:0);
    HANDLE h=win_delete_handle(parent,n,s->s,access);if(h==INVALID_HANDLE_VALUE)return GetLastError();
    DWORD err=win_delete_identity(h,n,1);
    if(!err){
        j->p.items_total++;
        if(!(n->info.attrs&FILE_ATTRIBUTE_DIRECTORY))j->p.bytes_total+=n->info.size;
        /* Every reparse point is a leaf; unknown tags never grant descent. */
        if((n->info.attrs&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))==FILE_ATTRIBUTE_DIRECTORY){
            err=win_delete_children(j,h,&n->children);
            for(win_delete_node *c=n->children;!err&&c;c=c->next){
                uint32_t old=pb_push(s,c->name,(uint32_t)wcslen(c->name));
                if(old==PB_FAIL){err=ERROR_FILENAME_EXCED_RANGE;break;}
                err=win_delete_scan(j,s,h,c);pb_pop(s,old);
            }
        }
    }
    CloseHandle(h);j->cur=s->s;report(j,0);return err;
}
static void delete_item(fpe_job *j,pbuf *s,HANDLE parent,win_delete_node *n)
{
    if(cancelled(j))return;
    DWORD access=DELETE|FILE_READ_ATTRIBUTES|((n->info.attrs&FILE_ATTRIBUTE_DIRECTORY)?FILE_LIST_DIRECTORY:0)|
        ((n->info.attrs&FILE_ATTRIBUTE_READONLY)?FILE_WRITE_ATTRIBUTES:0);
    HANDLE h=win_delete_handle(parent,n,s->s,access);
    DWORD err=h==INVALID_HANDLE_VALUE?GetLastError():win_delete_identity(h,n,0);
    if(!err){
        for(win_delete_node *c=n->children;c&&!cancelled(j);c=c->next){
            uint32_t old=pb_push(s,c->name,(uint32_t)wcslen(c->name));
            if(old==PB_FAIL){fail(j,ERROR_FILENAME_EXCED_RANGE,s->s);continue;}
            delete_item(j,s,h,c);pb_pop(s,old);
        }
        if(!cancelled(j)){
            FILE_BASIC_INFO before;
            if(!GetFileInformationByHandleEx(h,FileBasicInfo,&before,sizeof before))err=GetLastError();
            else{
                err=win_discard(h);
                /* If an empty-directory check or disposition fails, restore
                 * read-only state on this same held object. */
                if(err&&(before.FileAttributes&FILE_ATTRIBUTE_READONLY)){
                    FILE_BASIC_INFO restore={0};restore.FileAttributes=before.FileAttributes;
                    if(!SetFileInformationByHandle(h,FileBasicInfo,&restore,sizeof restore))diagnostic(j,FPE_JOB_ERROR,GetLastError(),s->s);
                }
            }
            if(!err&&!(n->info.attrs&FILE_ATTRIBUTE_DIRECTORY))j->p.bytes_done+=n->info.size;
        }
    }
    if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);
    j->cur=s->s;if(err)fail(j,err,s->s);
    if(!cancelled(j)){j->p.items_done++;report(j,0);}
}

/* ---- recycle ----------------------------------------------------------------- */

static void do_recycle(fpe_job *j)
{
    j->p.items_total = j->nsrc;
    j->p.state = FPE_JOB_RUNNING;
    report(j, 1);
    /* One source at a time identifies failures and permits cancellation
     * between shell calls. An in-flight shell call cannot be interrupted. */
    for (uint32_t i = 0; i < j->nsrc && !cancelled(j); i++) {
        size_t chars = wcslen(j->src[i]) + 2;
        wchar_t *list = fpe_zalloc(chars * sizeof(wchar_t));
        if (!list) { fail(j, ERROR_NOT_ENOUGH_MEMORY, j->src[i]); break; }
        wcscpy(list, j->src[i]);
        j->cur = j->src[i];
        report(j, 1);
        SHFILEOPSTRUCTW op = { 0 };
        op.wFunc = FO_DELETE;
        op.pFrom = list;
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOF_WANTNUKEWARNING;
        int64_t t = fpe_prof_begin();
        int rc = SHFileOperationW(&op);
        FPE_PROF_END("job.recycle", t);
        if (op.fAnyOperationsAborted) InterlockedExchange(&j->cancel, 1);
        else if (rc) fail(j, (DWORD)rc, j->src[i]);
        if (!op.fAnyOperationsAborted) j->p.items_done++;
        fpe_free(list);
        report(j, 1);
    }
}

/* ---- job runner -------------------------------------------------------------- */

static void native_progress(void *user,const fpe_job_progress *p)
{
    fpe_job *j=user;j->p=*p;j->p.current=NULL;
    if(j->defer_final&&p->state>=FPE_JOB_DONE){j->final_report=1;return;}
    if(j->cb)j->cb(j->user,p);
}
static void native_event(void *user,const fpe_job_event *event)
{
    fpe_job *j=user;if(j->event_cb)j->event_cb(j->user,event);
}

static void run_tree(fpe_job *j);
static fpe_err check_job_context(fpe_job *j)
{
    if (j->wsl_context_checked) return 0;
    int enabled,root; fpe_wsl_job_policy(&enabled,&root);
    if (j->wsl_native && !enabled) return ERROR_NOT_SUPPORTED;
    /* The Windows/9P writer cannot provide Linux directory durability.
     * Reject before publication or source unlink, including selected copies.
     * Read-only Linux sources may still be copied to Windows via this route. */
    if (!j->wsl_native) {
        if (j->wsl_dest_endpoint) return ERROR_NOT_SUPPORTED;
        if (j->op == FPE_OP_MOVE)
            for (uint32_t i=0;i<j->nsrc;i++)
                if (j->wsl_src_endpoints[i]) return ERROR_NOT_SUPPORTED;
    }
    wchar_t *now=NULL;fpe_err err=0;
    for(uint32_t i=0;i<j->nsrc&&!j->tree_root&&!err;i++){
        err=fpe_wsl_endpoint(j->src[i],&now);
        if(!err&&((now==NULL)!=(j->wsl_src_endpoints[i]==NULL) ||
                 (now&& !fpe_path_equal(now,j->wsl_src_endpoints[i]))))err=ERROR_NOT_SUPPORTED;
        fpe_free(now);now=NULL;
    }
    if(!err&&j->dest){
        err=fpe_wsl_endpoint(j->dest,&now);
        if(!err&&((now==NULL)!=(j->wsl_dest_endpoint==NULL) ||
                 (now&& !fpe_path_equal(now,j->wsl_dest_endpoint))))err=ERROR_NOT_SUPPORTED;
        fpe_free(now);now=NULL;
    }
    if(!err&&j->tree_root){
        err=fpe_wsl_endpoint(j->tree_root,&now);
        if(!err&&((now==NULL)!=(j->wsl_tree_endpoint==NULL) ||
                 (now&& !fpe_path_equal(now,j->wsl_tree_endpoint))))err=ERROR_NOT_SUPPORTED;
        fpe_free(now);
    }
    return err;
}
static void run_job_inner(fpe_job *j)
{
    fpe_err context_error=check_job_context(j);
    if(context_error){
        j->p=(fpe_job_progress){.job=j->id,.op=j->op,.state=FPE_JOB_FAILED};
        fail(j,context_error,j->dest?j->dest:(j->tree_root?j->tree_root:j->src[0]));
        return;
    }
    if(j->tree_root){run_tree(j);return;}
    pbuf s = { 0 }, d = { 0 };
    wchar_t *dest_nt = NULL;
    item_info *info = NULL;win_delete_node **deletions=NULL;wchar_t **resolved_sources=NULL;win_id *source_parents=NULL;win_chain dest_guard={0};
    uint8_t *flags = NULL; /* per source: 1 = valid, 2 = needs scan */

    j->p.job = j->id;
    j->p.op = j->op;
    j->p.state = FPE_JOB_SCANNING;
    report(j, 1);
    if (cancelled(j)) goto finish;

    int native = j->wsl_native;
    if(native) {
        int64_t t=fpe_prof_begin();
        fpe_err err=fpe_wsl_job_guarded(j->regular_only?WSLP_OP_COPY_FILE:(uint32_t)j->op,j->policy,(const wchar_t *const *)j->src,j->nsrc,j->dest,
                                j->id,native_progress,native_event,j,j->parent_stop?j->parent_stop:&j->cancel,j->wsl_as_root,j->ancestry);
        FPE_PROF_END("job.wsl",t);
        if(err) {
            if(err==ERROR_CANCELLED || cancelled(j))j->p.state=FPE_JOB_CANCELLED;
            else {j->p.state=FPE_JOB_FAILED;fail(j,err,j->dest?j->dest:j->src[0]);}
            if(j->p.state==FPE_JOB_CANCELLED)report(j,1);
        }
        return;
    }

    if (j->op == FPE_OP_RECYCLE) { do_recycle(j); goto finish; }

    info = fpe_zalloc(j->nsrc * sizeof *info);
    flags = fpe_zalloc(j->nsrc);resolved_sources=fpe_zalloc(j->nsrc*sizeof *resolved_sources);source_parents=fpe_zalloc(j->nsrc*sizeof *source_parents);
    if(j->op==FPE_OP_DELETE)deletions=fpe_zalloc(j->nsrc*sizeof *deletions);
    if (!pb_init(&s) || !pb_init(&d) || !info || !flags || !source_parents || !resolved_sources || (j->op==FPE_OP_DELETE&&!deletions)) {
        j->p.state = FPE_JOB_FAILED;
        fail(j, ERROR_NOT_ENOUGH_MEMORY, NULL);
        goto cleanup;
    }

    if(j->dest){
        wchar_t *path=NULL;fpe_err err=fpe_path_nt(j->dest,&path);
        if(!err)err=win_resolve_directory(j,path,&dest_guard,&dest_nt);fpe_free(path);
        if(!err&&j->ancestry&&!j->regular_only)err=fpe_win_ancestry_validate(j->ancestry,dest_guard.items[dest_guard.count-1]);
        if(err){j->p.state=FPE_JOB_FAILED;fail(j,err,j->dest);goto cleanup;}
    }

    /* Validate sources and work out totals. */
    int64_t t_scan = fpe_prof_begin();
    for (uint32_t i = 0; i < j->nsrc && !cancelled(j); i++) {
        wchar_t *nt = NULL;
        fpe_err err = fpe_path_nt(j->src[i], &nt);
        if (!err && !pb_set(&s, nt)) err = ERROR_FILENAME_EXCED_RANGE;
        fpe_free(nt);
        win_chain captured={0};
        if(!err&&(j->dest||j->op==FPE_OP_DELETE)){
            err=win_source_parent(j,&s,&captured);
            if(!err){source_parents[i]=captured.id;resolved_sources[i]=fpe_wcsdup(s.s);if(!resolved_sources[i])err=ERROR_NOT_ENOUGH_MEMORY;}
        }
        if (!err) err = get_info(s.s, &info[i]);
        if(!err&&j->regular_only&&(info[i].attrs&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DEVICE)))err=ERROR_NOT_SUPPORTED;
        if (!err && j->dest && (info[i].attrs & FILE_ATTRIBUTE_DIRECTORY) && path_within(dest_nt,s.s))
            err = ERROR_INVALID_PARAMETER; /* would copy a folder into itself */
        if (err) {
            j->p.items_total++;
            j->p.items_done++;
            fail(j, err, j->src[i]);win_chain_close(&captured);
            continue;
        }
        if(j->op==FPE_OP_DELETE){
            win_delete_node *n=fpe_zalloc(sizeof *n);deletions[i]=n;
            if(!n)err=ERROR_NOT_ENOUGH_MEMORY;
            else{
                n->name=fpe_wcsdup(wcsrchr(s.s,L'\\')+1);n->info=info[i];
                err=n->name?win_delete_scan(j,&s,captured.items[captured.count-1],n):ERROR_NOT_ENOUGH_MEMORY;
            }
            if(err){if(err!=ERROR_CANCELLED)fail(j,err,j->src[i]);win_chain_close(&captured);continue;}
            flags[i]=1;win_chain_close(&captured);continue;
        }
        flags[i] = 1;
        if (j->op == FPE_OP_MOVE && same_volume(s.s,dest_nt)) {
            j->p.items_total++; /* a rename: no need to walk the tree */
        } else {
            flags[i] |= 2;
            scan_item(j, &s, &info[i]);
        }
        win_chain_close(&captured);
    }
    FPE_PROF_END("job.scan", t_scan);

    j->p.state = FPE_JOB_RUNNING;
    report(j, 1);

    for (uint32_t i = 0; i < j->nsrc && !cancelled(j); i++) {
        if (!(flags[i] & 1)) continue;
        wchar_t *nt = NULL;
        if ((resolved_sources[i]?!pb_set(&s,resolved_sources[i]):fpe_path_nt(j->src[i],&nt)||!pb_set(&s,nt))) {
            fpe_free(nt);
            fail(j, ERROR_FILENAME_EXCED_RANGE, j->src[i]);
            continue;
        }
        fpe_free(nt);

        if (j->op == FPE_OP_DELETE) {
            win_chain captured={0};DWORD err=win_source_parent(j,&s,&captured);
            if(!err&&!win_same_id(captured.id,source_parents[i]))err=ERROR_FILE_INVALID;
            if(err)fail(j,err,j->src[i]);
            else delete_item(j,&s,captured.items[captured.count-1],deletions[i]);
            win_chain_close(&captured);
            continue;
        }

        const wchar_t *base = wcsrchr(j->src[i], L'\\');
        base = base ? base + 1 : j->src[i];
        if (!*base || !pb_set(&d, dest_nt) || pb_push(&d, base, (uint32_t)wcslen(base)) == PB_FAIL) {
            fail(j, ERROR_INVALID_PARAMETER, j->src[i]);
            continue;
        }

        win_chain captured={0};DWORD guard=win_source_parent(j,&s,&captured);
        if(!guard&&!win_same_id(captured.id,source_parents[i]))guard=ERROR_FILE_INVALID;
        if(guard){win_chain_close(&captured);fail(j,guard,j->src[i]);continue;}
        HANDLE parent=dest_guard.items[dest_guard.count-1];
        if (j->op == FPE_OP_COPY) {
            /* Copying into the item's own folder always keeps both. */
            fpe_conflict policy = fpe_equal_nocase(s.s, s.len, d.s, d.len) ? FPE_CONFLICT_RENAME : j->policy;
            copy_item(j,&s,&d,captured.items[captured.count-1],parent,&info[i],0,policy);
        } else {
            move_item(j,&s,&d,captured.items[captured.count-1],parent,&info[i]);
        }
        win_chain_close(&captured);
    }

finish:
    j->cur = NULL;
    if(j->durability_error){j->p.state=FPE_JOB_DONE;}
    else if(j->namespace_error){j->p.state=FPE_JOB_FAILED;}
    else if (cancelled(j)) {
        j->p.state = FPE_JOB_CANCELLED;
    } else {
        j->p.state = FPE_JOB_DONE;
        if (j->p.items_done < j->p.items_total) j->p.items_done = j->p.items_total;
        if (j->p.bytes_done < j->p.bytes_total) j->p.bytes_done = j->p.bytes_total;
    }
    report(j, 1);

cleanup:
    win_chain_close(&dest_guard);fpe_free(source_parents);
    if(deletions){for(uint32_t i=0;i<j->nsrc;i++)win_delete_free(deletions[i]);fpe_free(deletions);}
    if(resolved_sources){for(uint32_t i=0;i<j->nsrc;i++)fpe_free(resolved_sources[i]);fpe_free(resolved_sources);}
    fpe_free(dest_nt);
    fpe_free(s.s);
    fpe_free(d.s);
    fpe_free(info);
    fpe_free(flags);
}

static int ancestry_live(void *user){return !cancelled(user);}
static void run_job(fpe_job *j)
{
    fpe_win_ancestry *ancestry=NULL;wchar_t *original=j->dest;
    /* The selected coordinator owns one fence and all its ancestry guards.
     * Native, streamed and same-volume fast moves all enter here before any
     * source unlink or destination mutation. Linux destinations fence in WSL. */
    if(j->dest&&!j->wsl_dest_endpoint&&!j->regular_only){
        fpe_err error=check_job_context(j);
        if(!error){j->p=(fpe_job_progress){.job=j->id,.op=j->op,.state=FPE_JOB_SCANNING};report(j,1);
            error=fpe_win_ancestry_open(j->dest,ancestry_live,j,&ancestry);}
        if(error){j->p=(fpe_job_progress){.job=j->id,.op=j->op,.state=error==ERROR_CANCELLED?FPE_JOB_CANCELLED:FPE_JOB_FAILED};
            if(error==ERROR_CANCELLED)report(j,1);else fail(j,error,j->dest);return;}
        j->ancestry=ancestry;j->dest=(wchar_t*)fpe_win_ancestry_path(ancestry);
    }
    j->defer_final=1;run_job_inner(j);
    if(ancestry){j->dest=original;j->ancestry=NULL;fpe_win_ancestry_close(ancestry);}
    j->defer_final=0;if(j->final_report){j->final_report=0;j->cur=NULL;report(j,1);}
}

/* One queue entry and one final outcome for selected relative files. Each
 * native helper receives a regular-only copy, never a directory operation. */
typedef struct tree_progress {fpe_job *parent;fpe_job_progress prior,last;const wchar_t *relative;} tree_progress;
static void tree_progress_cb(void *user,const fpe_job_progress *v)
{
    tree_progress *b=user;b->last=*v;b->last.current=NULL;fpe_job *j=b->parent;
    j->p=*v;j->p.op=FPE_OP_COPY;j->p.state=FPE_JOB_RUNNING;j->p.current=NULL;
    j->p.items_total=j->nsrc;j->p.items_done=b->prior.items_done+(v->items_done?1:0);
    j->p.bytes_done+=b->prior.bytes_done;j->p.bytes_total+=b->prior.bytes_total;
    j->p.errors+=b->prior.errors;j->p.skipped+=b->prior.skipped;
    if(!j->p.last_error)j->p.last_error=b->prior.last_error;j->cur=b->relative;report(j,0);
}
static void tree_event_cb(void *user,const fpe_job_event *v)
{
    tree_progress *b=user;if(b->parent->event_cb){fpe_job_event e=*v;e.path=b->relative;b->parent->event_cb(b->parent->user,&e);}
}
static int tree_live(void *user){return !cancelled(user);}
static wchar_t *tree_join(const wchar_t *a,const wchar_t *b)
{
    size_t n=wcslen(a),m=wcslen(b);if(n+m+2>FPE_MAX_PATH)return NULL;
    wchar_t *p=fpe_alloc((n+m+2)*sizeof *p);if(p)swprintf(p,n+m+2,L"%ls\\%ls",a,b);return p;
}
static fpe_err tree_sync(fpe_job *j,fpe_path_guard *g,fpe_path_guard *roots,fpe_path_guard *parents,unsigned depth)
{
    fpe_path_guard *held[258]={roots,roots+1};uint32_t count=2;
    for(unsigned i=0;i<depth*2;i++)held[count++]=parents+i;
    return fpe_guard_sync(g,held,count,tree_live,j);
}
static void run_tree(fpe_job *j)
{
    fpe_path_guard roots[2]={{0}},*parents=NULL;int root=j->wsl_as_root,barrier_failed=0;
    j->p=(fpe_job_progress){.job=j->id,.op=FPE_OP_COPY,.state=FPE_JOB_SCANNING,.items_total=j->nsrc};report(j,1);
    fpe_err err=fpe_guard_open(j->tree_root,0,0,0,root,tree_live,j,&roots[0]);
    if(!err)err=fpe_guard_open(j->dest,0,0,0,root,tree_live,j,&roots[1]);
    if(!err&&!roots[1].linux){err=fpe_win_ancestry_validate(j->ancestry,roots[1].directory);roots[1].ancestry=j->ancestry;}
    if(!err&&roots[1].linux)err=fpe_wsl_guard_ancestry(roots+1,tree_live,j);
    if(!err&&!(parents=fpe_zalloc(256*sizeof *parents)))err=ERROR_NOT_ENOUGH_MEMORY;
    if(!err){err=tree_sync(j,roots+1,roots,parents,0);if(err)barrier_failed=1;}
    if(err){if(err!=ERROR_CANCELLED)fail(j,err,j->dest);goto end;}
    for(uint32_t i=0;i<j->nsrc&&!cancelled(j);i++){
        wchar_t *rel=fpe_wcsdup(j->src[i]),*source=NULL;HANDLE leaf=INVALID_HANDLE_VALUE;unsigned depth=0;fpe_path_guard *at[]={&roots[0],&roots[1]};
        err=rel?0:ERROR_NOT_ENOUGH_MEMORY;wchar_t *part=rel,*slash;
        while(!err&&(slash=wcschr(part,L'\\'))){
            fpe_path_guard *dest_parent=at[1];
            *slash=0;for(int side=0;side<2&&!err;side++){
                fpe_path_guard *g=parents+depth*2+side;err=fpe_guard_child(at[side],part,side==1,root,tree_live,j,g);if(!err)at[side]=g;
            }
            depth++;part=slash+1;
            if(!err){err=tree_sync(j,at[1],roots,parents,depth);if(!err)err=tree_sync(j,dest_parent,roots,parents,depth);if(err)barrier_failed=1;}
        }
        if(!err&&!(source=tree_join(at[0]->path,part)))err=ERROR_FILENAME_EXCED_RANGE;
        /* Windows denies replacement of the selected leaf while copying. */
        if(!err&&!at[0]->linux){
            wchar_t *nt=NULL;err=fpe_path_nt(source,&nt);
            if(!err){leaf=CreateFileW(nt,FILE_READ_DATA|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,NULL);if(leaf==INVALID_HANDLE_VALUE)err=GetLastError();}
            fpe_free(nt);BY_HANDLE_FILE_INFORMATION info;
            if(!err&&!GetFileInformationByHandle(leaf,&info))err=GetLastError();
            if(!err&&(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DEVICE)))err=ERROR_NOT_SUPPORTED;
        }
        if(err){if(err!=ERROR_CANCELLED){fail(j,err,j->src[i]);j->p.items_done++;}}
        else{
            tree_progress progress={j,j->p,{0},j->src[i]};wchar_t *one[]={source};
            fpe_job child={.id=j->id,.op=FPE_OP_COPY,.policy=j->policy,.src=one,.nsrc=1,.dest=at[1]->path,.regular_only=1,.parent_stop=&j->cancel,.buf=j->buf,.cb=tree_progress_cb,.event_cb=tree_event_cb,.user=&progress,
                .wsl_native=j->wsl_native,.wsl_as_root=j->wsl_as_root,.wsl_context_checked=1,.ancestry=j->ancestry};
            run_job(&child);
            j->p.items_done=progress.prior.items_done+1;j->p.items_total=j->nsrc;
            j->p.bytes_done=progress.prior.bytes_done+child.p.bytes_done;j->p.bytes_total=progress.prior.bytes_total+child.p.bytes_total;
            j->p.errors=progress.prior.errors+child.p.errors;j->p.skipped=progress.prior.skipped+child.p.skipped;
            if(child.p.last_error)j->p.last_error=child.p.last_error;
            if(child.durability_error)j->durability_error=child.durability_error;
            /* No successful selected-copy completion until the leaf's entry
             * and every created/merged ancestor within the endpoint have
             * checked barriers. Keep all source and namespace guards alive. */
            for(unsigned n=depth+1;n&&!cancelled(j);n--){
                fpe_path_guard *g=n==1?roots+1:parents+(n-2)*2+1;
                err=tree_sync(j,g,roots,parents,depth);
                if(err){barrier_failed=1;if(err!=ERROR_CANCELLED)fail(j,err,j->src[i]);break;}
            }
        }
        if(leaf!=INVALID_HANDLE_VALUE)CloseHandle(leaf);fpe_free(source);fpe_free(rel);
        while(depth){depth--;fpe_guard_close(parents+depth*2+1);fpe_guard_close(parents+depth*2);}
        j->cur=NULL;j->p.state=FPE_JOB_RUNNING;report(j,1);
        if(barrier_failed)break;
    }
end:
    fpe_free(parents);fpe_guard_close(&roots[1]);fpe_guard_close(&roots[0]);
    j->cur=NULL;j->p.state=j->durability_error?FPE_JOB_DONE:cancelled(j)?FPE_JOB_CANCELLED:err&&j->p.items_done==0?FPE_JOB_FAILED:FPE_JOB_DONE;report(j,1);
}

static void free_job(fpe_job *j)
{
    for (uint32_t i = 0; i < j->nsrc; i++) fpe_free(j->src ? j->src[i] : NULL);
    fpe_free(j->src);
    for(uint32_t i=0;i<j->nsrc;i++)fpe_free(j->wsl_src_endpoints?j->wsl_src_endpoints[i]:NULL);
    fpe_free(j->wsl_src_endpoints);
    fpe_free(j->wsl_dest_endpoint);
    fpe_free(j->wsl_tree_endpoint);
    fpe_free(j->dest);
    fpe_free(j->tree_root);
    if (j->buf) VirtualFree(j->buf, 0, MEM_RELEASE);
    fpe_free(j);
}

static DWORD WINAPI job_thread(LPVOID p)
{
    fpe_jobs *q = p;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    for (;;) {
        AcquireSRWLockExclusive(&q->lock);
        while (!q->head && !q->stop) SleepConditionVariableSRW(&q->cv, &q->lock, INFINITE, 0);
        fpe_job *j = q->head;
        if (j) {
            q->head = j->next;
            if (!q->head) q->tail = NULL;
            q->current = j;
        }
        ReleaseSRWLockExclusive(&q->lock);
        if (!j) break;

        int64_t t = fpe_prof_begin();
        run_job(j);
        FPE_PROF_END("job.run", t);

        AcquireSRWLockExclusive(&q->lock);
        q->current = NULL;
        ReleaseSRWLockExclusive(&q->lock);
        free_job(j);
        fpe_gauge_add(&g_jobs_queued, -1);
    }

    if (SUCCEEDED(hr)) CoUninitialize();
    return 0;
}

/* ---- public ------------------------------------------------------------------ */

void fpe_jobs_init(fpe_jobs *q)
{
    memset(q, 0, sizeof *q);
    InitializeSRWLock(&q->lock);
    InitializeConditionVariable(&q->cv);
}

void fpe_jobs_shutdown(fpe_jobs *q)
{
    AcquireSRWLockExclusive(&q->lock);
    q->stop = 1;
    for (fpe_job *j = q->head; j; j = j->next) InterlockedExchange(&j->cancel, 1);
    if (q->current) InterlockedExchange(&q->current->cancel, 1);
    HANDLE t = q->thread;
    q->thread = NULL;
    WakeAllConditionVariable(&q->cv);
    ReleaseSRWLockExclusive(&q->lock);

    if (t) {
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
    }
}

static uint64_t submit(fpe_engine *e, const wchar_t *tree_root,fpe_op op, const wchar_t *const *sources, uint32_t count,
                                const wchar_t *dest_dir, fpe_conflict policy, fpe_job_cb cb, fpe_job_event_cb event_cb, void *user)
{
    if (!e || !sources || !count || op > FPE_OP_RECYCLE || policy > FPE_CONFLICT_FAIL) return 0;
    if ((op == FPE_OP_COPY || op == FPE_OP_MOVE) && (!dest_dir || !dest_dir[0])) return 0;

    fpe_job *j = fpe_zalloc(sizeof *j);
    if (!j) return 0;
    j->op = op;
    j->policy = policy;
    j->cb = cb;
    j->event_cb = event_cb;
    j->user = user;
    j->src = fpe_zalloc(count * sizeof(wchar_t *));
    j->buf = VirtualAlloc(NULL, FPE_ENUM_BUF, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!j->src || !j->buf) { free_job(j); return 0; }
    j->nsrc = count;
    for (uint32_t i = 0; i < count; i++) {
        fpe_err error = !sources[i] ? ERROR_INVALID_PARAMETER : tree_root ?
            ((j->src[i]=fpe_wcsdup(sources[i])) ? 0 : ERROR_NOT_ENOUGH_MEMORY) :
            fpe_path_operand(sources[i], &j->src[i]);
        if (error) { free_job(j); SetLastError(error); return 0; }
    }
    if (op == FPE_OP_COPY || op == FPE_OP_MOVE) {
        fpe_err error = fpe_path_operand(dest_dir, &j->dest);
        if (error) { free_job(j); SetLastError(error); return 0; }
    }

    if(tree_root&&!(j->tree_root=fpe_wcsdup(tree_root))){free_job(j);return 0;}
    int wsl_enabled;fpe_wsl_job_policy(&wsl_enabled,&j->wsl_as_root);
    j->wsl_src_endpoints=fpe_zalloc(count*sizeof *j->wsl_src_endpoints);
    if(!j->wsl_src_endpoints){free_job(j);return 0;}
    for(uint32_t i=0;i<count&&!tree_root;i++){
        if(fpe_wsl_endpoint(j->src[i],&j->wsl_src_endpoints[i])){free_job(j);return 0;}
        if(j->wsl_src_endpoints[i])j->wsl_native=wsl_enabled;
    }
    if(j->dest){
        if(fpe_wsl_endpoint(j->dest,&j->wsl_dest_endpoint)){free_job(j);return 0;}
        if(j->wsl_dest_endpoint)j->wsl_native=wsl_enabled;
    }
    if(j->tree_root){
        if(fpe_wsl_endpoint(j->tree_root,&j->wsl_tree_endpoint)){free_job(j);return 0;}
        if(j->wsl_tree_endpoint)j->wsl_native=wsl_enabled;
    }
    fpe_jobs *q = fpe_engine_jobs(e);
    AcquireSRWLockExclusive(&q->lock);
    if (q->stop) {
        ReleaseSRWLockExclusive(&q->lock);
        free_job(j);
        return 0;
    }
    if (!q->thread) {
        q->thread = CreateThread(NULL, JOB_STACK, job_thread, q, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (!q->thread) {
            ReleaseSRWLockExclusive(&q->lock);
            free_job(j);
            return 0;
        }
    }
    j->id = ++q->next_id;
    if (q->tail) q->tail->next = j;
    else q->head = j;
    q->tail = j;
    uint64_t id = j->id;
    fpe_gauge_add(&g_jobs_queued, 1);
    WakeConditionVariable(&q->cv);
    ReleaseSRWLockExclusive(&q->lock);
    return id;
}

FPE_API uint64_t fpe_job_submit_ex(fpe_engine *e,fpe_op op,const wchar_t *const *sources,uint32_t count,const wchar_t *dest,fpe_conflict policy,fpe_job_cb cb,fpe_job_event_cb events,void *user)
{return submit(e,NULL,op,sources,count,dest,policy,cb,events,user);}
FPE_API uint64_t fpe_job_submit_tree(fpe_engine *e,const wchar_t *root,const wchar_t *const *relative,uint32_t count,const wchar_t *dest,fpe_conflict policy,fpe_job_cb cb,fpe_job_event_cb events,void *user)
{
    SetLastError(ERROR_INVALID_PARAMETER);
    if(!root||!*root||!dest||!*dest||!relative||!count||count>256)return 0;
    size_t chars=0;
    for(uint32_t i=0;i<count;i++){
        if(!fpe_relative_valid(relative[i],!fpe_wsl_path_kind(root))||!fpe_relative_valid(relative[i],!fpe_wsl_path_kind(dest))){SetLastError(ERROR_INVALID_NAME);return 0;}
        chars+=wcslen(relative[i])+1;if(chars>60000){SetLastError(ERROR_BUFFER_OVERFLOW);return 0;}
        unsigned depth=0;for(const wchar_t *p=relative[i];*p;p++)if(*p==L'\\'&&++depth>128)return 0;
    }
    SetLastError(0);return submit(e,root,FPE_OP_COPY,relative,count,dest,policy,cb,events,user);
}

FPE_API uint64_t fpe_job_submit(fpe_engine *e, fpe_op op, const wchar_t *const *sources, uint32_t count,
                                const wchar_t *dest_dir, fpe_conflict policy, fpe_job_cb cb, void *user)
{
    return fpe_job_submit_ex(e, op, sources, count, dest_dir, policy, cb, NULL, user);
}

FPE_API void fpe_job_cancel(fpe_engine *e, uint64_t id)
{
    fpe_jobs *q = fpe_engine_jobs(e);
    AcquireSRWLockExclusive(&q->lock);
    for (fpe_job *j = q->head; j; j = j->next)
        if (!id || j->id == id) InterlockedExchange(&j->cancel, 1);
    if (q->current && (!id || q->current->id == id)) InterlockedExchange(&q->current->cancel, 1);
    ReleaseSRWLockExclusive(&q->lock);
}
