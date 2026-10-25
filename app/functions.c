/* The built-in functions. Each entry is a complete contract (see fn.h):
 * adding one here adds no permanent UI and no startup cost; it shows up in
 * a menu only when it applies, and its key works only where it applies.
 *
 * Order matters for keys bound to several functions: the first that applies
 * wins (F5 copies to the other pane when there is one, else refreshes). */
#include "app.h"
#include "shell.h"

static Pane *P(fn_ctx *c) { return (Pane *)c->pane; }

static wchar_t *first_path(fn_ctx *c)
{
    wchar_t **v = fn_paths(c);
    return v && c->n ? v[0] : NULL;
}

/* The selection can turn out empty once gathered (selected rows filtered out). */
static int has_items(fn_ctx *c) { return fn_paths(c) && c->n; }

/* Drives and top-level places in the tree are not deleted, cut or renamed
 * from a stray key; Explorer's own menu items still work on them. */
static int not_root(const fn_ctx *c) { return !(c->state & FN_S_ROOT); }

/* ---- navigation ------------------------------------------------------------ */

static void run_back(fn_ctx *c) { app_go_back(P(c)); }
static void run_forward(fn_ctx *c) { app_go_forward(P(c)); }
static void run_up(fn_ctx *c) { app_go_up(P(c)); }
static void run_address(fn_ctx *c) { app_focus_address(P(c)); }
static void run_open(fn_ctx *c) { app_open_cursor(P(c)); }

static void run_open_folder(fn_ctx *c)
{
    wchar_t *path = first_path(c);
    if (path) app_navigate(P(c), path);
}

static void run_open_other(fn_ctx *c)
{
    wchar_t *path = first_path(c);
    if (path) app_navigate(app_other_pane(P(c)), path);
}

/* On items: only a folder. On the background or a tree folder: that folder. */
static int terminal_applies(const fn_ctx *c) { return c->n == 0 ? c->dir != NULL : (c->state & FN_S_DIR) != 0; }

static void run_terminal(fn_ctx *c)
{
    const wchar_t *dir = c->n ? first_path(c) : c->dir;
    wchar_t program[MAX_PATH], shell[MAX_PATH];
    const fn_def *self = fn_find("shell.terminal");
    if (!MultiByteToWideChar(CP_UTF8, 0, fn_option(self, "program"), -1, program, MAX_PATH)) program[0] = 0;
    if (!MultiByteToWideChar(CP_UTF8, 0, fn_option(self, "shell"), -1, shell, MAX_PATH)) shell[0] = 0;
    if (!dir) return;
    if (!program[0] || !_wcsicmp(program, L"panel")) app_terminal_run(dir, NULL, NULL);
    else if (shell_open_terminal(dir, program, shell)) app_status(P(c), 1, L"Could not start the terminal \"%ls\"", program);
}

/* ---- tabs -------------------------------------------------------------------- */

static int many_tabs(const fn_ctx *c) { return app_tab_count((Pane *)c->pane) > 1; }
static int closed_tabs(const fn_ctx *c) { return app_tab_closed_count() > 0; }
static void run_tab_new(fn_ctx *c) { app_tab_new(P(c), app_pane_path(P(c)), 1); }
static void run_tab_close(fn_ctx *c) { app_tab_close(P(c)); }
static void run_tab_next(fn_ctx *c) { app_tab_step(P(c), 1); }
static void run_tab_prev(fn_ctx *c) { app_tab_step(P(c), -1); }
static void run_tab_reopen(fn_ctx *c) { app_tab_reopen(P(c)); }

/* A middle click opens behind the shown tab, like a browser. */
static void run_tab_open(fn_ctx *c)
{
    wchar_t *path = first_path(c);
    if (path) app_tab_new(P(c), path, c->from != FN_FROM_MOUSE);
}

/* ---- pins ------------------------------------------------------------------------ */

