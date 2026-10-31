/*
 * fpe.h - OpenPilot file engine.
 *
 * A small, dependency-free C engine for building instant Windows file
 * explorers: bulk directory snapshots, natural sorting, quick filtering,
 * a stale-while-revalidate folder cache, change watching and a queued
 * copy/move/delete job runner.
 *
 * Conventions
 *   - Strings are UTF-16 (wchar_t), NUL-terminated unless a length is given.
 *   - fpe_err values are Win32 error codes; 0 (ERROR_SUCCESS) means success.
 *   - fpe_dir snapshots are immutable and reference counted; share them
 *     freely between threads.
 *   - fpe_view is NOT thread-safe; own each view from a single thread.
 *   - fpe_engine, fpe_watch and job functions are thread-safe.
 */
#ifndef FPE_H
#define FPE_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(FPE_BUILD_DLL)
#define FPE_API __declspec(dllexport)
#elif defined(FPE_USE_DLL)
#define FPE_API __declspec(dllimport)
#else
#define FPE_API
#endif

#define FPE_VERSION_MAJOR 0
#define FPE_VERSION_MINOR 1
#define FPE_VERSION_PATCH 0

typedef uint32_t fpe_err;

/* Live, read-only recursive filename search. Worker-only; no index or cache.
 * Words are ANDed; quotes keep spaces, * and ? match a whole name. Filters:
 * ext:txt;pdf, file:, folder:, size:>=10mb, dm:>=2024-01-01 (UTC).
 * Unsupported syntax is rejected, never silently interpreted as another query.
 * A NULL hit is a cancellation/progress pulse. Paths are borrowed for the call.
 * Links are returned but never followed. Linux uses the configured WSL user.
 * Limits: 300 hits, 100000 entries, 128 levels; limited/errors mean incomplete. */
typedef struct fpe_search_stats {
    uint32_t scanned, matched, skipped, errors, first_error, limited, cancelled;
} fpe_search_stats;
struct fpe_entry;
typedef int (*fpe_search_cb)(void *user,const wchar_t *path,const struct fpe_entry *entry,const fpe_search_stats *stats);
FPE_API fpe_err fpe_search_validate(const wchar_t *query);
/* Safe equivalent Everything query; metadata filters use live UTC metadata
 * and return ERROR_NOT_SUPPORTED here. Never passes arbitrary search syntax. */
FPE_API fpe_err fpe_search_index_query(const wchar_t *query,wchar_t *out,size_t cap);
FPE_API fpe_err fpe_search_folder(const wchar_t *folder,const wchar_t *query,fpe_search_cb cb,void *user,fpe_search_stats *out);

/* Optional native WSL filename index. Worker-only, cold until a WSL folder is
 * searched/managed; selected folders are persisted inside the distro per UID.
 * One active watched root per process; switching roots reconciles the saved
 * database before declaring it current. No index is implicitly enabled. */
enum {FPE_INDEX_NONE,FPE_INDEX_SCANNING,FPE_INDEX_CURRENT,FPE_INDEX_STALE,FPE_INDEX_PAUSED,FPE_INDEX_ERROR};
enum {FPE_INDEX_STATUS,FPE_INDEX_ENABLE,FPE_INDEX_PAUSE,FPE_INDEX_RESUME,FPE_INDEX_REBUILD,FPE_INDEX_REMOVE};
typedef struct fpe_index_status {
    uint32_t state,error,uid,enabled;
    uint64_t entries,scanned,errors,skipped,updated;
    wchar_t root[32768]; /* actual selected root; may be an ancestor */
} fpe_index_status;
typedef int (*fpe_index_cb)(void *user,const fpe_index_status *status);
FPE_API fpe_err fpe_search_index_control(const wchar_t *folder,uint32_t action,fpe_index_cb cb,void *user,fpe_index_status *out);
/* ERROR_RETRY / NOT_SUPPORTED: use live search and discard any provisional
 * candidates already delivered. Short, Unicode-only and metadata queries use
 * live search. The original matcher and current permissions decide each hit. */
