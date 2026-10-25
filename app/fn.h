/* fn.h - the function registry.
 *
 * Everything a user can invoke is a function: a static fn_def that states
 * what it does, when it applies, how it is reached and what it may keep
 * alive. The core owns dispatch, key bindings, menus, settings, timers and
 * instrumentation; a function never installs hooks, timers or menu items of
 * its own. Adding a function therefore adds capability without adding
 * permanent UI or permanent cost.
 *
 * Three things are kept apart:
 *   capability  the function is compiled in. Code pages are only read from
 *               disk when they run, so this costs (almost) nothing.
 *   activation  settings [functions] id = off. An inactive function never
 *               starts, never shows up and its keys fall through. Functions
 *               that need it (`needs`) are inactive too.
 *   exposure    where an active function shows up: its keys ([keys]),
 *               context menus ([menu]) and the command palette ([palette]),
 *               and only when its conditions hold for what is targeted
 *               (`where`, `requires`, `applies`).
 *
 * Lifetime is part of the contract and the core enforces it:
 *   FN_HOT   needed all the time: started at startup (FN_DEFERRED: right
 *            after the first frame), stopped at exit.
 *   FN_WARM  started on first use; may keep resources (core timers) between
 *            uses; stopped after idle_ms without use, or on request.
 *   FN_COLD  started for one invocation and stopped as soon as it and its
 *            async work (fn_hold) are done. May not own timers.
 * A stopped function owns nothing: the core cancels its timers.
 *
 * Keys: a key may be bound to several functions; the first one (in
 * registration order) that applies to the current target runs. F5 copies to
 * the other pane when there is one and refreshes otherwise.
 */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#include "keys.h" /* fn_binding, FN_CTRL..., fn_parse_key, fn_format_key */

#define FN_MAX      192 /* built-in functions and the ones settings sections make (fn_family) */
#define FN_KEYS_MAX 4
#define FN_MENU_ID0 100 /* menu item ids: FN_MENU_ID0 + slot (below SHELL_CMD_FIRST) */

enum { FN_HOT, FN_WARM, FN_COLD };
/* A start/stop callback may retain owned work until completion on the UI
 * thread. Dependents cannot start while their dependency is pending. */
#define FN_PENDING 0x7fffffff

/* fn_def.flags */
#define FN_DEFERRED 0x1u /* FN_HOT: start right after the first frame, not before it */
#define FN_OPTIONAL 0x2u /* inactive unless settings turn it on */
#define FN_HIDDEN   0x4u /* never offered in the command palette (mouse gestures, shell verbs) */
#define FN_RESTART  0x8u /* its settings are read at startup only: changes apply at the next start */

/* fn_def.where: filesystem commands use FN_ANYWHERE; logical archive entries
 * require an explicit FN_ARCHIVE opt-in. */
#define FN_PANE 0x1u /* a file pane: its folder and selection */
#define FN_TREE 0x2u /* a folder in the tree */
#define FN_ARCHIVE 0x4u /* logical archive entry; never a normal filesystem operand */
#define FN_ANYWHERE (FN_PANE | FN_TREE)

/* fn_ctx.state, tested against fn_def.requires */
#define FN_S_ITEMS  0x01u /* at least one item targeted */
#define FN_S_ONE    0x02u /* exactly one item */
#define FN_S_DIR    0x04u /* exactly one item and it is a folder */
#define FN_S_DUAL   0x08u /* both panes are shown */
#define FN_S_FOLDER 0x10u /* the pane shows a real folder */
#define FN_S_ROOT   0x20u /* the target is a top-level place (a drive, Documents, ...) */

/* fn_def.menus: context menus the function appears in */
#define FN_MENU_ITEMS  0x1u /* right-click on items in a pane */
#define FN_MENU_FOLDER 0x2u /* right-click on a pane's empty space */
#define FN_MENU_ARCHIVE 0x40u /* archive contents only */
#define FN_MENU_TEXT 0x20u /* text reader context menu */
#define FN_MENU_IMAGE  0x10u /* image reader context menu */
#define FN_MENU_TREE   0x4u /* right-click on a folder in the tree */
/* Explorer's menu has the same item (Cut, Rename, Properties...): shown
 * only when fn_menu is asked for OpenPilot's own menu. */
