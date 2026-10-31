/* Personal reading marks. Mutations reload under an exclusive file lock and
 * atomically replace the store; callers never overwrite another window's list. */
#pragma once
#include <windows.h>
#include <stdint.h>
#define DOCMARK_TITLE 160
#define DOCMARK_PER_FILE 512
#define DOCMARK_STORE_MAX 4096
typedef struct docmark { uint64_t id; uint32_t page; float y; wchar_t title[DOCMARK_TITLE]; } docmark;
DWORD docmarks_load(const wchar_t *store,const wchar_t *document,docmark *out,uint32_t capacity,uint32_t *count);
/* add: id==0 assigns a new ID. remove: delete only this document's given ID.
 * A missing store is empty; a corrupt/unreadable store is never overwritten. */
DWORD docmarks_change(const wchar_t *store,const wchar_t *document,docmark *mark,int remove);

/* Caller owns the path. NULL reference selects local app data. */
wchar_t *docmarks_location(const wchar_t *reference,const wchar_t *leaf);
wchar_t *docmarks_identity(const wchar_t *document,const wchar_t *scope);