FPE_API fpe_err fpe_search_folder_indexed(const wchar_t *folder,const wchar_t *query,fpe_search_cb cb,void *user,fpe_search_stats *out,fpe_index_status *index);
FPE_API void fpe_search_index_shutdown(void); /* worker/shutdown only */

/* ------------------------------------------------------------------------ */
/* Directory snapshots                                                       */
/* ------------------------------------------------------------------------ */

#define FPE_ENTRY_ASCII 0x1u /* name is pure ASCII (fast comparison path) */

typedef struct fpe_entry {
    uint64_t size;        /* bytes; 0 for directories */
    int64_t  mtime;       /* last write time, FILETIME (100 ns ticks since 1601 UTC) */
    int64_t  ctime;       /* creation time, FILETIME */
    uint32_t attrs;       /* FILE_ATTRIBUTE_* */
    uint32_t reparse_tag; /* IO_REPARSE_TAG_* if attrs has FILE_ATTRIBUTE_REPARSE_POINT */
    uint32_t name;        /* offset, in wchar_t, into the snapshot name pool */
    uint16_t name_len;    /* in wchar_t, excluding the terminating NUL */
    uint16_t ext;         /* offset of the extension (after the last '.'); name_len if none */
    uint32_t flags;       /* FPE_ENTRY_* */
    uint32_t reserved;
} fpe_entry;

static inline int fpe_entry_is_dir(const fpe_entry *e) { return (e->attrs & 0x10u) != 0; }

typedef struct fpe_dir fpe_dir;

typedef struct fpe_dir_stats {
    uint32_t entries;
    uint32_t dirs;
    uint32_t files;
    uint64_t file_bytes;   /* sum of file sizes (not recursive) */
    uint64_t memory_bytes; /* memory committed for this snapshot */
    uint64_t read_us;      /* wall time spent reading the directory */
    int64_t  taken_at;     /* FILETIME when the snapshot was taken */
} fpe_dir_stats;

/* Read a directory synchronously. Accepts relative, absolute, UNC and
 * \\?\ paths; long paths are supported. On success *out holds one reference. */
FPE_API fpe_err          fpe_dir_read(const wchar_t *path, fpe_dir **out);
FPE_API fpe_dir         *fpe_dir_retain(fpe_dir *dir);
FPE_API void             fpe_dir_release(fpe_dir *dir);
FPE_API uint32_t         fpe_dir_count(const fpe_dir *dir);
FPE_API const fpe_entry *fpe_dir_entries(const fpe_dir *dir);
FPE_API const wchar_t   *fpe_dir_name(const fpe_dir *dir, const fpe_entry *e);
FPE_API const wchar_t   *fpe_dir_path(const fpe_dir *dir); /* normalized, no \\?\ prefix */
FPE_API void             fpe_dir_get_stats(const fpe_dir *dir, fpe_dir_stats *out);

/* Linux details of an entry, for folders read inside a WSL distro by the
 * WSL helper (see fpe_wsl_configure). Strings live as long as the snapshot. */
typedef struct fpe_unix {
    uint32_t       mode;  /* st_mode: file type and permission bits */
    uint32_t       uid, gid;
    const wchar_t *owner; /* user name, NULL if the distro has none for uid */
    const wchar_t *group;
    const wchar_t *link;  /* where a symlink points, NULL for anything else */
} fpe_unix;

/* 1 if the snapshot came from the WSL helper and has Linux details. */
FPE_API int              fpe_dir_is_linux(const fpe_dir *dir);
/* 1 and *out filled for entries of such a snapshot, else 0. */
FPE_API int              fpe_dir_unix(const fpe_dir *dir, const fpe_entry *e, fpe_unix *out);
/* "drwxr-xr-x" (with s/S, t/T for setuid, setgid, sticky), NUL-terminated. */
FPE_API void             fpe_unix_mode_string(uint32_t mode, wchar_t out[11]);

/* Read-only current-level folder comparison. Names match case-insensitively
 * only when both snapshots are Windows. Neither metadata nor folder rows imply
 * equal contents. The caller owns the returned rows and retained snapshots. */