#define FN_MENU_OURS   0x8u

enum { FN_FROM_CODE, FN_FROM_KEY, FN_FROM_MENU, FN_FROM_VERB, FN_FROM_MOUSE, FN_FROM_PALETTE, FN_FROM_PANEL };

/* What an invocation acts on, filled by the app. Cheap to build: paths are
 * only gathered when a function asks for them (fn_paths). */
typedef struct fn_ctx {
    uint32_t       where;  /* FN_PANE or FN_TREE */
    uint32_t       state;  /* FN_S_* */
    int            from;   /* FN_FROM_* */
    void          *pane;   /* the pane acted on (from the tree: the active pane) */
    const wchar_t *dir;    /* the folder shown, or the tree folder */
    const wchar_t *dest;   /* where new or pasted items go */
    uint32_t       n;      /* targeted items */
    wchar_t      **paths;  /* their full paths, NULL until fn_paths() */
    uint32_t       dirs;   /* how many of them are folders (drives included), set with paths */
    uint32_t     (*gather)(struct fn_ctx *c); /* fills paths, returns the count */
    void          *menu_capture; /* app-owned pane identity while a modal menu is open */
    uint32_t       menu_revision; /* command registry shown by a modal menu */
} fn_ctx;

/* values: NULL = any text; "a b c" = one of these words; "+a b c" = some of
 * them, space-separated (empty: the default); "#lo..hi" = a whole number in
 * that range. A value that doesn't fit is reported and the
 * default is used instead, so readers of fn_option need no checks of their own. */
typedef struct fn_opt {
    const char *name, *def, *help, *values;
} fn_opt;

typedef struct fn_def {
    const char    *id;       /* "area.name"; settings refer to it, keep it stable */
    const wchar_t *title;    /* menu text */
    const char    *about;    /* one line: what it does (and what it costs) */
    uint8_t        life;     /* FN_HOT / FN_WARM / FN_COLD */
    uint8_t        flags;    /* FN_DEFERRED, FN_OPTIONAL */
    uint8_t        group;    /* menu group; groups are separated */
    uint32_t       where;    /* FN_PANE, FN_TREE */
    uint32_t       requires; /* FN_S_* bits that must all be set */
    uint32_t       menus;    /* FN_MENU_* */
    const char    *keys;     /* default bindings: "F7, Ctrl+Shift+N" */
    const char    *verbs;    /* Explorer menu verbs this function handles instead of the shell */
    const char    *needs;    /* functions that must be running first: "svc.shell" */
    const fn_opt  *opts;     /* options, ended by { NULL } */
    uint32_t       idle_ms;  /* FN_WARM: stop after this long unused, 0 = only on request */

    int  (*applies)(const fn_ctx *c); /* further conditions: no I/O, no allocation */
    void (*label)(const fn_ctx *c, wchar_t *buf, size_t cap); /* menu text depending on the target */
    void (*run)(fn_ctx *c);
    int  (*start)(void); /* 0 = ok, FN_PENDING = start_complete later; requires stop_async */
    void (*stop)(void);
    void (*tick)(void);  /* fn_timer() */
    /* FN_HOT: whether the current settings need it at all (NULL = always).
     * Unwanted hot functions are not started at startup; they stay
     * available to fn_start and fn_hold. */
    int  (*wanted)(void);
    /* Reads its settings (fn_option, fn_active) into the app: once at
     * startup (fn_configure_all), then whenever they change (fn_reload),
     * unless FN_RESTART. Problems go to fn_config_error. */
    void (*configure)(void);
    int (*stop_async)(void); /* 0 = released; FN_PENDING = fn_stop_complete later */
} fn_def;