/* From a menu on a folder item or the tree: that folder. From a key in a
 * pane: the folder the pane shows, like a bookmark. */
static const wchar_t *pin_target(const fn_ctx *c)
{
    if (c->where == FN_TREE || (c->from == FN_FROM_MENU && c->n))
        return (c->state & FN_S_DIR) && c->paths ? c->paths[0] : NULL;
    return c->dir;
}

/* Before the paths are gathered (a key press) it can't tell yet: it applies. */
static int pin_applies(const fn_ctx *c)
{
    if ((c->where == FN_TREE || (c->from == FN_FROM_MENU && c->n)) && !(c->state & FN_S_DIR)) return 0;
    const wchar_t *t = pin_target(c);
    return !t ? !c->paths : !app_is_pinned(t);
}

static int unpin_applies(const fn_ctx *c)
{
    const wchar_t *t = pin_target(c);
    return t && app_is_pinned(t);
}

static void run_pin(fn_ctx *c)
{
    fn_paths(c);
    const wchar_t *t = pin_target(c);
    if (!t) return;
    if (app_is_pinned(t)) {
        app_status(P(c), 0, L"Already pinned");
        return;
    }
    app_pin(t);
    app_status(P(c), 0, L"Pinned to the tree");
}

static void run_unpin(fn_ctx *c)
{
    fn_paths(c);
    const wchar_t *t = pin_target(c);
    if (t) app_unpin(t);
}

/* ---- files ------------------------------------------------------------------ */

static void run_copy_to_other(fn_ctx *c) { app_transfer(P(c), 0); }
static void run_move_to_other(fn_ctx *c) { app_transfer(P(c), 1); }

static void run_rename(fn_ctx *c)
{
    if(c->where==FN_PANE&&c->n>1){fn_run_id("file.bulk_rename",c);return;}
    if (c->where == FN_PANE) app_rename_cursor(P(c));
    else if (first_path(c)) app_rename_path(first_path(c));
}

static void run_delete(fn_ctx *c)
{
    /* Explorer's menu deletes permanently when Shift is held */
    int permanent = c->from == FN_FROM_VERB && GetKeyState(VK_SHIFT) < 0;
    if (has_items(c)) app_delete(c->paths, c->n, permanent);
}

static void run_delete_permanent(fn_ctx *c)
{
    if (has_items(c)) app_delete(c->paths, c->n, 1);
}

static void run_new_folder(fn_ctx *c) { app_new_item(P(c), 0); }
static void run_new_file(fn_ctx *c) { app_new_item(P(c), 1); }

static void run_properties(fn_ctx *c)
{
    fn_paths(c);
    if (shell_properties(app_window(), c->dir, (const wchar_t *const *)c->paths, c->n))
        app_status(P(c), 1, L"No properties to show");
}

static void copy_path_label(const fn_ctx *c, wchar_t *buf, size_t cap)
{
    _snwprintf(buf, cap, L"%ls", c->n == 0 ? L"Copy folder path" : c->n == 1 ? L"Copy path" : L"Copy paths");
}

static void run_copy_path(fn_ctx *c)
{
    int result;
    if (has_items(c)) result = shell_copy_paths(app_window(), (const wchar_t *const *)c->paths, c->n);
    else if (c->dir) result = shell_copy_paths(app_window(), &c->dir, 1);
    else return;
    if (result) {
        app_status(P(c), 1, result == -2
            ? L"Copy failed and the previous clipboard text could not be restored"
            : L"Could not copy paths to the clipboard");
        return;
    }
    app_status(P(c), 0, c->n > 1 ? L"%u paths copied" : L"Path copied", c->n);
}

/* ---- clipboard + selection ------------------------------------------------------- */

static void run_copy(fn_ctx *c)
{
    if (has_items(c)) app_clip_set(P(c), c->paths, c->n, 0);
}

static void run_cut(fn_ctx *c)
{
    if (has_items(c)) app_clip_set(P(c), c->paths, c->n, 1);
}

