#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
enum {
    IMAGE_FIT,
    IMAGE_WIDTH,
    IMAGE_ACTUAL,
    IMAGE_ZOOM_IN,
    IMAGE_ZOOM_OUT,
    IMAGE_BACKGROUND,
    IMAGE_ROTATE,
    IMAGE_FLIP,
    IMAGE_METADATA,
    IMAGE_COPY,
    IMAGE_PLAY,
    IMAGE_PREVIOUS,
    IMAGE_NEXT,
    IMAGE_OPEN,
    IMAGE_PREVIOUS_PAGE,
    IMAGE_NEXT_PAGE
};
int imageview_available(void);
int imageview_animated(void);
int imageview_page_can(int direction);
void imageview_controls_refresh(void);
int imageview_start(HWND parent);
void imageview_stop(void);
HWND imageview_window(void);
void imageview_open(const wchar_t *path, uint64_t size, int64_t mtime);
void imageview_clear(void);
void imageview_configure(uint32_t cache_mb, uint32_t background);
void imageview_action(int action);
/* Folder navigation is owned by the source pane; animation steps stay separate. */
void imageview_open_browse(const wchar_t *path,uint64_t size,int64_t mtime);
const wchar_t *imageview_path(void);
int imageview_matches(uint64_t size,int64_t mtime);
void imageview_browse(uint32_t position,uint32_t count,int previous,int next,int state);
void imageview_report_result(void);
