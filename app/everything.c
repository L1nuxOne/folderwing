/* Everything IPC client: see everything.h. Layouts are those of the
 * Everything SDK's everything_ipc.h (IPC version 1, UTF-16). */
#include "everything.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define EV_COPYDATA_QUERYW 2
#define EV_ITEM_FOLDER     0x1u
#define EV_ITEM_DRIVE      0x2u

#pragma pack(push, 1)
typedef struct ev_query_w {
    DWORD reply_hwnd; /* window handles fit in 32 bits, also on x64 */
    DWORD reply_copydata_message;
    DWORD search_flags;
    DWORD offset;
    DWORD max_results;
    /* WCHAR search_string[], null-terminated */
} ev_query_w;

typedef struct ev_item_w {
    DWORD flags;
    DWORD filename_offset; /* from the start of the list */
    DWORD path_offset;
} ev_item_w;

typedef struct ev_list_w {
    DWORD totfolders, totfiles, totitems;
    DWORD numfolders, numfiles, numitems;
    DWORD offset;
    /* ev_item_w items[numitems] */
} ev_list_w;
#pragma pack(pop)

static SRWLOCK instance_lock=SRWLOCK_INIT;
static wchar_t instance_name[64];
void ev_set_instance(const wchar_t *instance)
{
    AcquireSRWLockExclusive(&instance_lock);_snwprintf(instance_name,64,L"%ls",instance?instance:L"");instance_name[63]=0;ReleaseSRWLockExclusive(&instance_lock);
}
HWND ev_window(void)
{
    wchar_t instance[64],cls[128];AcquireSRWLockShared(&instance_lock);wcscpy(instance,instance_name);ReleaseSRWLockShared(&instance_lock);
    if(instance[0]){swprintf(cls,128,L"EVERYTHING_TASKBAR_NOTIFICATION_(%ls)",instance);return FindWindowW(cls,NULL);}
    HWND h = FindWindowW(L"EVERYTHING_TASKBAR_NOTIFICATION", NULL);
    /* Everything 1.5 alpha runs as the instance "1.5a" by default */
    if (!h) h = FindWindowW(L"EVERYTHING_TASKBAR_NOTIFICATION_(1.5a)", NULL);
    return h;
}

int ev_folder_query(const wchar_t *folder,wchar_t *out,size_t cap)
{
    if(!folder||!*folder||wcspbrk(folder,L"\"*?<>|\r\n"))return 0;
    size_t n=wcslen(folder);while(n>3&&folder[n-1]==L'\\')n--;
    if(n+32>=cap)return 0;
    _snwprintf(out,cap,L"folder:path:wfn:\"%.*ls\"",(int)n,folder);return 1;
}

int ev_query(HWND reply, uint32_t id, const wchar_t *search, uint32_t max_results)
{
    HWND ev = ev_window();
    if (!ev) return -1;
    size_t chars = wcslen(search) + 1;
    size_t size = sizeof(ev_query_w) + chars * sizeof(wchar_t);
    ev_query_w *q = malloc(size);
    if (!q) return -2;
    q->reply_hwnd = (DWORD)(uintptr_t)reply;
    q->reply_copydata_message = id;
    q->search_flags = 0;
    q->offset = 0;
    q->max_results = max_results;
    memcpy(q + 1, search, chars * sizeof(wchar_t));
    COPYDATASTRUCT cds = { EV_COPYDATA_QUERYW, (DWORD)size, q };
    DWORD_PTR result = 0;
    /* Not SMTO_BLOCK: Everything may send its answer before this returns,
     * and this thread has to take it while it waits. */
    LRESULT ok = SendMessageTimeoutW(ev, WM_COPYDATA, (WPARAM)reply, (LPARAM)&cds, SMTO_ABORTIFHUNG, 1000, &result);
    free(q);
    return ok && result ? 0 : -2;
}

/* A null-terminated string at `off` inside the reply: its length, or -1. */
static long str_at(const uint8_t *base, size_t len, DWORD off)
{
    if (off < sizeof(ev_list_w) || off >= len || (off & 1)) return -1;
    const wchar_t *s = (const wchar_t *)(base + off);
    size_t max = (len - off) / sizeof(wchar_t);
    size_t n = wcsnlen(s, max);
    return n < max ? (long)n : -1;
}

ev_list *ev_parse(const void *data, size_t len)
{
    const uint8_t *base = data;
    if (!data || len < sizeof(ev_list_w)) return NULL;
    ev_list_w head;
    memcpy(&head, base, sizeof head);
    if (head.numitems > (len - sizeof head) / sizeof(ev_item_w)) return NULL;
    const ev_item_w *items = (const ev_item_w *)(base + sizeof head);

    size_t chars = 0;
    for (DWORD i = 0; i < head.numitems; i++) {
        long nl = str_at(base, len, items[i].filename_offset), pl = str_at(base, len, items[i].path_offset);
        if (nl < 0 || pl < 0) return NULL;
        chars += (size_t)nl + (size_t)pl + 3;
    }
    ev_list *out = malloc(sizeof *out + head.numitems * sizeof(ev_item) + chars * sizeof(wchar_t));
    if (!out) return NULL;
    out->total = head.totitems;
    out->n = 0;
    wchar_t *w = (wchar_t *)&out->items[head.numitems];
    for (DWORD i = 0; i < head.numitems; i++) {
        const wchar_t *name = (const wchar_t *)(base + items[i].filename_offset);
        const wchar_t *path = (const wchar_t *)(base + items[i].path_offset);
        size_t nl = wcslen(name), pl = wcslen(path);
        if (!nl) continue;
        ev_item *it = &out->items[out->n++];
        it->path = w;
        it->folder = (items[i].flags & (EV_ITEM_FOLDER | EV_ITEM_DRIVE)) != 0;
        if (pl) {
            memcpy(w, path, pl * sizeof(wchar_t));
            w += pl;
            if (path[pl - 1] != L'\\') *w++ = L'\\';
        }
        it->name = (uint32_t)(w - it->path);
        memcpy(w, name, nl * sizeof(wchar_t));
        w += nl;
        if (!pl && (items[i].flags & EV_ITEM_DRIVE) && name[nl - 1] != L'\\') *w++ = L'\\'; /* "C:" -> "C:\" */
        *w++ = 0;
    }
    return out;
}
