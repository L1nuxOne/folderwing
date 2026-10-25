#ifndef FPE_INTERNAL_H
#define FPE_INTERNAL_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>
#include <string.h>

#include "fpe.h"

#define FPE_MAX_PATH 32768u
#define FPE_ENUM_BUF (256u * 1024u)

#ifndef SORT_DIGITSASNUMBERS
#define SORT_DIGITSASNUMBERS 0x00000008
#endif

/* Junctions, symlinks and mount points: never descend into these when
 * walking trees, or deletes and copies can escape the selected folder. */
#define FPE_IS_NAME_SURROGATE(tag) (((tag) & 0x20000000u) != 0)

/* ---- memory ---------------------------------------------------------- */

void *fpe_alloc(size_t n);
void *fpe_zalloc(size_t n);
void *fpe_realloc(void *p, size_t n);
void  fpe_free(void *p);
wchar_t *fpe_wcsdup(const wchar_t *s);

/* Growable buffer backed by reserved virtual memory: commits on demand,
 * never fragments the heap, and frees in one call. Pointers into it stay
 * valid until the next push (a push may relocate when the reservation is
 * exceeded). */
typedef struct fpe_vbuf {
    uint8_t *base;
    size_t   used;
    size_t   committed;
    size_t   reserved;
} fpe_vbuf;

int   fpe_vbuf_init(fpe_vbuf *b, size_t reserve);
void *fpe_vbuf_push(fpe_vbuf *b, size_t n);
void  fpe_vbuf_trim(fpe_vbuf *b); /* decommit pages past `used` */
void  fpe_vbuf_free(fpe_vbuf *b);

/* ---- text ------------------------------------------------------------- */

void fpe_text_init(void);
extern uint16_t fpe_fold_table[65536];
static inline wchar_t fpe_fold(wchar_t c) { return (wchar_t)fpe_fold_table[(uint16_t)c]; }

int fpe_name_cmp(const wchar_t *a, uint32_t na, int a_ascii,
                 const wchar_t *b, uint32_t nb, int b_ascii);
int fpe_is_ascii(const wchar_t *s, uint32_t n);
int fpe_equal_nocase(const wchar_t *a, uint32_t na, const wchar_t *b, uint32_t nb);
/* needle must already be folded */
int fpe_contains_folded(const wchar_t *hay, uint32_t nh, const wchar_t *needle, uint32_t nn);

/* ---- paths ------------------------------------------------------------ */

/* Full path without \\?\ prefix and without trailing separator (roots keep
 * theirs). Caller frees. */
fpe_err fpe_path_display(const wchar_t *in, wchar_t **out);
/* Job-only normalization: refuse ambiguous native Windows suffixes. */
fpe_err fpe_path_operand(const wchar_t *in, wchar_t **out);
/* \\?\ form of a display path for Win32 calls that accept long paths. */
fpe_err fpe_path_nt(const wchar_t *display, wchar_t **out);
uint32_t fpe_path_hash(const wchar_t *s);

/* ---- enumeration ------------------------------------------------------ */

typedef int (*fpe_enum_fn)(void *ctx, const FILE_FULL_DIR_INFO *info);
/* Streams every entry (including "." and "..") to fn; fn returns 0 to stop. */
fpe_err fpe_enum(const wchar_t *nt_path, uint8_t *buf, uint32_t cap, fpe_enum_fn fn, void *ctx);

const wchar_t *fpe_dir_names(const fpe_dir *dir); /* name pool base */

/* Snapshot read on an already-normalized path. buf may be NULL. */
fpe_err fpe_dir_read_nt(const wchar_t *display, const wchar_t *nt, uint8_t *buf,
                        size_t reserve_hint, fpe_dir **out);
/* The same, but folders inside WSL distros go through the WSL helper when it runs. */
fpe_err fpe_dir_load(const wchar_t *display, const wchar_t *nt, fpe_dir **out);

/* Building a snapshot from another source (the WSL helper): new, add each
 * entry (Linux listings: fpe_dir_add_unix right after each fpe_dir_add),
 * then finish; on failure release it. */
fpe_dir *fpe_dir_new(const wchar_t *display, size_t reserve_hint);
fpe_err  fpe_dir_add(fpe_dir *d, const wchar_t *name, uint32_t n, uint64_t size, int64_t mtime, int64_t ctime,
                     uint32_t attrs, uint32_t reparse_tag);
fpe_err  fpe_dir_add_unix(fpe_dir *d, uint32_t mode, uint32_t uid, uint32_t gid, const wchar_t *link, uint32_t link_len);
fpe_err  fpe_dir_add_owner(fpe_dir *d, uint32_t id, int group, const wchar_t *name, uint32_t n);
fpe_err  fpe_dir_finish(fpe_dir *d);

/* ---- WSL (wsl.c) ------------------------------------------------------ */

/* fpe_wsl_list: no answer from the helper (not running yet, starting, gone,
 * or it failed to start); \\wsl.localhost is the way to read the folder */
#define FPE_WSL_NOT_READY 0x20000001u

/* A folder inside a WSL distro: \\wsl.localhost\<distro>\... or \\wsl$\<distro>\... */
int     fpe_wsl_owns(const wchar_t *display);
/* Lists through the distro's helper. wait = 0: FPE_WSL_NOT_READY unless it
 * runs (a start is requested); wait = 1: waits for it to start. Any other
 * error is the distro's answer about the folder. */
