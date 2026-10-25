#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "spacewire.h"
typedef struct sp_totals {uint64_t logical,allocated,files,folders,skipped,errors,unknown,hardlinks;} sp_totals;
typedef struct sp_node {
 wchar_t *name;uint32_t parent,first,last,next,flags,error,end;sp_totals sum;
} sp_node;
typedef struct sp_stamp {uint64_t device,identity;} sp_stamp;
typedef struct sp_names {struct sp_names *next;size_t used,capacity;volatile LONG refs;wchar_t text[];} sp_names;
typedef struct sp_identity {uint64_t device,id;uint32_t owner;} sp_identity;
typedef struct sp_model {
 sp_node *nodes;uint32_t count,capacity,state,error;uint64_t name_bytes,revision;
 sp_identity *identities;uint32_t identity_capacity,identity_count;sp_names *names;uint32_t backend,phase,backend_error,backend_pid;uint64_t checked,generation;uint32_t lazy_free,lazy_slots;sp_stamp *stamps;uint64_t root_device,root_identity;
} sp_model;
void space_model_free(sp_model *m);
int space_model_record(sp_model *m,const sp_record *record,const wchar_t *name);
wchar_t *space_model_path(const sp_model *m,uint32_t id,const wchar_t *root);
int space_model_descendant(const sp_model *m,uint32_t id,uint32_t root);
typedef struct sp_row {uint32_t id,depth;uint64_t size,logical,allocated,files;wchar_t type[48];} sp_row;
/* view 0 tree, 1 files, 2 extension groups, 3 selected extension files.
 * sort 0 size descending, 1 size ascending, 2 name. Caller owns rows. */
sp_row *space_model_rows(const sp_model *m,uint32_t root,int view,int allocated,int sort,
 const wchar_t *filter,const wchar_t *extension,const unsigned char *expanded,uint32_t *count);
/* Flat-query snapshot: begin/capture require the caller's model lock. Capture
 * copies at most 256 nodes, with no allocation/filtering/sorting under that lock.
 * Work/finish/free need no live model; a name-slab lease survives reset/free.
 * ERROR_RETRY means the model changed during capture; discard and reschedule.
 * A successful finish transfers rows to the caller. Always free the query. */
typedef struct sp_query sp_query;
sp_query *space_query_begin(const sp_model *,uint32_t root,int view,int allocated,int sort,const wchar_t *filter,const wchar_t *extension);
DWORD space_query_capture(sp_query *,const sp_model *,int *done);
DWORD space_query_work(sp_query *,HANDLE cancel);
sp_row *space_query_finish(sp_query *,HANDLE cancel,uint32_t *count,DWORD *error);
uint64_t space_query_revision(const sp_query *);
void space_query_free(sp_query *);
/* Caller-owned worker; no UI calls. cb zero or cancel stops this exact scan. */
typedef int (*space_emit)(void *,const sp_record *,const wchar_t *);
DWORD space_scan(const wchar_t *root,int wsl_enabled,int as_root,HANDLE cancel,space_emit emit,void *user);
DWORD space_scan_mode(const wchar_t *root,int wsl_enabled,int as_root,HANDLE cancel,space_emit emit,void *user,int mode);
DWORD space_scan_page(const wchar_t *root,int wsl_enabled,int as_root,HANDLE cancel,space_emit emit,void *user,const sp_page_request *page);
/* mode 1: folder summaries; 2: direct listing; 3: explicit full-file analysis.
 * Only completed summary models accept disposable file rows; no total changes. */
uint32_t space_model_list_add(sp_model *m,uint32_t parent,const sp_node *file);
void space_model_list_clear(sp_model *m,uint32_t parent);

/* NTFS is opt-in by an explicit whole fixed-volume root. The ordinary scanner
 * remains available to folder tools and duplicate-finder enumeration; the main
 * Space view requests Linux folder summaries explicitly. */
int space_ntfs_root(const wchar_t *root);
DWORD space_ntfs_scan(const wchar_t *root,HANDLE cancel,HANDLE visible,space_emit emit,void *user);

/* Bounded NTFS delivery. Items/names are borrowed only for the callback; status,
 * reset and completion boundaries are delivered immediately. A quiet pipe never
 * retains completed records while waiting for the next frame. */
#define SP_NTFS_BATCH 256u
typedef struct sp_batch_item {sp_record record;wchar_t name[256];} sp_batch_item;
typedef int (*space_emit_batch)(void *,const sp_batch_item *,uint32_t);
DWORD space_ntfs_scan_batched(const wchar_t *root,HANDLE cancel,HANDLE visible,space_emit_batch emit,void *user);
