/* app.h - what functions may call in the app.
 *
 * Functions (functions.c, and any new fn_*.c) work through this interface
 * and the fn_ctx they are given; panes are opaque handles and the window's
 * internals stay private to fpdemo.c. */
#pragma once
#include "fn.h"
#include "fpe.h"
#include "session.h"

typedef struct Pane Pane;

HWND           app_window(void);
/* Focus a child only while OpenPilot owns the foreground; preserve other apps. */
void           app_focus_window(HWND window);
Pane          *app_left_pane(void);
Pane          *app_other_pane(Pane *p);
const wchar_t *app_pane_path(Pane *p);
void           app_status(Pane *p, int err, const wchar_t *fmt, ...);

/* navigation and view */
void app_navigate(Pane *p, const wchar_t *path);
void app_go_back(Pane *p);
void app_go_forward(Pane *p);
void app_go_up(Pane *p);
void app_refresh(Pane *p);
void app_focus_address(Pane *p);
void app_open_cursor(Pane *p);
void app_set_sort(Pane *p, fpe_sort_key k);
void app_select_all(Pane *p);

/* files */
void app_rename_cursor(Pane *p);
void app_rename_path(const wchar_t *path); /* shows it in the active pane and renames there */
void app_new_item(Pane *p, int file);
/* A file named name ("New note.md", counted up when taken) in the folder
 * shown, a copy of tmpl (NULL: empty), then renamed; owner runs till then. */
void app_new_file(Pane *p, const wchar_t *name, const wchar_t *tmpl, const fn_def *owner);
int app_stage_tree_copy(const wchar_t *root,const wchar_t *const *relative,uint32_t n,const wchar_t *dest);
int app_stage_copy(const wchar_t *const *paths,uint32_t n,const wchar_t *dest);
void app_transfer(Pane *p, int move);      /* to the other pane's folder, after asking */
void app_clip_set(Pane *p, wchar_t *const *paths, uint32_t n, int cut);
void app_paste_into(const wchar_t *dest);
void app_delete(wchar_t *const *paths, uint32_t n, int permanent);

/* The terminal in the side panel: a new session in dir running cmd (NULL: the shell), title (NULL: the program's).
 * 0 = asked; -1 = no terminal (a status says why). */
int  app_terminal_run(const wchar_t *dir, const wchar_t *cmd, const wchar_t *title);

/* tabs: a pane always has at least one */
int  app_tab_count(Pane *p);
int  app_tab_closed_count(void);
void app_tab_new(Pane *p, const wchar_t *path, int show);
void app_tab_close(Pane *p); /* the shown one */
void app_tab_step(Pane *p, int dir);
void app_tab_reopen(Pane *p);

/* folders pinned to the top of the tree */
int  app_is_pinned(const wchar_t *path);
void app_pin(const wchar_t *path);
void app_unpin(const wchar_t *path);

/* window */
void app_toggle_tree(void);
void app_toggle_dual(void);
void app_toggle_hidden(void);
void app_focus_cycle(int dir);
void app_preview_close(void); /* until the cursor moves to another PDF */
/* UI thread only. Begin supersedes an older request; submit consumes both
 * movable handles, even on failure. Retries never block the message loop. */
uint64_t app_clipboard_begin(HWND owner);
void app_clipboard_submit(uint64_t request, UINT format, HGLOBAL data,
                          UINT extra_format, HGLOBAL extra, const wchar_t *success);
void app_clipboard_cancel(HWND owner); /* NULL cancels everything at shutdown */

/* for overlays (the palette) */
Pane *app_active_pane(void);
HFONT app_font(void);
int   app_px(int v);   /* v pixels at 96 dpi, scaled to the window's dpi */
int   app_row_h(void);
/* The pane's selection (or cursor row), as a key press would target it. */
void  app_pane_ctx(fn_ctx *c, Pane *p, int from);
void  app_ctx_free(fn_ctx *c);
int   app_menu_context_current(const fn_ctx *c);
/* The icon a row would show for path, in a 16-px (scaled) slot of height h.
 * place: APP_PLACE_* for places, else -1. */
void  app_draw_icon(HDC dc, const wchar_t *path, int folder, int place, int x, int y, int h);
/* Shows the item's folder with the cursor on the item. */
void  app_reveal(Pane *p, const wchar_t *path);

/* Folders worth jumping to, in this order: recently shown (newest first),
 * open tabs, pinned, then drives and known folders. label may be NULL. */
enum { APP_PLACE_RECENT, APP_PLACE_TAB, APP_PLACE_PIN, APP_PLACE_ROOT };
typedef void (*app_place_fn)(void *user, const wchar_t *path, const wchar_t *label, int kind);
void  app_places(app_place_fn fn, void *user);

/* debug mode */
int  app_debug_shown(void);
void app_debug_show(int on);
void app_debug_tick(void);
void app_debug_reset(void);
void app_debug_save(void);

/* the built-in functions (functions.c) */
void functions_register(void);
/* Image folder browsing: frozen pane order, decoded-file result on the UI thread. */
void app_image_step(int direction);
void app_image_result(int supported);
UINT app_menu_track(HMENU menu,HWND owner,POINT point); /* excludes user wait from dispatch timing */
void app_image_button(int direction);
void app_image_page_button(int direction); /* normal configurable function dispatch */

/* Archive content is a third preview pane; paths here are owned immutable temporary copies. */
int app_archive_preview_kind(const wchar_t *name);
void app_archive_preview(const wchar_t *name,const wchar_t *path,uint64_t bytes);
void app_archive_close(void);
/* On success, retain owner until this Operations copy reaches a terminal state. */
int app_stage_archive_copy(const wchar_t *source,const wchar_t *dest,void (*release)(void *),void *owner);

/* Reader commands use archive scope when displaying an archive entry. */
void app_preview_ctx(fn_ctx *context,HWND reader,int from);
/* UI-only workspace capture/apply. Capture owns its strings; apply clones
 * before changing any pane. File access belongs to the workspace worker. */
const wchar_t *app_workspace_session(void);
DWORD app_workspace_capture(session *snapshot);
DWORD app_workspace_apply(const session *snapshot);