fpe_err fpe_wsl_list(const wchar_t *display, int wait, fpe_dir **out);
/* Watches through the helper: ev is set on every change (fpe_watch debounces). */
typedef struct wsl_watch wsl_watch;
wsl_watch *fpe_wsl_watch(const wchar_t *display, HANDLE ev);
void       fpe_wsl_unwatch(wsl_watch *w);
void       fpe_wsl_shutdown(void);
fpe_err fpe_wsl_size(const wchar_t *path, int as_root, fpe_size_cb cb, void *user, fpe_size_result *out);
fpe_err fpe_wsl_rename_batch(const wchar_t *folder, int as_root, fpe_rename_item *items,
    uint32_t count, int apply, fpe_rename_cb cb, void *user);
fpe_err fpe_wsl_job(uint32_t op, fpe_conflict policy, const wchar_t *const *sources, uint32_t count,
                    const wchar_t *dest, uint64_t id, fpe_job_cb cb, fpe_job_event_cb events, void *user,
                    volatile LONG *cancel, int as_root);
typedef struct fpe_win_ancestry fpe_win_ancestry;
fpe_err fpe_wsl_job_guarded(uint32_t op,fpe_conflict policy,const wchar_t *const *sources,uint32_t count,
                    const wchar_t *dest,uint64_t id,fpe_job_cb cb,fpe_job_event_cb events,void *user,
                    volatile LONG *cancel,int as_root,fpe_win_ancestry *ancestry);
void fpe_wsl_job_policy(int *enabled, int *as_root);
fpe_err fpe_wsl_endpoint(const wchar_t *path, wchar_t **out);

/* ---- time ------------------------------------------------------------- */

uint64_t fpe_now_us(void);

/* ---- jobs (engine internals) ------------------------------------------ */

typedef struct fpe_job fpe_job;

typedef struct fpe_jobs {
    SRWLOCK            lock;
    CONDITION_VARIABLE cv;
    fpe_job           *head;
    fpe_job           *tail;
    fpe_job           *current;
    HANDLE             thread;
    int                stop;
    uint64_t           next_id;
} fpe_jobs;

void fpe_jobs_init(fpe_jobs *j);
void fpe_jobs_shutdown(fpe_jobs *j);
fpe_jobs *fpe_engine_jobs(fpe_engine *e);

fpe_err fpe_wsl_hash(const wchar_t *path,int as_root,fpe_compare_cb cb,void *user,fpe_hash *out);

fpe_err fpe_compare_level(const wchar_t *left,const wchar_t *right,int hashes,int root,fpe_compare_cb cb,void *user,fpe_comparison *out);

/* Rename an already-held object within its captured directory (parent=NULL),
 * or relative to an explicitly held destination directory. */
DWORD fpe_win_rename_handle(HANDLE file,HANDLE parent,const wchar_t *name,int replace,int readonly);
HANDLE fpe_win_reopen_directory(HANDLE directory,DWORD access,DWORD share);
DWORD fpe_win_ancestry_open(const wchar_t *path,fpe_compare_cb cb,void *user,fpe_win_ancestry **out);
const wchar_t *fpe_win_ancestry_path(fpe_win_ancestry *ancestry);
HANDLE fpe_win_ancestry_root(fpe_win_ancestry *ancestry);
DWORD fpe_win_ancestry_validate(fpe_win_ancestry *ancestry,HANDLE directory);
DWORD fpe_win_ancestry_exchange(fpe_win_ancestry *ancestry,HANDLE directory,int restore);
void fpe_win_ancestry_close(fpe_win_ancestry *ancestry);

/* Private directory guard: caller keeps ancestors alive until children close. */
typedef struct fpe_path_guard {
    wchar_t *path; uint64_t device; int linux;
    HANDLE directory,barrier,to,from,process;
    fpe_win_ancestry *ancestry; /* borrowed from the selected-copy coordinator */
} fpe_path_guard;
fpe_err fpe_guard_open(const wchar_t *path,int anchored,uint64_t device,int create,int as_root,
    fpe_compare_cb cb,void *user,fpe_path_guard *out);
void fpe_guard_close(fpe_path_guard *guard);
fpe_err fpe_guard_child(const fpe_path_guard *parent,const wchar_t *relative,int create,int root,
    fpe_compare_cb cb,void *user,fpe_path_guard *out);
fpe_err fpe_guard_sync(fpe_path_guard *guard,fpe_path_guard *const *held,uint32_t count,
    fpe_compare_cb cb,void *user);
fpe_err fpe_wsl_guard_sync(fpe_path_guard *guard,fpe_compare_cb cb,void *user);
fpe_err fpe_wsl_guard_ancestry(fpe_path_guard *guard,fpe_compare_cb cb,void *user);
fpe_err fpe_wsl_guard(const wchar_t *path,int anchored,uint64_t device,int create,int as_root,
    fpe_compare_cb cb,void *user,fpe_path_guard *out);
int fpe_wsl_root(void);
int fpe_relative_valid(const wchar_t *relative,int windows);
typedef int (*fpe_search_entry_cb)(void *user,const wchar_t *relative,const fpe_entry *entry);
fpe_err fpe_wsl_search(const wchar_t *folder,fpe_compare_cb pulse,fpe_search_entry_cb hit,void *user,uint32_t *skipped,uint32_t *errors);
fpe_err fpe_wsl_index_search(const wchar_t *folder,const char *glob,fpe_compare_cb pulse,fpe_search_entry_cb hit,void *user,uint32_t *skipped,uint32_t *errors,fpe_index_status *index);


#endif