enum { FPE_COMPARE_LEFT=1,FPE_COMPARE_RIGHT,FPE_COMPARE_METADATA,FPE_COMPARE_SIZE,
    FPE_COMPARE_DATE,FPE_COMPARE_CONTENT,FPE_COMPARE_EQUAL,FPE_COMPARE_FOLDER,
    FPE_COMPARE_TYPE,FPE_COMPARE_EXCLUDED,FPE_COMPARE_AMBIGUOUS,FPE_COMPARE_ERROR };
typedef struct fpe_compare_row { int32_t left,right; uint32_t state,error; } fpe_compare_row;
typedef struct fpe_comparison {
    fpe_dir *dirs[2]; fpe_compare_row *rows; uint32_t count,completed;
    fpe_err error; int cancelled;
} fpe_comparison;
typedef int (*fpe_compare_cb)(void *user);
/* Hash regular files only, no leaf links/reparse points; detect changes during
 * the read. SHA-256 output is zero on any error. Worker-only, cancellable. */
typedef struct fpe_hash { uint64_t size; int64_t mtime; uint8_t sha256[32]; } fpe_hash;
FPE_API fpe_err fpe_file_hash(const wchar_t *path,int as_root,fpe_compare_cb cb,void *user,fpe_hash *out);
FPE_API fpe_err fpe_compare_folders(const wchar_t *left,const wchar_t *right,int hashes,int as_root,
    fpe_compare_cb cb,void *user,fpe_comparison *out);
FPE_API void fpe_compare_free(fpe_comparison *result);
/* Recursive depth-first rows use relative paths in retained virtual snapshots.
 * Folder rows describe scanned groups, never imply equal contents. 100000
 * combined entries,128 levels,64MiB of relative path text; no links/mounts.
 * Any unreadable/truncated subtree sets error; cancellation retains partial rows. */
FPE_API fpe_err fpe_compare_tree(const wchar_t *left,const wchar_t *right,int hashes,int as_root,
    fpe_compare_cb cb,void *user,fpe_comparison *out);

/* ------------------------------------------------------------------------ */
/* Views: sorted + filtered presentation of a snapshot                        */
/* ------------------------------------------------------------------------ */

typedef enum fpe_sort_key {
    FPE_SORT_NAME = 0, /* natural, case-insensitive ("file2" < "file10") */
    FPE_SORT_EXT,
    FPE_SORT_SIZE,
    FPE_SORT_MTIME,
    FPE_SORT_CTIME
} fpe_sort_key;

typedef struct fpe_view_opts {
    fpe_sort_key key;
    uint8_t descending;
    uint8_t dirs_first;
    uint8_t show_hidden;
    uint8_t show_system; /* protected OS files (hidden + system) */
} fpe_view_opts;

typedef struct fpe_view fpe_view;

/* opts may be NULL (name, ascending, dirs first, hidden files hidden). */
FPE_API fpe_view        *fpe_view_create(fpe_dir *dir, const fpe_view_opts *opts);
FPE_API void             fpe_view_destroy(fpe_view *v);
/* Swap in a newer snapshot, keeping sort options and filter. */
FPE_API fpe_err          fpe_view_set_dir(fpe_view *v, fpe_dir *dir);
FPE_API void             fpe_view_set_opts(fpe_view *v, const fpe_view_opts *opts);
FPE_API void             fpe_view_get_opts(const fpe_view *v, fpe_view_opts *out);
/* Case-insensitive substring filter on names. NULL or "" clears it.
 * Narrowing an existing filter only rescans the currently visible rows. */
FPE_API fpe_err          fpe_view_set_filter(fpe_view *v, const wchar_t *needle);
/* Optional additional UI-thread predicate. Context is borrowed until cleared or
 * the view is destroyed. Setting it always rebuilds, including changed context.
 * It must not mutate the view or retain borrowed entries/names. */
