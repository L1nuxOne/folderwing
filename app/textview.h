#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
enum {
    TEXTVIEW_WRAP,
    TEXTVIEW_LINES,
    TEXTVIEW_FIND,
    TEXTVIEW_NEXT,
    TEXTVIEW_PREV,
    TEXTVIEW_SELECT_ALL,
    TEXTVIEW_COPY,
    TEXTVIEW_ENCODING,
    TEXTVIEW_OPEN
};
int textview_available(void);
int textview_start(HWND parent);
void textview_stop(void);
HWND textview_window(void);
void textview_open(const wchar_t *path, uint64_t bytes, int64_t mtime);
void textview_clear(void);
void textview_configure(uint32_t limit_mb, int wrap, int lines, uint32_t encoding);
void textview_action(int action);
int textview_key_scope(void); /* reader has focus; menu/palette invocation can use actions regardless */
int textview_selected(void);

/* Select a normalized UTF-16 match after this exact document snapshot loads. */
int textview_goto_match(const wchar_t *path,uint32_t line,uint32_t column,const wchar_t *expected);