typedef struct fn_host {
    uint64_t (*now_ms)(void);
    void     (*arm)(uint32_t ms);                    /* call fn_tick() in ms; 0 cancels */
    void     (*status)(int err, const wchar_t *msg); /* tell the user (may be NULL) */
} fn_host;

/* ---- setup --------------------------------------------------------------- */
void fn_init(const fn_host *host);
int  fn_register(const fn_def *defs, uint32_t n); /* defs must outlive the registry */
/* A family: functions the settings file makes. tmpl->id is the prefix
 * ("relay."); each section [relay.NAME] (NAME: letters, digits, - and _, at
 * most 32) is a function copied from tmpl with the id relay.NAME, whose
 * options are the section. Its title is its "title" option, else NAME.
 * It comes and goes with its section (fn_reload); keys, menus, the palette
 * and [functions] treat it like any other. Register before the settings
 * are read. Its callbacks learn which one they serve from fn_self(). */
int  fn_family(const fn_def *tmpl);
/* The function whose callback (applies, label, run, configure, start,
 * stop) is being called, or NULL outside of one. */
const fn_def *fn_self(void);
/* INI text (UTF-8):
 *   [functions]  id = on | off
 *   [keys]       id = F2, Ctrl+Shift+R | none
 *   [menu]       id = on | off
 *   [palette]    id = on | off
 *   [<id>]       option = value
 *   [relay.NAME] a function of a family (fn_family): its options
 * Unknown names are reported by fn_settings_errors(). */
int  fn_settings(const char *text, size_t len); /* parsed completely before anything changes */
int  fn_settings_file(const wchar_t *path); /* a missing file is not an error */
const char *fn_settings_errors(void);       /* "" when fine; parse problems, then configure's */
/* The settings text with one setting changed, for the settings panel:
 *   sec "functions", key a function id, val on | off
 *   sec "keys", key a function id, val "F7, Ctrl+Shift+N" | none (written as fn_format_key names them)
 *   sec "menu" or "palette", key a function id that can be there, val on | off
 *   sec a function id, key one of its options, val a value that fits it
 * val NULL, or the default, removes the line so the default holds. The
 * rest stays as it was: comments, order, line ends. The line that counts
 * (the last) is the one changed; a missing one goes after a commented-out
 * one (";id = on", as fn_template writes), else at the end of its section,
 * else in a new section at the end. 0 = *out (malloc'd, *out_len bytes);
 * -1 = not a valid setting (why says what is); -2 = out of memory. */
int  fn_settings_edit(const char *text, size_t len, const char *sec, const char *key, const char *val, char **out,
                      size_t *out_len, char *why, size_t cap);
/* A transactional draft may edit one registered family's new/existing member
 * before it exists. The same option/key validation and INI preservation apply. */
int fn_settings_family_edit(const char *text,size_t len,const char *id,const char *sec,const char *key,
                           const char *val,char **out,size_t *out_len,char *why,size_t cap);
uint32_t fn_bindings(const fn_def *d,fn_binding *out,uint32_t cap);
void fn_reset(void);                        /* forget everything (tests) */
void fn_configure_all(void);                /* startup: every configure, in registration order */
/* New settings while running: the result is what a fresh start with them
 * would give. Functions whose activation or options changed are
 * configured again (FN_RESTART ones wait for the next start), running
 * functions now off are stopped, hot ones now needed are started.
 * Returns how many functions changed (keys, menus and palette included). */
int  fn_reload(const char *text, size_t len);
uint32_t fn_revision(void);                 /* changes before registry/settings mutation */
void fn_config_error(const fn_def *d, const char *fmt, ...); /* from configure: "[id] ..." */

/* ---- queries ------------------------------------------------------------- */
const fn_def *fn_find(const char *id);
uint32_t      fn_count(void);
const fn_def *fn_at(uint32_t i);
int  fn_active(const fn_def *d);  /* switched on, and everything it needs is too */
int  fn_running(const fn_def *d);
int  fn_applies(const fn_def *d, const fn_ctx *c);
/* Offered in the command palette: active, runnable, not FN_HIDDEN and not
 * switched off in [palette]. Whether it applies is up to the caller. */