typedef int (*fpe_view_matcher)(const fpe_dir *, const fpe_entry *, const wchar_t *, void *);
FPE_API void             fpe_view_set_matcher(fpe_view *v, fpe_view_matcher match, void *context);
/* Items eligible under hidden/system options, before text/predicate filtering. */
FPE_API uint32_t         fpe_view_unfiltered_count(const fpe_view *v);
FPE_API fpe_dir         *fpe_view_dir(const fpe_view *v); /* borrowed */
FPE_API uint32_t         fpe_view_count(const fpe_view *v);
FPE_API const fpe_entry *fpe_view_at(const fpe_view *v, uint32_t i);
FPE_API uint32_t         fpe_view_entry_index(const fpe_view *v, uint32_t i); /* index into fpe_dir_entries */
FPE_API const wchar_t   *fpe_view_name(const fpe_view *v, uint32_t i);
/* Monotonic identity of this view's visible order; changes only when the
 * snapshot, ordering or filter changes. Copy order on the owning thread and
 * retain fpe_view_dir before processing those snapshot indices on a worker. */
FPE_API uint64_t         fpe_view_revision(const fpe_view *v);
FPE_API fpe_err          fpe_view_copy_order(const fpe_view *v, uint32_t *indices, uint32_t capacity);
/* Row of the entry with this name (case-insensitive), or UINT32_MAX. */
FPE_API uint32_t         fpe_view_find(const fpe_view *v, const wchar_t *name);

/* Natural, case-insensitive name comparison used by FPE_SORT_NAME. */
FPE_API int              fpe_compare_names(const wchar_t *a, const wchar_t *b);

/* ------------------------------------------------------------------------ */
/* Engine: async loading + folder cache                                       */
/* ------------------------------------------------------------------------ */

typedef struct fpe_engine fpe_engine;

typedef struct fpe_engine_config {
    uint32_t cache_dirs;  /* max cached snapshots (default 64) */
    uint64_t cache_bytes; /* max cached snapshot memory (default 64 MiB) */
} fpe_engine_config;

FPE_API fpe_engine *fpe_engine_create(const fpe_engine_config *cfg); /* cfg may be NULL */
/* Waits for in-flight loads, cancels and drains queued jobs. */
FPE_API void        fpe_engine_destroy(fpe_engine *e);

/* Cached snapshot for path (one reference) or NULL. Never touches the disk
 * beyond path normalization, so it is safe to call on the UI thread. */
FPE_API fpe_dir    *fpe_engine_cached(fpe_engine *e, const wchar_t *path);

/* Called on a worker thread. On success dir carries one reference that the
 * callee must release. On failure dir is NULL. */
typedef void (*fpe_load_cb)(void *user, uint64_t ticket, fpe_err err, fpe_dir *dir);

/* Read path on a worker thread and refresh the cache. Returns a ticket
 * (never 0) that is passed back to cb, or 0 if the request was rejected. */
FPE_API uint64_t    fpe_engine_load(fpe_engine *e, const wchar_t *path, fpe_load_cb cb, void *user);
FPE_API void        fpe_engine_invalidate(fpe_engine *e, const wchar_t *path);

/* ------------------------------------------------------------------------ */
/* Change watching                                                            */
/* ------------------------------------------------------------------------ */

typedef struct fpe_watch fpe_watch;

/* Called on the watcher thread after changes settle (debounced). If the
 * directory becomes unwatchable (deleted, unmounted) cb fires once more and
 * the watch goes idle; reload to discover the error. */
typedef void (*fpe_watch_cb)(void *user, fpe_watch *w);

FPE_API fpe_err fpe_watch_start(const wchar_t *path, uint32_t debounce_ms,
                                fpe_watch_cb cb, void *user, fpe_watch **out);
/* Blocks until the watcher thread exits. Do not call from inside cb. */
FPE_API void    fpe_watch_stop(fpe_watch *w);

/* ------------------------------------------------------------------------ */
/* WSL                                                                        */
/* ------------------------------------------------------------------------ */
/*
 * Folders inside WSL distros (\\wsl.localhost\<distro>\..., \\wsl$\...) are
 * listed and watched by folderwing-wsl, a small static Linux program next to the
 * engine, run in the distro with wsl.exe: one per distro, started on the
 * first such folder (on a thread; \\wsl.localhost answers until it runs) and
 * stopped idle_ms after the last folder closes. Without it (disabled, no
 * folderwing-wsl, no wsl.exe) those folders are read like any network share.
 */
