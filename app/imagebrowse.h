#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "fpe.h"
#define IMAGEBROWSE_MAX_ROWS 1000000u
typedef struct imagebrowse_worker imagebrowse_worker;
typedef struct imagebrowse_list {
    fpe_dir *dir;
    uint64_t sequence,revision,max_bytes;
    uint32_t count;
    uint32_t rows[]; /* visible row numbers, sorted in the pane's exact order */
} imagebrowse_list;
int imagebrowse_candidate(const fpe_dir *dir,const fpe_entry *entry,const wchar_t *name);
imagebrowse_worker *imagebrowse_start(HWND target,UINT message);
DWORD imagebrowse_submit(imagebrowse_worker *worker,const fpe_view *view,uint64_t sequence,uint64_t max_bytes);
imagebrowse_list *imagebrowse_take(imagebrowse_worker *worker);
void imagebrowse_cancel(imagebrowse_worker *worker,uint64_t sequence);
void imagebrowse_stop(imagebrowse_worker *worker);
void imagebrowse_free(imagebrowse_list *list);
/* index is 0-based, UINT32_MAX if absent; neighbors never wrap. */
uint32_t imagebrowse_index(const imagebrowse_list *list,uint32_t row);
uint32_t imagebrowse_neighbor(const imagebrowse_list *list,uint32_t row,int direction);
int imagebrowse_remove(imagebrowse_list *list,uint32_t row);
