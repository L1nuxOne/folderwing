/* everything.h - asks a running Everything (voidtools) for search results.
 *
 * OpenPilot keeps no index of its own: whole-disk search is Everything's
 * job. This is its documented IPC (v1): a WM_COPYDATA query to Everything's
 * window, answered later by a WM_COPYDATA to a window of ours. Nothing is
 * loaded and nothing runs in the background; when Everything is not running
 * the query just fails.
 *
 *   ev_query(reply_hwnd, id, L"report pdf", 100);
 *   ...
 *   case WM_COPYDATA:  (in reply_hwnd's window procedure)
 *       if (cds->dwData == id) list = ev_parse(cds->lpData, cds->cbData);
 *
 * The data is only valid during WM_COPYDATA, so ev_parse copies it. Give
 * each query a new id and drop replies to older ones. The reply can arrive
 * before ev_query returns (Everything answers while the query is being
 * sent), so be ready for it before calling. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ev_item {
    wchar_t *path;   /* full path */
    uint32_t name;   /* where the name starts in path */
    uint8_t  folder; /* a folder or a drive */
} ev_item;

typedef struct ev_list {
    uint32_t total; /* matches in Everything, of which n are here */
    uint32_t n;
    ev_item  items[]; /* one allocation with the strings */
} ev_list;

/* Everything's IPC window, or NULL when it is not running. */
HWND     ev_window(void);
void     ev_set_instance(const wchar_t *instance); /* NULL/empty: normal instance, then 1.5a */
/* 0: sent; -1: Everything is not running; -2: it did not take the query
 * (busy, or running elevated while we are not). */
int      ev_query(HWND reply, uint32_t id, const wchar_t *search, uint32_t max_results);
/* NULL when the reply is malformed. Free with free(). */
ev_list *ev_parse(const void *data, size_t len);
/* Exact folder lookup used to choose a backend. Returns 0 for paths whose
 * characters cannot be represented safely in Everything's query language. */
int ev_folder_query(const wchar_t *folder,wchar_t *out,size_t cap);
