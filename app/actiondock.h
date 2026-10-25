/* One native button dock, shared by all placements. No helper/process work. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
enum {AD_LEFT,AD_TREE_BOTTOM,AD_TOP,AD_HIDDEN};
enum {AD_FIND,AD_CONTENT,AD_COMPARE,AD_SIZE,AD_DUPLICATES,AD_GIT,AD_WORKSPACES,AD_TERMINAL,AD_OPERATIONS,AD_DIRECT,AD_MORE,AD_COUNT};
#define AD_ALL ((1u<<AD_MORE)-1u)
typedef struct ad_geometry {RECT body,tree,dock,buttons[AD_COUNT];int effective,tree_width;uint32_t visible;} ad_geometry;
typedef struct ad_state {int enabled,selected;wchar_t tooltip[320];} ad_state;
typedef struct ad_host {void (*query)(int,ad_state*);void (*begin)(void);void (*end)(void);void (*invoke)(int,POINT);void (*leave)(int);void (*focus)(HWND);int (*key)(UINT,int,LPARAM);void (*enter)(HWND);} ad_host;
extern const char *const ad_names[AD_MORE];
extern const wchar_t *const ad_labels[AD_COUNT];
void actiondock_geometry(int w,int h,int tree,int tree_width,UINT dpi,int position,int labels,uint32_t mask,ad_geometry *out);
HWND actiondock_create(HWND parent,const ad_host *host);
void actiondock_destroy(void);
HWND actiondock_window(void);
HWND actiondock_button(int id);
int actiondock_contains(HWND window);
HWND actiondock_focus_target(void);
void actiondock_place(const ad_geometry *geometry,UINT dpi,int labels,HFONT font);
void actiondock_refresh(void);
void actiondock_popup(void);