typedef struct fpe_wsl_config {
    uint8_t  enabled; /* 0: never start the helper */
    uint8_t  as_root; /* run it as root rather than the distro's default user */
    uint32_t idle_ms; /* default 60000 */
} fpe_wsl_config;

FPE_API void fpe_wsl_configure(const fpe_wsl_config *cfg); /* NULL: defaults (enabled) */

typedef struct fpe_wsl_stats {
    uint32_t helpers;    /* running */
    uint32_t starts;     /* since the process started */
    uint32_t failures;   /* starts that didn't answer */
    uint32_t last_start_ms;
} fpe_wsl_stats;

FPE_API void fpe_wsl_get_stats(fpe_wsl_stats *out);

/* Reads a WSL folder through its distro's helper, waiting for the helper to
 * start (tools and tests; normal loads may use an unambiguous provider listing).
 * ERROR_NOT_SUPPORTED: not a WSL folder, or no helper can run. */
FPE_API fpe_err fpe_wsl_read(const wchar_t *path, fpe_dir **out);
/* Windows components ignore case; the Linux portion of a WSL path is exact. */
FPE_API int fpe_path_equal(const wchar_t *a, const wchar_t *b);
/* 1 for a WSL path when native integration is enabled (including mapped
 * drives). Does not start a helper. Mutations fail if it cannot be launched. */
FPE_API int fpe_wsl_native(const wchar_t *path);
/* An escaped Linux byte identity cannot be sent to Windows filesystem/shell
 * APIs. Does not depend on helper enablement and never starts a helper. */
FPE_API int fpe_wsl_requires_native(const wchar_t *path);
/* Namespace detection only, including mapped drives; independent of enablement. */
FPE_API int fpe_wsl_path_kind(const wchar_t *path);
/* Registered distro plus deployed native helper; metadata probe only. */
FPE_API int fpe_wsl_available(const wchar_t *path);
/* Exact-path mutations, synchronous for use on workers. Never overwrite a
 * rename or creation target. template_path may be NULL for an empty file. */
FPE_API fpe_err fpe_wsl_rename(const wchar_t *from, const wchar_t *to);
FPE_API fpe_err fpe_wsl_create(const wchar_t *path, int directory, const wchar_t *template_path);
/* Native Linux symlink, exact output path, no replacement or source mutation.
 * Relative targets use the physical source parent and output directory.
 * Windows targets use their distro-visible mount; other distros are refused.
 * compatible performs no filesystem I/O except the existing mapped-drive cache. */
FPE_API int fpe_wsl_symlink_compatible(const wchar_t *source, const wchar_t *destination);
FPE_API fpe_err fpe_wsl_symlink(const wchar_t *source, const wchar_t *output, int relative);

/* Fresh Linux permission metadata and guarded, nonrecursive chmod. Worker-only.
 * expected NULL queries; otherwise verifies identity/ctime/mode/ownership before
 * applying 0000..7777. Symlinks and special files are refused. as_root is an
 * explicitly captured WSL user choice, never an automatic retry. changed may
 * be set on an error when chmod ran but its requested mode was not retained. */
typedef struct fpe_permissions {
    uint64_t device, inode;
    int64_t ctime_sec;
    uint32_t ctime_nsec, mode, uid, gid;
    wchar_t owner[128], group[128];
} fpe_permissions;
FPE_API fpe_err fpe_wsl_permissions(const wchar_t *path, int as_root,
    const fpe_permissions *expected, uint32_t mode, fpe_permissions *out, int *changed);

/* Ownership is a separate, guarded operation: never chmod or restore privilege
 * bits after chown. Blank fields keep each item's value; resolve names/decimal
 * IDs once before showing the draft. Numeric UINT32_MAX (-1) is not accepted. */
