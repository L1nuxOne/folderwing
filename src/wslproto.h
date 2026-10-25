/*
 * wslproto.h - messages between the engine and folderwing-wsl, its helper inside a
 * WSL distro (linux/fpwsl.c).
 *
 * The helper runs as `wsl.exe --distribution D --cd <its folder> --exec
 * ./folderwing-wsl`; wsl.exe relays its stdin and stdout byte for byte. Every message
 * is a header and `size` payload bytes, little endian on both sides. The
 * helper answers requests in order, one at a time; CHANGED is pushed
 * whenever a watched folder changes.
 *
 * Paths and names are the distro's own bytes (normally UTF-8); the engine
 * maps them to and from Windows names the way \\wsl.localhost does.
 */
#ifndef WSLPROTO_H
#define WSLPROTO_H

#include <stdint.h>

#define WSLP_MAGIC   0x31505357u /* "WSP1" */
#define WSLP_VERSION 8u /* Bounded Windows group commit, fresh source checks and durable results. */
#define WSLP_JOB_MAX (16u << 20)
#define WSLP_MAX     (1u << 20)  /* largest payload either side sends */

enum {
    WSLP_HELLO = 1, /* helper, first: wslp_hello */
    WSLP_LIST,      /* engine: path; helper answers ENTRIES... then LIST_END with the same id */
    WSLP_ENTRIES,   /* helper: wslp_entry records, each followed by name and link bytes, padded to 8 */
    WSLP_OWNERS,    /* helper: wslp_owner records for the uids and gids of the listing, before LIST_END */
    WSLP_LIST_END,  /* helper: wslp_end */
    WSLP_WATCH,     /* engine: path; id names the watch from now on; helper answers WATCHED */
    WSLP_WATCHED,   /* helper: wslp_end (err only) */
    WSLP_UNWATCH,   /* engine: no payload; no answer */
    WSLP_CHANGED,   /* helper: no payload; the watched folder changed, or is gone (then the watch ends) */
    WSLP_JOB,       /* --jobs helper: wslp_job then destination + source path records */
    WSLP_JOB_PROGRESS, /* wslp_progress + a path suffix for current item */
    WSLP_JOB_EVENT, /* wslp_event + path suffix, root selects original submitted path */
    WSLP_JOB_CANCEL,
    WSLP_JOB_CONTINUE, /* acknowledge RUNNING before the first mutation */
    WSLP_FILE_BEGIN, /* Linux -> Windows: wslp_file + native suffix, root 0 */
    WSLP_FILE_DATA,  /* raw bytes, bounded to 64 KiB */
    WSLP_FILE_END,   /* int32 errno: 0 commits; a failure aborts the temporary file */
    WSLP_FILE_ACK,   /* Windows -> Linux: int32 errno, after commit/abort */

