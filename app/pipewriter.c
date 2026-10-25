#define WIN32_LEAN_AND_MEAN
#include "pipewriter.h"
#include <stdlib.h>
#include <string.h>

typedef struct queued_write {
    struct queued_write *next;
    size_t size;
    uint32_t tag;
    uint8_t bytes[];
} queued_write;

struct pipe_writer {
    LONG refs, cancelled, failed;
    CRITICAL_SECTION lock;
    HANDLE pipe, wake, thread;
    HWND window;
    UINT failed_message;
    uint32_t generation;
    size_t queued_bytes, queue_limit;
    queued_write *first, *last;
};

static void release(pipe_writer *w)
{
    if (InterlockedDecrement(&w->refs)) return;
    while (w->first) {
        queued_write *m = w->first;
        w->first = m->next;
        free(m);
    }
    if (w->thread) CloseHandle(w->thread);
    if (w->pipe) CloseHandle(w->pipe);
    if (w->wake) CloseHandle(w->wake);
    DeleteCriticalSection(&w->lock);
    free(w);
}

static DWORD WINAPI write_worker(void *arg)
{
    pipe_writer *w = arg;
    for (;;) {
        if (InterlockedCompareExchange(&w->cancelled, 0, 0)) break;
        EnterCriticalSection(&w->lock);
        queued_write *m = w->first;
        if (m) {
            w->first = m->next;
            if (!w->first) w->last = NULL;
            w->queued_bytes -= m->size;
        }
        LeaveCriticalSection(&w->lock);
        if (!m) { WaitForSingleObject(w->wake, INFINITE); continue; }
        size_t off = 0;
        while (off < m->size && !InterlockedCompareExchange(&w->cancelled, 0, 0)) {
            DWORD wrote = 0;
            if (!WriteFile(w->pipe, m->bytes + off, (DWORD)(m->size - off), &wrote, NULL) || !wrote) break;
            off += wrote;
        }
        size_t wanted = m->size;
        free(m);
        if (InterlockedCompareExchange(&w->cancelled, 0, 0)) break;
        if (off < wanted) {
            InterlockedExchange(&w->failed, 1);
            PostMessageW(w->window, w->failed_message, w->generation, 0);
            break;
        }
    }
    release(w);
    return 0;
}

pipe_writer *pipe_writer_begin(HANDLE pipe, size_t queue_limit, HWND window, UINT failed_message,
                               uint32_t generation)
{
    pipe_writer *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->refs = 1;
    InitializeCriticalSection(&w->lock);
    w->window = window; w->failed_message = failed_message; w->generation = generation;
    w->queue_limit = queue_limit;
    if (!DuplicateHandle(GetCurrentProcess(), pipe, GetCurrentProcess(), &w->pipe, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
        !(w->wake = CreateEventW(NULL, FALSE, FALSE, NULL))) { release(w); return NULL; }
    InterlockedIncrement(&w->refs);
    w->thread = CreateThread(NULL, 0, write_worker, w, 0, NULL);
    if (!w->thread) { release(w); release(w); return NULL; }
    return w;
}

int pipe_writer_send(pipe_writer *w, const void *bytes, size_t size, uint32_t tag, uint32_t drop_mask)
{
    if (!w || !bytes || !size || size > w->queue_limit || size > UINT32_MAX) return -1;
    queued_write *m = malloc(sizeof *m + size);
    if (!m) return -1;
    m->next = NULL; m->size = size; m->tag = tag;
    memcpy(m->bytes, bytes, size);
    EnterCriticalSection(&w->lock);
    if (InterlockedCompareExchange(&w->cancelled, 0, 0) || InterlockedCompareExchange(&w->failed, 0, 0)) {
        LeaveCriticalSection(&w->lock); free(m); return -1;
    }
    queued_write **at = &w->first;
    w->last = NULL;
    while (*at) {
        queued_write *old = *at;
        if (old->tag & drop_mask) {
            *at = old->next;
            w->queued_bytes -= old->size;
            free(old);
        } else { w->last = old; at = &old->next; }
    }
    if (w->queued_bytes > w->queue_limit - size) {
        LeaveCriticalSection(&w->lock); free(m); return -1;
    }
    if (w->last) w->last->next = m;
    else w->first = m;
    w->last = m;
    w->queued_bytes += size;
    LeaveCriticalSection(&w->lock);
    SetEvent(w->wake);
    return 0;
}

void pipe_writer_cancel(pipe_writer **slot)
{
    pipe_writer *w = *slot;
    *slot = NULL;
    if (!w) return;
    InterlockedExchange(&w->cancelled, 1);
    SetEvent(w->wake);
    CancelIoEx(w->pipe, NULL);
    CancelSynchronousIo(w->thread);
    release(w);
}
