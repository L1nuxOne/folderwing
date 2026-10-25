/* Shell integration for the demo: system icons, context menus, drag and
 * drop, properties and small shell actions.
 *
 * Nothing here touches the shell before shell_init() or shell_icons_start();
 * the app starts them right after its first frame so startup never pays for
 * COM, OLE or the system image list. Everything runs on the UI thread except
 * the icon worker. */
#pragma once
#include <windows.h>
#include <stdint.h>

/* OLE on the calling (UI) thread: menus, drag and drop, properties. */
int  shell_init(void);
void shell_shutdown(void);
int  shell_ready(void);
/* The icon worker: builds the system image list and resolves icons, posting
 * `icons_msg` to `notify` when some have arrived. Until it runs (or when it
 * is never started) every icon lookup returns -1. */
int  shell_icons_start(HWND notify, UINT icons_msg);
void shell_icons_stop(void);

/* ---- icons -------------------------------------------------------------------- */
/* Index into the small system image list, or -1 when the caller should draw
 * its own fallback. Never blocks on file I/O: type icons come from the
 * registry by extension, and files that carry their own icon (.exe, .lnk,
 * .ico, ...) are read on the worker; until then their type icon is returned.
 * Cloud placeholders and offline files always get their type icon so that
 * drawing a folder never downloads anything. */
int  shell_icon_entry(const wchar_t *dir, const wchar_t *name, uint32_t name_len, uint32_t ext, uint32_t attrs);
int  shell_icon_folder(void);
int  shell_icon_path(const wchar_t *path); /* a specific item's own icon (e.g. Desktop), via the worker */
int  shell_icon_stock(int siid);           /* SHSTOCKICONID */
int  shell_icon_size(void);
void shell_icon_draw(HDC dc, int index, int x, int y, int faded);

/* ---- context menus ---------------------------------------------------------------- */
#define SHELL_CMD_FIRST 1000 /* app items in the menu must use smaller ids */

/* Shows `menu` (the app's own items) followed by the shell's items for
 * `paths` (all in one folder), or for the folder `dir` itself when n == 0.
 * Returns the app item picked, or 0. A shell verb named in `intercept`
 * (space-separated, e.g. "rename delete") is not run: it is copied to
 * verb_out for the app to handle. Any other shell item runs here. */
UINT shell_context_menu(HWND owner, HMENU menu, const wchar_t *dir, const wchar_t *const *paths, uint32_t n, POINT screen,
                        int extended, const char *intercept, char *verb_out, size_t verb_cap);
/* Forward the owner window's messages while a menu is open (submenus like
 * "Open with" and "Send to" draw themselves through these). */
int  shell_menu_msg(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *result);
int  shell_invoke_verb(HWND owner, const wchar_t *dir, const wchar_t *const *paths, uint32_t n, const char *verb);
int  shell_properties(HWND owner, const wchar_t *dir, const wchar_t *const *paths, uint32_t n);

/* ---- drag and drop -------------------------------------------------------------------- */
typedef struct shell_drop_handler {
    /* pt is in client coordinates. Return the DROPEFFECT to offer (the app
     * also updates its hover highlight here). */
    DWORD (*over)(void *ctx, POINT pt, DWORD keys, DWORD allowed, const wchar_t *const *paths, uint32_t n);
    void  (*leave)(void *ctx);
    DWORD (*drop)(void *ctx, POINT pt, DWORD effect, DWORD keys, DWORD allowed,
                   const wchar_t *const *paths, uint32_t n);
} shell_drop_handler;

int   shell_drop_register(HWND h, const shell_drop_handler *handler, void *ctx);
void  shell_drop_revoke(HWND h);
/* Explorer's rule: Ctrl copies, Shift moves, otherwise move within a volume
 * and copy across volumes. NONE when the drop makes no sense (a folder into
 * itself, or a move into the folder the items are already in). */
DWORD shell_drop_effect(const wchar_t *const *paths, uint32_t n, const wchar_t *dest, DWORD keys, DWORD allowed);
/* Starts a drag of `paths` (one folder); returns when it is dropped or cancelled. */
DWORD shell_drag_out(HWND h, const wchar_t *const *paths, uint32_t n, int right_button);
/* Legal choices independent of modifier keys, including an explicit same-folder copy. */
DWORD shell_drop_choices(const wchar_t *const *paths, uint32_t n, const wchar_t *dest, DWORD allowed);
HRESULT shell_drag_continue(int escape, DWORD keys, int right_button);
/* Worker-only: save a .lnk atomically, refusing an existing output. Does not launch the target. */
DWORD shell_create_shortcut(const wchar_t *source, const wchar_t *output);

/* ---- small actions -------------------------------------------------------------------- */
/* program: "auto" (Windows Terminal, else the shell alone), "wt", a name on
 * PATH ("pwsh", "cmd") or a full path. shell, in Windows Terminal or alone:
 * "auto" (pwsh, else powershell, else cmd), "pwsh", "powershell", "cmd", or
 * "profile": Windows Terminal's default profile (whatever it is, WSL included). */
int shell_open_terminal(const wchar_t *dir, const wchar_t *program, const wchar_t *shell);
/* 0: accepted publication; -1: failed; -2: failed and prior Unicode restoration failed. */
int shell_copy_paths(HWND owner, const wchar_t *const *paths, uint32_t n);
