#pragma once
#include <windows.h>
#include "archiveclient.h"
#include "fn.h"
int archiveview_available(void);
int archiveview_start(HWND parent);
void archiveview_stop(void);
HWND archiveview_window(void);
void archiveview_open(const wchar_t *path,uint64_t size,int64_t mtime);
void archiveview_clear(void);
void archiveview_tick(void);
int archiveview_scope(const fn_ctx *context);
int archiveview_can_open(const fn_ctx *context);
int archiveview_can_extract(const fn_ctx *context);
int archiveview_can_up(const fn_ctx *context);
void archiveview_preview(fn_ctx *context);
void archiveview_extract(fn_ctx *context);
void archiveview_up(fn_ctx *context);
void archiveview_copy_name(fn_ctx *context);
void archiveview_copy_path(fn_ctx *context);
void archiveview_close(fn_ctx *context);
