#ifndef OPENPILOT_PIPEWRITER_H
#define OPENPILOT_PIPEWRITER_H
#include <windows.h>
#include <stdint.h>
#include <stddef.h>

typedef struct pipe_writer pipe_writer;

/* Owns a duplicate of pipe. Writes and backpressure never run on the UI
 * thread. Failed/cancelled writers do not publish queued requests. */
pipe_writer *pipe_writer_begin(HANDLE pipe, size_t queue_limit, HWND window, UINT failed_message,
                               uint32_t generation);
int pipe_writer_send(pipe_writer *writer, const void *bytes, size_t size, uint32_t tag, uint32_t drop_mask);
void pipe_writer_cancel(pipe_writer **slot);

#endif
