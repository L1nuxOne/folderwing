#ifndef OPENPILOT_PREVIEWREADER_H
#define OPENPILOT_PREVIEWREADER_H
#include <windows.h>
#include <stdint.h>

typedef struct preview_reader {
    LONG refs, cancelled;
    HANDLE pipe, thread;
    HWND window;
    uint32_t generation;
} preview_reader;

preview_reader *preview_reader_begin(HANDLE pipe, HWND window, uint32_t generation,
                                     LPTHREAD_START_ROUTINE worker);
void preview_reader_cancel(preview_reader **slot);
void preview_reader_finish(preview_reader *reader);

#endif