    WSLP_RESOLVE, /* --resolve-path: literal Linux path bytes */
    WSLP_RESOLVED, /* wslp_resolved followed by distro and physical directory bytes */
    WSLP_PERMISSIONS, /* --permissions: wslp_permission_request + literal path */
    WSLP_PERMISSION_RESULT, /* wslp_permission_result; fresh stat after a change */
    WSLP_IDENTITY_RESOLVE, /* --permissions: bounded literal owner/group names or IDs */
    WSLP_IDENTITY_RESULT,
    WSLP_OWNERSHIP, /* wslp_ownership_request + literal path; guarded chown only */
    WSLP_OWNERSHIP_RESULT, /* wslp_permission_result; actual retained IDs and mode */
    WSLP_TRANSFER, /* --export/--import: wslp_transfer + one literal native path */
    WSLP_TRANSFER_NODE, /* wslp_node + one component and optional literal link target */
    WSLP_TRANSFER_DATA, /* regular file bytes, at most 64 KiB */
    WSLP_TRANSFER_END, /* wslp_transfer_end; SHA-256 and guarded source-read status */
    WSLP_TRANSFER_ACK, /* wslp_transfer_ack; destination verified and committed */
    WSLP_TRANSFER_DIR_END, /* no payload; finalize current directory */
    WSLP_TRANSFER_QUIT, /* no payload; clean importer shutdown */
    WSLP_SIZE, /* --sizes: literal absolute native path, 1..16384 bytes */
    WSLP_SIZE_RESULT, /* wslp_size; streamed running snapshots, then one final */
    WSLP_SIZE_CANCEL, /* no payload, id 1 */
    WSLP_RENAME_BATCH, /* --rename: request + root bytes + item/name records */
    WSLP_RENAME_PROGRESS, /* completed count + apply flag; no authoritative results */
    WSLP_RENAME_RESULT, /* count/stopped + one result per item */
    WSLP_RENAME_CANCEL, /* no payload, id 1 */
    WSLP_HASH, /* --hash: absolute literal native path */
    WSLP_HASH_RESULT, /* wslp_hash, periodic progress then done=1 */
    WSLP_GUARD, /* --guard: request + literal absolute directory */
    WSLP_GUARD_RESULT, /* pinned directory, helper stays alive until EOF */
    WSLP_DIRECTORY_SYNC = 54, /* --jobs Linux -> Windows: wslp_directory + native suffix; 45..53 are search/index */
    WSLP_DIRECTORY_ACK, /* Windows -> Linux: int32 errno after directory/parent flush */
    WSLP_GUARD_SYNC, /* --guard: empty request to fsync its live pinned directory */
    WSLP_GUARD_ACK, /* --guard: int32 errno; pin remains live until EOF */
    WSLP_GUARD_ANCESTRY, /* --guard: fence existing parents within this mount; GUARD_ACK */
    WSLP_SOURCE_CHECK, /* --jobs: wslp_source_check + suffix; checked Windows source stream inventory */
    WSLP_SOURCE_ACK, /* Windows -> Linux: int32 errno; no unsupported stream may be discarded */
    WSLP_FILE_PREPARE, /* wslp_prepared_file + suffix; copy-only, bounded stage */
    WSLP_FILE_PREPARED, /* wslp_file_result; staging credit, NEVER durable completion */
    WSLP_FILE_COMMIT, /* wslp_file_result; ordered source revalidation/abort */
    WSLP_FILE_COMMITTED, /* wslp_file_result; after all ordinary per-file barriers */
    WSLP_GROUP_COMMIT, WSLP_GROUP_CHECK, WSLP_GROUP_CHECKED, WSLP_GROUP_COMMITTED,
};

typedef struct wslp_source_check {uint32_t root,path_bytes;} wslp_source_check;

/* metadata=1 only finalizes a newly copied directory; merges retain their
 * metadata policy. Windows applies nanosecond source times at 100ns precision. */
typedef struct wslp_directory {
    uint32_t root,path_bytes,metadata,pad;
    int64_t atime,mtime;
} wslp_directory;

/* State 0 running, 1 complete, 2 partial, 3 cancelled. Error is Linux errno. */
typedef struct wslp_guard_request {uint64_t device;uint32_t anchored,create;} wslp_guard_request;
typedef struct wslp_guard_result {uint64_t device;uint32_t error,pid,fd,pad;} wslp_guard_result;

typedef struct wslp_hash {uint64_t size;int64_t mtime;uint8_t sha256[32];uint32_t err,done;} wslp_hash;

typedef struct wslp_size {
    uint64_t bytes, files, folders, skipped, errors;
    uint32_t first_error, state;
} wslp_size;

typedef struct wslp_rename_identity {uint64_t device,inode,stamp;uint32_t nsec,attrs;} wslp_rename_identity;
typedef struct wslp_rename_request {uint32_t apply,count,root_bytes,pad;} wslp_rename_request;
typedef struct wslp_rename_item {wslp_rename_identity identity;uint32_t from_bytes,to_bytes;} wslp_rename_item;
typedef struct wslp_rename_result {wslp_rename_identity identity;uint32_t state,error;} wslp_rename_result;
typedef struct wslp_rename_end {uint32_t count,stopped;} wslp_rename_end;
typedef struct wslp_rename_progress {uint32_t completed,apply;} wslp_rename_progress;

