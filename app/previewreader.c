#define WIN32_LEAN_AND_MEAN
#include "previewreader.h"
#include <stdlib.h>

static void release(preview_reader *r)
{
    if (InterlockedDecrement(&r->refs)) return;
    if (r->thread) CloseHandle(r->thread);
    if (r->pipe) CloseHandle(r->pipe);
    free(r);
}

preview_reader *preview_reader_begin(HANDLE pipe, HWND window, uint32_t generation,
                                     LPTHREAD_START_ROUTINE worker)
{
    preview_reader *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->refs = 1;
    r->window = window; r->generation = generation;
    if (!DuplicateHandle(GetCurrentProcess(), pipe, GetCurrentProcess(), &r->pipe, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        release(r);
        return NULL;
    }
    InterlockedIncrement(&r->refs);
    r->thread = CreateThread(NULL, 0, worker, r, 0, NULL);
    if (!r->thread) { release(r); release(r); return NULL; }
    return r;
}

void preview_reader_cancel(preview_reader **slot)
{
    preview_reader *r = *slot;
    *slot = NULL;
    if (!r) return;
    InterlockedExchange(&r->cancelled, 1);
    CancelIoEx(r->pipe, NULL);
    CancelSynchronousIo(r->thread);
    release(r);
}

void preview_reader_finish(preview_reader *r)
{
    release(r);
}
