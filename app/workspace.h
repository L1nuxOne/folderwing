/* Named, explicitly saved folder/layout snapshots. Session path encoding is
 * reused; pins, recents and window placement do not belong to a workspace. */
#pragma once
#include "session.h"
#define WS_MAX 32u
#define WS_NAME 81u
#define WS_SNAPSHOT_MAX (1024u*1024u)
#define WS_FILE_MAX (16u*1024u*1024u)
typedef struct workspace {wchar_t name[WS_NAME];session snapshot;} workspace;
typedef struct workspace_catalog {workspace items[WS_MAX];uint32_t count;uint64_t revision;} workspace_catalog;
enum { WS_LOAD,WS_CREATE,WS_UPDATE,WS_RENAME,WS_DELETE };
void workspace_free(workspace_catalog *c);
int workspace_name(const wchar_t *input,wchar_t out[WS_NAME]);
int workspace_snapshot_valid(const session *s);
DWORD workspace_clone(const session *s,session *out);
int workspace_find(const workspace_catalog *c,const wchar_t *name);
/* Strict, checksummed versioned envelope. On error existing c is retained. */
DWORD workspace_parse(const uint8_t *data,size_t bytes,workspace_catalog *c);
DWORD workspace_format(const workspace_catalog *c,uint8_t **data,size_t *bytes);
/* Worker-only I/O. A mutation checks the loaded revision under a per-file
 * lock and atomically replaces the catalog. Conflict never overwrites edits. */
DWORD workspace_load(const wchar_t *file,workspace_catalog *out,HANDLE cancel);
DWORD workspace_change(const wchar_t *file,uint64_t expected,uint32_t action,const wchar_t *name,const wchar_t *new_name,const session *snapshot,workspace_catalog *out,HANDLE cancel);