#define FPE_ID_USER 1u
#define FPE_ID_GROUP 2u
typedef struct fpe_identity {
    uint32_t fields, uid, gid;
    wchar_t owner[128], group[128];
} fpe_identity;
FPE_API fpe_err fpe_wsl_identity(const wchar_t *context, int as_root,
    const wchar_t *owner, const wchar_t *group, fpe_identity *out, uint32_t *bad_field);
FPE_API fpe_err fpe_wsl_ownership(const wchar_t *path, int as_root,
    const fpe_permissions *expected, const fpe_identity *identity,
    fpe_permissions *out, int *changed);

/* Optional worker-scoped metadata connection for a bulk request. Single-thread
 * owned; caller must close on every exit. Captures as_root once, starts lazily,
 * and switches helpers when the distro changes. Each operation remains fresh
 * and guarded. A transport/protocol failure poisons the session: no replay or
 * automatic mutation retry. Existing one-item APIs keep their cold behavior. */
typedef struct fpe_wsl_metadata fpe_wsl_metadata;
FPE_API fpe_wsl_metadata *fpe_wsl_metadata_begin(int as_root);
FPE_API void fpe_wsl_metadata_end(fpe_wsl_metadata *session);
FPE_API fpe_err fpe_wsl_metadata_permissions(fpe_wsl_metadata *session,
    const wchar_t *path, const fpe_permissions *expected, uint32_t mode,
    fpe_permissions *out, int *changed);
FPE_API fpe_err fpe_wsl_metadata_ownership(fpe_wsl_metadata *session,
    const wchar_t *path, const fpe_permissions *expected, const fpe_identity *identity,
    fpe_permissions *out, int *changed);

/* Recursive logical folder size. Worker-only, uncached, metadata-only.
 * One root per call; folders includes the root. Regular-file directory entries
 * are counted independently (hard links count again). Links/reparse points,
 * special files and Linux mount boundaries are excluded and counted as skipped.
 * Depth is bounded to 128; unreadable/changed/over-limit items produce PARTIAL.
 * This is a best-effort scan of a live filesystem, not an atomic snapshot.
 * cb runs on the caller's thread about every 50 ms, initially and finally;
 * return zero to cancel. Cancellation is checked between filesystem calls.
 * WSL uses an isolated native helper and the explicitly captured as_root user;
 * unavailable/disabled native integration fails rather than following 9P links. */
enum { FPE_SIZE_RUNNING, FPE_SIZE_COMPLETE, FPE_SIZE_PARTIAL, FPE_SIZE_CANCELLED };
typedef struct fpe_size_result {
    uint64_t bytes, files, folders, skipped, errors;
    uint32_t first_error, state;
} fpe_size_result;
typedef int (*fpe_size_cb)(void *user, const fpe_size_result *result);
FPE_API fpe_err fpe_folder_size(const wchar_t *path, int as_root,
    fpe_size_cb cb, void *user, fpe_size_result *out);

/* A bounded, explicit rename draft for one parent folder. Names are Windows
 * display components (WSL reserved characters use U+F000 aliases; genuine aliases/escape use U+F080 quoting).
 * Preview (apply=0) fills identities and READY/UNCHANGED/error rows. Apply
 * requires that preview, rechecks the whole batch before changing anything,
 * then renames in order without replacing targets. Occupied targets, including
 * other selected names, are conflicts; no staging/swap/rollback is performed.
 * File contents are never read. Linux identity checks are best effort against
 * concurrent changes; kernel no-replace still protects destination contents.
 * cb runs on the caller's worker thread; zero stops remaining items after the
 * current request. A broken apply reply produces UNCERTAIN rows, never a retry.
 * Inspect every row even when the function returns an error. */
#define FPE_RENAME_MAX 256u
enum { FPE_RENAME_PENDING, FPE_RENAME_READY, FPE_RENAME_UNCHANGED,
       FPE_RENAME_RENAMED, FPE_RENAME_ERROR, FPE_RENAME_STOPPED,
       FPE_RENAME_BLOCKED, FPE_RENAME_UNCERTAIN };