typedef struct wslp_permission_state {
    uint64_t device, inode;
    int64_t ctime_sec;
    uint32_t ctime_nsec, mode, uid, gid;
} wslp_permission_state;
typedef struct wslp_permission_request {
    uint32_t apply, mode, path_bytes, pad;
    wslp_permission_state expected;
} wslp_permission_request;
typedef struct wslp_permission_result {
    int32_t err;
    uint32_t changed; /* chmod succeeded, even if verification reports an error */
    wslp_permission_state state;
    char owner[128], group[128]; /* UTF-8, NUL terminated; numeric ids always present */
} wslp_permission_result;

#define WSLP_ID_USER 1u
#define WSLP_ID_GROUP 2u
typedef struct wslp_identity_request {
    uint32_t fields, pad;
    char owner[128], group[128]; /* UTF-8, literal, NUL terminated */
} wslp_identity_request;
typedef struct wslp_identity_result {
    int32_t err;
    uint32_t bad_field, fields, uid, gid, pad;
    char owner[128], group[128]; /* empty for an unmapped numeric ID */
} wslp_identity_result;
typedef struct wslp_ownership_request {
    uint32_t fields, uid, gid, path_bytes;
    wslp_permission_state expected;
} wslp_ownership_request;

typedef struct wslp_resolved { int32_t err;uint32_t distro_len,path_len; } wslp_resolved;

typedef struct wslp_head {
    uint32_t type;
    uint32_t size; /* payload bytes after the header */
    uint32_t id;   /* request id, echoed in answers; watch id for watches */
} wslp_head;

typedef struct wslp_hello {
    uint32_t magic, version;
    uint32_t uid, pid;
} wslp_hello;

#define WSLP_E_DIRLINK 0x1u /* a symlink to a directory */
#define WSLP_E_BROKEN  0x2u /* a symlink to nothing */

typedef struct wslp_entry {
    uint64_t size;
    int64_t  mtime;    /* ns since 1970 UTC */
    int64_t  btime;    /* ns since 1970 UTC, 0 if the filesystem doesn't keep it */
    uint32_t mode;     /* st_mode: type and permission bits */
    uint32_t uid, gid;
    uint16_t name_len; /* bytes after the record */
    uint16_t link_len; /* symlink target bytes after the name */
    uint32_t flags;    /* WSLP_E_* */
} wslp_entry;          /* 48 bytes */

#define WSLP_OWNER_USER  0u
#define WSLP_OWNER_GROUP 1u

typedef struct wslp_owner {
    uint32_t id;
    uint8_t  kind; /* WSLP_OWNER_* */
    uint8_t  len;  /* name bytes after the record, padded to 8 */
    uint16_t pad;
} wslp_owner;

typedef struct wslp_end {
    int32_t  err;   /* errno, 0 = fine */
    uint32_t count; /* entries sent */
} wslp_end;

/* Dedicated --jobs process; listing/watching stays on its own helper.
 * Operation 0..3 and policy 0..3 match fpe.h; extra operations are exact-path.
 * Path records are a header and raw bytes, no NUL. Windows paths are resolved
 * through wslpath by argv, never through a command interpreter. */
#define WSLP_OP_RENAME 4u
#define WSLP_OP_MKDIR 5u
#define WSLP_OP_NEWFILE 6u
#define WSLP_OP_SYMLINK_RELATIVE 7u
#define WSLP_OP_SYMLINK_ABSOLUTE 8u
#define WSLP_OP_COPY_FILE 9u /* selected regular leaf only; never recurse */
#define WSLP_PATH_WINDOWS 1u
#define WSLP_NO_ROOT UINT32_MAX