/* A key pastes into the folder shown; a menu on items only offers it for a folder. */
static int paste_applies(const fn_ctx *c)
{
    if (c->from == FN_FROM_MENU && (c->state & FN_S_ITEMS) && !(c->state & FN_S_DIR)) return 0;
    return c->dest && IsClipboardFormatAvailable(CF_HDROP);
}
static void run_paste(fn_ctx *c) { app_paste_into(c->dest); }
static void run_select_all(fn_ctx *c) { app_select_all(P(c)); }

/* ---- view + window ---------------------------------------------------------------- */

static void run_refresh(fn_ctx *c) { app_refresh(P(c)); }
static void run_sort_name(fn_ctx *c) { app_set_sort(P(c), FPE_SORT_NAME); }
static void run_sort_type(fn_ctx *c) { app_set_sort(P(c), FPE_SORT_EXT); }
static void run_sort_size(fn_ctx *c) { app_set_sort(P(c), FPE_SORT_SIZE); }
static void run_sort_modified(fn_ctx *c) { app_set_sort(P(c), FPE_SORT_MTIME); }
static void run_sort_created(fn_ctx *c) { app_set_sort(P(c), FPE_SORT_CTIME); }
static void run_hidden(fn_ctx *c) { app_toggle_hidden(); }
static void run_tree(fn_ctx *c) { app_toggle_tree(); }
static void run_dual(fn_ctx *c) { app_toggle_dual(); }
static void run_focus_next(fn_ctx *c) { app_focus_cycle(1); }
static void run_focus_prev(fn_ctx *c) { app_focus_cycle(-1); }

/* ---- debug mode ---------------------------------------------------------------------- */

/* Warm: while the panel is open it profiles and repaints twice a second;
 * closing it stops the function, which ends its timer. */
static void run_debug_panel(fn_ctx *c)
{
    const fn_def *self = fn_find("debug.panel");
    if (app_debug_shown()) {
        app_debug_show(0);
        fn_stop(self);
    } else {
        app_debug_show(1);
        fn_timer(self, 500);
    }
}

static void run_debug_reset(fn_ctx *c) { app_debug_reset(); }
static void run_debug_save(fn_ctx *c) { app_debug_save(); }

/* ---- the table ------------------------------------------------------------------------- */

#define ITEMS FN_S_ITEMS
#define ONE_DIR (FN_S_ONE | FN_S_DIR)

static const fn_opt terminal_opts[] = {
    { "program", "panel", "panel (the terminal in the side panel), auto (Windows Terminal, else the shell alone), wt, "
                          "pwsh, cmd or a full path" },
    { "shell", "auto", "the shell: auto (WSL Bash in Linux folders; otherwise pwsh, powershell or cmd), wsl, pwsh, powershell, cmd, "
                       "or profile (Windows Terminal's default profile, e.g. WSL; auto in the panel)", "auto wsl pwsh powershell cmd profile" },
    { NULL },
};