typedef struct fpe_rename_identity {
    uint64_t device, inode, stamp;
    uint32_t nsec, attrs; /* attrs uses FILE_ATTRIBUTE_DIRECTORY/REPARSE_POINT */
} fpe_rename_identity;
typedef struct fpe_rename_item {
    wchar_t from[260], to[260];
    fpe_rename_identity identity;
    uint32_t state, error;
} fpe_rename_item;
typedef int (*fpe_rename_cb)(void *user, uint32_t completed, int applying);
FPE_API fpe_err fpe_rename_batch(const wchar_t *folder, int as_root,
    fpe_rename_item *items, uint32_t count, int apply, fpe_rename_cb cb, void *user);

/* ------------------------------------------------------------------------ */
/* File operation jobs (run one at a time, in submission order)              */
/* ------------------------------------------------------------------------ */

typedef enum fpe_op {
    FPE_OP_COPY = 0,
    FPE_OP_MOVE,
    FPE_OP_DELETE,  /* permanent */
    FPE_OP_RECYCLE  /* to the Recycle Bin */
} fpe_op;

typedef enum fpe_conflict {
    FPE_CONFLICT_RENAME = 0, /* keep both: "name (2).ext" */
    FPE_CONFLICT_SKIP,       /* keep the existing item; folders are merged */
    FPE_CONFLICT_OVERWRITE,  /* replace files; folders are merged */
    FPE_CONFLICT_FAIL        /* count an error (ERROR_FILE_EXISTS) */
} fpe_conflict;

typedef enum fpe_job_state {
    FPE_JOB_QUEUED = 0,
    FPE_JOB_SCANNING,
    FPE_JOB_RUNNING,
    FPE_JOB_DONE,      /* finished; check errors/skipped */
    FPE_JOB_CANCELLED,
    FPE_JOB_FAILED     /* could not start; see last_error */
} fpe_job_state;

typedef struct fpe_job_progress {
    uint64_t       job;
    fpe_op         op;
    fpe_job_state  state;
    uint64_t       bytes_done;
    uint64_t       bytes_total;
    uint32_t       items_done;
    uint32_t       items_total;
    uint32_t       errors;
    uint32_t       skipped;
    fpe_err        last_error;
    const wchar_t *current; /* item being processed; valid during the callback only */
} fpe_job_progress;

/* Called on the job thread, throttled to ~20 Hz, and exactly once with a
 * final state (DONE, CANCELLED or FAILED). */
typedef void (*fpe_job_cb)(void *user, const fpe_job_progress *p);

/* dest_dir is required for COPY and MOVE and ignored otherwise.
 * Returns a job id, or 0 if the arguments are invalid. Windows job operands
 * containing a literal component ending in dot/space are refused before
 * normalization with GetLastError()==ERROR_INVALID_NAME. Typed . and .. retain
 * navigation semantics; exact native Linux names are preserved. Windows
 * permanent delete verifies scanned identities through held no-follow objects;
 * changed/new entries and unsupported identity queries produce job errors. */
FPE_API uint64_t fpe_job_submit(fpe_engine *e, fpe_op op,
                                const wchar_t *const *sources, uint32_t count,
                                const wchar_t *dest_dir, fpe_conflict policy,
                                fpe_job_cb cb, void *user);
/* Optional per-item diagnostics. Strings are valid only during the callback,
 * on the job thread. Errors identify the failing source or destination;
 * skipped links and existing files say why. The original submit ABI is kept. */
enum { FPE_JOB_ERROR, FPE_JOB_SKIPPED };
typedef struct fpe_job_event {
    uint64_t job;
    uint32_t kind;
    fpe_err error;
    const wchar_t *path;
} fpe_job_event;
typedef void (*fpe_job_event_cb)(void *user, const fpe_job_event *event);
FPE_API uint64_t fpe_job_submit_ex(fpe_engine *e, fpe_op op,
                                   const wchar_t *const *sources, uint32_t count,
                                   const wchar_t *dest_dir, fpe_conflict policy,
                                   fpe_job_cb cb, fpe_job_event_cb event_cb, void *user);

