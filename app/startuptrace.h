/* Optional, process-spanning startup diagnostics. No file is opened unless
 * OPENPILOT_PANEL_STARTUP_LOG names an output file in the launch environment. */
#pragma once
#include <windows.h>
#include <stdio.h>

static void panel_startup_trace(const char *stage)
{
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    wchar_t path[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"OPENPILOT_PANEL_STARTUP_LOG", path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    QueryPerformanceFrequency(&frequency);
    char line[256];
    int size = snprintf(line, sizeof line, "%lu,%lu,%lld,%lld,%s\r\n", GetCurrentProcessId(),
                        GetCurrentThreadId(), (long long)now.QuadPart, (long long)frequency.QuadPart, stage);
    if (size <= 0 || size >= (int)sizeof line) return;
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(file, line, (DWORD)size, &written, NULL);
    CloseHandle(file);
}
