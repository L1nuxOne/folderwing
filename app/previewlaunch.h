#ifndef OPENPILOT_PREVIEWLAUNCH_H
#define OPENPILOT_PREVIEWLAUNCH_H

#include <windows.h>
#include <stdint.h>

typedef struct preview_launch preview_launch;

/* The worker duplicates all three inherited handles. The caller keeps its
 * originals and can close them as soon as begin returns. */
preview_launch *preview_launch_begin(const wchar_t *exe_name, HANDLE child_in, HANDLE child_out,
                                     HANDLE section, SIZE_T memory_limit, HWND window, UINT message,
                                     uint32_t generation);
/* 0: pending, 1: adopted a live child, -1: launch failed. Never waits. */
int preview_launch_take(preview_launch **slot, HANDLE *process, HANDLE *job, DWORD *pid);
/* Detach promptly. The worker owns and reaps a late/suspended child. */
void preview_launch_cancel(preview_launch **slot);

#endif
