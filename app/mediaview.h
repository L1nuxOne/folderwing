#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "mediaproto.h"
int mediaview_available(void);
int mediaview_start(HWND parent);
void mediaview_stop(void);
HWND mediaview_window(void);
void mediaview_open(const wchar_t *path,uint64_t size,int64_t mtime);
void mediaview_clear(void);
void mediaview_suspend(void);
void mediaview_configure(unsigned volume,int muted);
void mediaview_control(uint32_t action,double value);
int mediaview_state(mediap_state *state); /* snapshot only; no I/O */
DWORD mediaview_helper_pid(void);