int  fn_in_palette(const fn_def *d);
const char *fn_option(const fn_def *d, const char *name); /* setting, else default, else "" */
wchar_t **fn_paths(fn_ctx *c);

/* ---- invoking -------------------------------------------------------------- */
/* 1 = ran, 0 = inactive or does not apply, -1 = could not start. */
int  fn_run(const fn_def *d, fn_ctx *c);
int  fn_run_id(const char *id, fn_ctx *c);
int  fn_run_id_at_revision(const char *id, uint32_t revision, fn_ctx *c);
int  fn_key(uint8_t vk, uint8_t mods, fn_ctx *c);
int  fn_verb(const char *verb, fn_ctx *c);
/* Space-separated verbs of the functions that apply to c (for the shell menu). */
size_t fn_verbs(const fn_ctx *c, char *buf, size_t cap);
/* Appends the functions exposed in `menu` (FN_MENU_ITEMS, _FOLDER or _TREE,
 * | FN_MENU_OURS for OpenPilot's own menu) that apply to c; returns how many. */
uint32_t fn_menu(HMENU m, uint32_t menu, const fn_ctx *c);
int  fn_menu_run(UINT id, fn_ctx *c);
/* For work the app drives itself (a menu, a drag): starts d and what it
 * needs, counts a use and times it until fn_end. 0 = ok. */
int  fn_begin(const fn_def *d);
void fn_end(const fn_def *d);

/* ---- lifetime ---------------------------------------------------------------- */
int  fn_start(const fn_def *d);
void fn_stop(const fn_def *d);
int  fn_start_complete(const fn_def *d, int result); /* result 0/-1; UI thread; failed start owns nothing */
int  fn_stop_complete(const fn_def *d); /* UI thread; all async resources released */
int  fn_fail(const fn_def *d); /* runtime failure after owned work has exited; 0 if still stopping */
void fn_start_hot(int deferred);
/* Start at most one pending hot function (dependencies first). Keep cursor
 * across calls; 1 = continue next message, FN_PENDING = wait for completion. */
int fn_start_hot_step(int deferred, uint32_t *cursor);
int  fn_shutdown(void); /* 1 = all released; 0 = awaiting async stop completions */
/* Async work in flight keeps a function running. fn_hold starts it if
 * needed; release only what was held (0 = held). */
int  fn_hold(const fn_def *d);
void fn_release(const fn_def *d);
/* Calls d->tick every period_ms while d runs (0 stops). -1 for FN_COLD. */
int  fn_timer(const fn_def *d, uint32_t period_ms);
void fn_tick(void);

/* ---- keys ------------------------------------------------------------------------ */
int  fn_key_text(const fn_def *d, wchar_t *buf, size_t cap); /* first binding, 0 if none */

/* ---- reports ------------------------------------------------------------------- */
size_t fn_report(char *buf, size_t cap);     /* debug panel: what runs and what it holds */
size_t fn_report_csv(char *buf, size_t cap); /* fn,id,runs,,,state,holds */
size_t fn_list(char *buf, size_t cap);       /* every function with its contract and settings */
/* Every function and its settings for the settings panel, UTF-8, one record
 * per line, fields separated by tabs (tabs and line breaks inside become
 * spaces); returns the length it needs, writing at most cap - 1:
 *   fn    id title life state keys default-keys flags needs about
 *         state: on | off | needs (on, but something it needs is off)
 *         keys, default-keys: "Ctrl+Shift+N, F7" as fn_format_key names them
 *         flags: r restart-only, m hidden from menus, p hidden from the palette, o optional,
 *                k can have keys, M is in context menus, P can be in the palette,
 *                f made by its settings section (fn_family)
 *   opt   name value default set(0|1) values help
 *   error text */
size_t fn_describe(char *buf, size_t cap);
size_t fn_template(char *buf, size_t cap);   /* a commented settings file listing everything */