/* prepare_width is 0 (serial), 2, 4 or 8; only op=copy with a Windows destination. */
typedef struct wslp_job { uint32_t op, policy, count, prepare_width; } wslp_job;
typedef struct wslp_path { uint32_t bytes, flags; } wslp_path;
typedef struct wslp_progress {
    uint64_t bytes_done, bytes_total;
    uint32_t state, items_done, items_total, errors, skipped;
    int32_t error;
    uint32_t root, path_bytes;
} wslp_progress;
typedef struct wslp_event { uint32_t kind; int32_t error; uint32_t root, path_bytes; } wslp_event;

static inline uint32_t wslp_pad8(uint32_t n) { return (n + 7u) & ~7u; }


/* Windows exports keep NTFS I/O on the Windows worker. */
typedef struct wslp_file {
    int64_t atime, mtime; /* nanoseconds since 1970 */
    uint32_t replace, root, path_bytes, pad;
} wslp_file;

#define WSLP_PREPARE_MAX 8u
#define WSLP_PREPARE_BYTES (1u << 20)
typedef struct wslp_prepared_file {wslp_file file;uint64_t token,bytes;} wslp_prepared_file;
typedef struct wslp_file_result {uint64_t token;int32_t error;uint32_t pad;} wslp_file_result;

#define WSLP_GROUP_FLAG 0x100u
typedef struct wslp_group {uint32_t count,pad;wslp_file_result files[WSLP_PREPARE_MAX];} wslp_group;

#define WSLP_TRANSFER_CHUNK 65536u
#define WSLP_TRANSFER_DEPTH 256u
typedef struct wslp_transfer {uint32_t op,policy,path_bytes,pad;} wslp_transfer;
typedef struct wslp_node {
    uint64_t size,device,inode,filesystem;
    int64_t atime,mtime;
    uint32_t mode,uid,gid,name_bytes,link_bytes,metadata_bytes;
} wslp_node;
typedef struct wslp_transfer_end {int32_t err;uint32_t pad;uint64_t bytes;uint8_t sha256[32];} wslp_transfer_end;
typedef struct wslp_transfer_ack {int32_t err;uint32_t skipped,committed,pad;} wslp_transfer_ack;

/* Isolated read-only --search stream: raw relative names, metadata, final status. */
#define WSLP_SEARCH 45u
#define WSLP_SEARCH_ENTRY 46u
#define WSLP_SEARCH_END 47u
typedef struct wslp_search_entry {uint64_t size;int64_t mtime;uint32_t attrs,path_bytes;} wslp_search_entry;
typedef struct wslp_search_end {uint32_t error,scanned,skipped,errors;} wslp_search_end;

/* Optional --index session. OPEN binds one selected root (or an enabled
 * ancestor); QUERY carries the requested absolute scope and a safe ASCII GLOB.
 * STATUS is always a reply, never unsolicited. Query entries use SEARCH_ENTRY,
 * followed by STATUS then SEARCH_END. A non-current final status invalidates
 * every candidate in that reply. Original paths are bytes, not SQLite text. */
#define WSLP_INDEX_OPEN 48u
#define WSLP_INDEX_STATUS 49u
#define WSLP_INDEX_CONTROL 50u
#define WSLP_INDEX_QUERY 51u
#define WSLP_INDEX_ABORT 52u /* stop current query, retain its watched index */
#define WSLP_INDEX_HELLO 53u /* dedicated capability greeting; legacy modes keep HELLO */
typedef struct wslp_index_open {uint32_t action,path_bytes;} wslp_index_open;
typedef struct wslp_index_query {uint32_t path_bytes,glob_bytes;} wslp_index_query;
typedef struct wslp_index_status {
    uint32_t state,error,uid,enabled;
    uint64_t entries,scanned,errors,skipped,updated;
    uint32_t root_bytes,pad;
} wslp_index_status;

#endif