/* Selected regular files, preserving relative parent directories under dest.
 * One reviewed copy job, at most256 relative paths and60000 path characters; no directory recursion.
 * Missing parents are created only on the job worker, after Start. */
FPE_API uint64_t fpe_job_submit_tree(fpe_engine *e,const wchar_t *source_root,
    const wchar_t *const *relative,uint32_t count,const wchar_t *dest_root,
    fpe_conflict policy,fpe_job_cb cb,fpe_job_event_cb events,void *user);

/* Cancel one job, or every queued and running job when job == 0. */
FPE_API void     fpe_job_cancel(fpe_engine *e, uint64_t job);

/* ------------------------------------------------------------------------ */
/* Profiling (debug mode)                                                     */
/* ------------------------------------------------------------------------ */
/*
 * Zones time named pieces of work ("area.what"); reports group them by the
 * part before the first '.', so each feature gets a subtotal. Zones are off
 * until fpe_prof_enable(1): while off, a zone costs one call and a branch.
 * Times are inclusive (a zone contains the zones it calls). Name sub-steps
 * "parent/child" (e.g. "dir.read/query"): reports nest them under the
 * parent, leave them out of the feature subtotal, and show the parent's
 * self time (total minus its children).
 *
 * Gauges track live amounts (counts, bytes). They are updated only on rare
 * events (a snapshot created or freed, a job queued) and are always on, so
 * enabling profiling later still shows correct current values.
 *
 *   int64_t t = fpe_prof_begin();
 *   ... work ...
 *   FPE_PROF_END("ui.paint", t);
 */

typedef struct fpe_zone {
    const char       *name;
    volatile int64_t  calls;
    volatile int64_t  ticks;
    volatile int64_t  max_ticks;
    volatile long     state;
    struct fpe_zone  *next;
} fpe_zone;

typedef struct fpe_gauge {
    const char       *name; /* names ending in ".bytes" are shown as sizes */
    volatile int64_t  value;
    volatile int64_t  peak;
    volatile long     state;
    struct fpe_gauge *next;
} fpe_gauge;

#define FPE_ZONE_INIT(label)  { (label), 0, 0, 0, 0, NULL }
#define FPE_GAUGE_INIT(label) { (label), 0, 0, 0, NULL }

/* One static zone per call site; sites sharing a label are merged in reports. */
#define FPE_PROF_END(label, t0)                                   \
    do {                                                          \
        static fpe_zone fpe_prof_zone_ = FPE_ZONE_INIT(label);    \
        fpe_prof_end(&fpe_prof_zone_, (t0));                      \
    } while (0)

/* Counts calls without timing them, for code too hot to time (inner loops). */
#define FPE_PROF_COUNT(label)                                     \
    do {                                                          \
        static fpe_zone fpe_prof_zone_ = FPE_ZONE_INIT(label);    \
        fpe_prof_count(&fpe_prof_zone_);                          \
    } while (0)

typedef struct fpe_prof_row {
    const char *name;
    int         is_gauge;
    int64_t     calls;    /* zones */
    double      total_ms;
    double      max_ms;
    int64_t     value;    /* gauges */
    int64_t     peak;
} fpe_prof_row;

FPE_API void     fpe_prof_enable(int on);
FPE_API int      fpe_prof_enabled(void);
FPE_API int64_t  fpe_prof_begin(void); /* 0 when profiling is off */
FPE_API void     fpe_prof_end(fpe_zone *z, int64_t t0);
FPE_API void     fpe_prof_count(fpe_zone *z); /* no-op while profiling is off */
FPE_API void     fpe_gauge_add(fpe_gauge *g, int64_t delta);
/* Clears zone counters and resets gauge peaks to current values. */
FPE_API void     fpe_prof_reset(void);
/* Copies merged zones (with calls) and gauges; returns how many were written. */
FPE_API uint32_t fpe_prof_rows(fpe_prof_row *out, uint32_t max);
/* Plain-text (UTF-8) report: zones grouped by feature, heaviest first, then
 * gauges. Always NUL-terminates; returns the length written. */
FPE_API size_t   fpe_prof_format(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* FPE_H */