static const fn_def builtins[] = {
    /* navigation */
    { .id = "nav.back", .title = L"Back", .about = "Go back in this pane's history",
      .life = FN_COLD, .where = FN_ANYWHERE, .keys = "Alt+Left", .run = run_back },
    { .id = "nav.forward", .title = L"Forward", .about = "Go forward in this pane's history",
      .life = FN_COLD, .where = FN_ANYWHERE, .keys = "Alt+Right", .run = run_forward },
    { .id = "nav.up", .title = L"Up", .about = "Go to the parent folder",
      .life = FN_COLD, .where = FN_ANYWHERE, .keys = "Alt+Up, Backspace", .run = run_up },
    { .id = "nav.address", .title = L"Edit address", .about = "Type a path in the address bar",
      .life = FN_COLD, .where = FN_PANE, .keys = "Ctrl+L, Alt+D, F4", .run = run_address },
    { .id = "item.open", .title = L"Open", .about = "Open the item under the cursor (folders open in the pane)",
      .life = FN_COLD, .where = FN_PANE, .requires = ITEMS, .keys = "Enter",
      .menus = FN_MENU_ITEMS | FN_MENU_OURS, .group = 0, .run = run_open },
    { .id = "nav.open_folder", .title = L"Open", .about = "Explorer's Open on a folder: open it here, not in Explorer",
      .life = FN_COLD, .flags = FN_HIDDEN, .where = FN_ANYWHERE, .requires = ONE_DIR, .verbs = "open explore opennewwindow",
      .run = run_open_folder },
    { .id = "nav.open_other", .title = L"Open in other pane", .about = "Show the folder in the other pane",
      .life = FN_COLD, .where = FN_ANYWHERE, .requires = ONE_DIR | FN_S_DUAL,
      .menus = FN_MENU_ITEMS | FN_MENU_TREE, .group = 0, .run = run_open_other },

    /* tabs: the strip only shows once a pane has two */
    { .id = "tab.open", .title = L"Open in new tab", .about = "Open the folder in a new tab (middle click: behind this one)",
      .life = FN_COLD, .where = FN_ANYWHERE, .requires = ONE_DIR, .menus = FN_MENU_ITEMS | FN_MENU_TREE, .group = 0,
      .keys = "Ctrl+Enter", .run = run_tab_open },
    { .id = "shell.terminal", .title = L"Open terminal here", .about = "Start a terminal in the folder",
      .life = FN_COLD, .where = FN_ANYWHERE, .menus = FN_MENU_ITEMS | FN_MENU_FOLDER | FN_MENU_TREE, .group = 0,
      .opts = terminal_opts, .applies = terminal_applies, .run = run_terminal },
    { .id = "tab.new", .title = L"New tab", .about = "Open this folder again in a new tab",
      .life = FN_COLD, .where = FN_ANYWHERE, .requires = FN_S_FOLDER, .keys = "Ctrl+T", .run = run_tab_new },
    { .id = "tab.close", .title = L"Close tab", .about = "Close the shown tab (middle click on a tab)",
      .life = FN_COLD, .where = FN_ANYWHERE, .keys = "Ctrl+W, Ctrl+F4", .applies = many_tabs, .run = run_tab_close },
    { .id = "tab.next", .title = L"Next tab", .about = "Show the next tab",
      .life = FN_COLD, .where = FN_ANYWHERE, .keys = "Ctrl+Tab, Ctrl+PgDn", .applies = many_tabs, .run = run_tab_next },
    { .id = "tab.prev", .title = L"Previous tab", .about = "Show the previous tab",
      .life = FN_COLD, .where = FN_ANYWHERE, .keys = "Ctrl+Shift+Tab, Ctrl+PgUp", .applies = many_tabs,
      .run = run_tab_prev },
    { .id = "tab.reopen", .title = L"Reopen closed tab", .about = "Bring back the last closed tab",
      .life = FN_COLD, .where = FN_ANYWHERE, .keys = "Ctrl+Shift+T", .applies = closed_tabs, .run = run_tab_reopen },

    /* files (before view: F5 copies when there are two panes) */
    { .id = "file.copy_to_other", .title = L"Copy to other pane", .about = "Copy the selection to the other pane's folder (asks)",
      .life = FN_COLD, .where = FN_PANE, .requires = ITEMS | FN_S_DUAL, .keys = "F5", .run = run_copy_to_other },
    { .id = "file.move_to_other", .title = L"Move to other pane", .about = "Move the selection to the other pane's folder (asks)",
      .life = FN_COLD, .where = FN_PANE, .requires = ITEMS | FN_S_DUAL, .keys = "F6", .run = run_move_to_other },
    { .id = "file.rename", .title = L"Rename", .about = "Rename in place (the extension stays unselected)",
      .life = FN_COLD, .where = FN_ANYWHERE, .requires = ITEMS, .keys = "F2", .verbs = "rename", .applies = not_root,
      .menus = FN_MENU_ITEMS | FN_MENU_TREE | FN_MENU_OURS, .group = 3, .run = run_rename },
    { .id = "file.delete", .title = L"Delete", .about = "Move to the Recycle Bin",
      .life = FN_COLD, .where = FN_ANYWHERE, .requires = ITEMS, .keys = "Delete", .verbs = "delete", .applies = not_root,
      .menus = FN_MENU_ITEMS | FN_MENU_TREE | FN_MENU_OURS, .group = 3, .run = run_delete },
    { .id = "file.delete_permanent", .title = L"Delete permanently", .about = "Delete without the Recycle Bin (asks)",
      .life = FN_COLD, .where = FN_ANYWHERE, .requires = ITEMS, .keys = "Shift+Delete", .applies = not_root, .run = run_delete_permanent },
    { .id = "file.new_folder", .title = L"New folder", .about = "Create a folder and rename it",
      .life = FN_COLD, .where = FN_PANE, .requires = FN_S_FOLDER, .menus = FN_MENU_FOLDER, .group = 1,
      .keys = "F7, Ctrl+Shift+N", .run = run_new_folder },
    { .id = "file.new_file", .title = L"New text file", .about = "Create an empty text file and rename it",
      .life = FN_COLD, .where = FN_PANE, .requires = FN_S_FOLDER, .menus = FN_MENU_FOLDER, .group = 1,
      .keys = "Shift+F7", .run = run_new_file },
    { .id = "file.properties", .title = L"Properties", .about = "Windows' Properties window",
      .life = FN_COLD, .where = FN_ANYWHERE, .keys = "Alt+Enter", .needs = "svc.shell",
      .menus = FN_MENU_ITEMS | FN_MENU_FOLDER | FN_MENU_TREE | FN_MENU_OURS, .group = 7, .run = run_properties },
    { .id = "file.copy_path", .title = L"Copy path", .about = "Copy full paths as text",
      .life = FN_COLD, .where = FN_ANYWHERE, .menus = FN_MENU_ITEMS | FN_MENU_FOLDER | FN_MENU_TREE, .group = 4,
      .keys = "Ctrl+Shift+C", .label = copy_path_label, .run = run_copy_path },

    { .id = "tree.pin", .title = L"Pin to tree", .about = "Keep the folder at the top of the tree",
      .life = FN_COLD, .where = FN_ANYWHERE, .menus = FN_MENU_ITEMS | FN_MENU_FOLDER | FN_MENU_TREE, .group = 4,
      .keys = "Ctrl+D", .applies = pin_applies, .run = run_pin },
    { .id = "tree.unpin", .title = L"Unpin from tree", .about = "Remove the folder from the top of the tree",
      .life = FN_COLD, .where = FN_ANYWHERE, .menus = FN_MENU_ITEMS | FN_MENU_FOLDER | FN_MENU_TREE, .group = 4,
      .applies = unpin_applies, .run = run_unpin },

    /* clipboard + selection */
    { .id = "edit.cut", .title = L"Cut", .about = "Cut items to the clipboard",
      .life = FN_COLD, .where = FN_ANYWHERE, .requires = ITEMS, .keys = "Ctrl+X", .verbs = "cut", .applies = not_root,
      .menus = FN_MENU_ITEMS | FN_MENU_TREE | FN_MENU_OURS, .group = 2, .run = run_cut },
    { .id = "edit.copy", .title = L"Copy", .about = "Copy items to the clipboard",
      .life = FN_COLD, .where = FN_ANYWHERE, .requires = ITEMS, .keys = "Ctrl+C", .verbs = "copy",
      .menus = FN_MENU_ITEMS | FN_MENU_TREE | FN_MENU_OURS, .group = 2, .run = run_copy },
    { .id = "edit.paste", .title = L"Paste", .about = "Copy or move clipboard items here",
      .life = FN_COLD, .where = FN_ANYWHERE, .keys = "Ctrl+V", .verbs = "paste", .applies = paste_applies,
      .menus = FN_MENU_ITEMS | FN_MENU_FOLDER | FN_MENU_TREE | FN_MENU_OURS, .group = 2,
      .run = run_paste },
    { .id = "sel.all", .title = L"Select all", .about = "Select every row",
      .life = FN_COLD, .where = FN_PANE, .keys = "Ctrl+A", .run = run_select_all },

    /* view */
    { .id = "view.refresh", .title = L"Refresh", .about = "Read the folder again",
      .life = FN_COLD, .where = FN_PANE, .menus = FN_MENU_FOLDER, .group = 5, .keys = "Ctrl+R, F5",
      .run = run_refresh },
    { .id = "view.sort_name", .title = L"Sort by name", .about = "Sort by name (again: reverse)",
      .life = FN_COLD, .where = FN_PANE, .keys = "Ctrl+1", .run = run_sort_name },
    { .id = "view.sort_type", .title = L"Sort by type", .about = "Sort by extension (again: reverse)",
      .life = FN_COLD, .where = FN_PANE, .keys = "Ctrl+2", .run = run_sort_type },
    { .id = "view.sort_size", .title = L"Sort by size", .about = "Sort by size, largest first (again: reverse)",
      .life = FN_COLD, .where = FN_PANE, .keys = "Ctrl+3", .run = run_sort_size },
    { .id = "view.sort_modified", .title = L"Sort by date modified", .about = "Newest first (again: reverse)",
      .life = FN_COLD, .where = FN_PANE, .keys = "Ctrl+4", .run = run_sort_modified },
    { .id = "view.sort_created", .title = L"Sort by date created", .about = "Newest first (again: reverse)",
      .life = FN_COLD, .where = FN_PANE, .keys = "Ctrl+5", .run = run_sort_created },
    { .id = "view.hidden", .title = L"Show hidden files", .about = "Show or hide hidden and system items",
      .life = FN_COLD, .where = FN_ANYWHERE, .keys = "Ctrl+H", .run = run_hidden },
    { .id = "view.tree", .title = L"Folder tree", .about = "Show or hide the folder tree",
      .life = FN_COLD, .where = FN_ANYWHERE, .keys = "Ctrl+B", .run = run_tree },
    { .id = "view.dual", .title = L"Dual pane", .about = "Show one pane or two",
      .life = FN_COLD, .where = FN_ANYWHERE, .keys = "Ctrl+\\", .run = run_dual },
    { .id = "window.focus_next", .title = L"Next pane", .about = "Move focus: tree, left pane, right pane",
      .life = FN_COLD, .where = FN_ANYWHERE, .keys = "Tab", .run = run_focus_next },
    { .id = "window.focus_prev", .title = L"Previous pane", .about = "Move focus backwards",
      .life = FN_COLD, .where = FN_ANYWHERE, .keys = "Shift+Tab", .run = run_focus_prev },

    /* debug mode */
    { .id = "debug.panel", .title = L"Debug panel", .about = "Profiling panel; profiles and repaints only while open",
      .life = FN_WARM, .where = FN_ANYWHERE, .keys = "F12", .run = run_debug_panel, .tick = app_debug_tick },
    { .id = "debug.reset", .title = L"Reset profiling", .about = "Clear profiling counters",
      .life = FN_COLD, .where = FN_ANYWHERE, .keys = "Ctrl+F12", .run = run_debug_reset },
    { .id = "debug.save", .title = L"Save profile", .about = "Write a profiling report to %TEMP%",
      .life = FN_COLD, .where = FN_ANYWHERE, .keys = "Shift+F12", .run = run_debug_save },
};

void functions_register(void) { fn_register(builtins, sizeof builtins / sizeof builtins[0]); }
