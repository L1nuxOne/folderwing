#include "../src/namecodec.h"
/* fpdemo - a deliberately small explorer over the engine.
 *
 * Layout: folder tree | left pane | right pane, plus a shared status bar.
 * Each pane has tabs (the strip shows from two on; a hidden tab keeps only
 * its path, history and sort order) and a breadcrumb address bar. The
 * session (placement, layout, tabs, pins) is saved on exit (session.c).
 * Every pane and the tree is a plain child window with one back buffer and
 * no per-row controls: only visible rows are drawn, so a 100k-entry folder
 * costs the same to paint as a 10-entry one. Navigation shows cached
 * snapshots immediately and revalidates in the background; a watcher keeps
 * each open folder live, and those reloads also refresh the tree.
 *
 * Core input stays here: arrows/PgUp/PgDn/Home/End move (Shift extends),
 * clicks select (Ctrl toggles, Shift extends), Ctrl+Space toggles, typing
 * filters, Backspace edits the filter, Esc clears it, then the selection;
 * in the tree Right/Left expand and collapse, Enter/Space open.
 * Every command (open, rename, copy, toggles, debug mode, ...) is a function
 * in the registry (fn.c, functions.c): `fpdemo --functions` lists them with
 * their keys, and folderwing.ini switches them off, rebinds keys or hides
 * them from menus. Services with a running cost (OLE, the icon worker, drop
 * targets) are functions too, so they can be switched off the same way.
 *
 * Shell integration lives in shell.c and starts after the first frame. The
 * palette (palette.c: commands, Go to, Everything search) is an overlay that
 * exists only while open; the app feeds it places (app_places) and keeps
 * the recent folders it lists.
 *
 * Debug mode (off normally): `--debug` or OPENPILOT_DEBUG=1 profiles from
 * process start; F12 toggles the debug panel (and profiling) at any time,
 * Ctrl+F12 resets counters, Shift+F12 saves a report. `--profile-out=FILE`
 * writes the report when the window closes (headless; .csv for scripts).
 * Timing zones are named by feature (pane.*, tree.*, status.*, app.*,
 * debug.*) next to the engine's own (dir.*, view.*, engine.*, ...), so each
 * feature's cost is visible; ui.dispatch times every message and the
 * slowest one is reported by name. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <knownfolders.h>
#include <objbase.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <winternl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#include "app.h"
#include "menu.h"
#include "relay.h"
#include "relayedit.h"
#include <commdlg.h>
#include "palette.h"
#include "pdfview.h"
#include "imageview.h"
#include "mediaview.h"
#include "mediautil.h"
#include "archiveview.h"
#include "providerpath.h"
#include "imagebrowse.h"
#include "imageproto.h"
#include "textview.h"
#include "textutil.h"
#include "imageutil.h"
#include "thumbnails.h"
#include "session.h"
#include "workspace.h"
#include "workspaces.h"
#include "pathwire.h"
#include "folderview.h"
#include "folderwatch.h"
#include "pathentry.h"
#include "permissions.h"
#include "foldersize.h"
#include "spacewire.h"
#include "spaceproto.h"
#include "spacevolumes.h"
#include "wsltree.h"
#include "duplicateproto.h"
#include "contentproto.h"
#include "diffproto.h"
#include "gitproto.h"
#include "termutil.h"
#include "foldercompare.h"
#include "bulkrename.h"
#include "linkdialog.h"
#include "panelproto.h"
#include "sidepanel.h"
#include "actiondock.h"
#include "brand.h"
#include "settingspath.h"
#include "operations.h"
#include "theme.h"
#include "fpe.h"
#include "shell.h"
#include "dropwarm.h"
#include "typeicons.h"

#define WM_APP_LOADED  (WM_APP + 1)
#define WM_APP_JOB     (WM_APP + 3)
#define WM_APP_ICONS   (WM_APP + 4) /* icon worker delivered file icons */
#define WM_APP_FSOP    (WM_APP + 5) /* rename / new item finished */
#define WM_APP_RELEASE_BUFFER (WM_APP + 12) /* retire presented CPU pixels at the next UI turn */
#define WM_APP_DROP_WARM (WM_APP + 11) /* synthetic dispatch after owned event/thread signals */
#define WM_APP_LATE    (WM_APP + 6) /* first frame is up: initialize the shell */
#define WM_APP_IMAGE_LIST (WM_APP + 9)
#define WM_APP_SETTINGS_SAVED (WM_APP + 10) /* dispatched by the UI after its save worker exits */
#define WM_APP_THUMB   (WM_APP + 100) /* a cached thumbnail became available */
#define PM_ADDRESS_DONE (WM_APP + 8) /* a background Linux path resolution */
#define PM_GEOMETRY (WM_APP + 10) /* one deferred address/scrollbar update per pane */
#define PM_RENAME_DONE (WM_APP + 7) /* to a pane: the rename box lost focus */

#define TIMER_LOADING 1
#define TIMER_JOBTEXT 2
#define TIMER_FN      3 /* the registry's one timer (fn_tick) */
#define TIMER_PREVIEW 4 /* the cursor stopped on a PDF */
#define TIMER_LINGER  5 /* no PDF for a while: the preview goes */
#define TIMER_SETTINGS 6 /* the settings folder changed and has been quiet since */
#define TIMER_SETTINGS_SAVE 8 /* completion fallback inside native nested message loops */
#define TIMER_CLIPBOARD 9 /* bounded publication retries on the UI thread */

#define TIMER_THUMB 7 /* collect the visible tiles after navigation settles */

#define HIST_MAX 64

enum { ICONS_OURS, ICONS_MIXED, ICONS_WINDOWS };
#define RESTORE_SEL_MAX 256


typedef struct load_msg {
    uint64_t ticket;
    fpe_err  err;
    fpe_dir *dir;
} load_msg;

typedef struct job_msg {
    fpe_job_progress p;
    fpe_job_event event;
    uint64_t token;
    int diagnostic;
    wchar_t path[]; /* copied while the callback's path is valid */
} job_msg;

typedef struct bbuf {
    HDC     dc;
    HBITMAP bmp, old;
    int     w, h;       /* the bitmap's size: drawing uses at most the window's */
    int     release_queued;
} bbuf;

/* A tab that is not shown holds no snapshot, view or watcher: only where it
 * was and how it was sorted. Showing it again goes through the folder
 * cache, so it is instant. */
typedef struct Tab {
    wchar_t      *path; /* kept current for the shown tab too (labels, session) */
    wchar_t      *back[HIST_MAX], *fwd[HIST_MAX];
    int           nback, nfwd;
    fpe_view_opts opts;
    wchar_t       cursor[260]; /* the name under the cursor when it was left */
    int           workspace_restore; /* not yet successfully opened */
    DWORD         load_error;
} Tab;

#define TABS_MAX   32
#define CRUMBS_MAX 48

typedef struct crumb {
    int x0, x1;
    int len; /* chars of the path this crumb stands for; 0 = the collapsed "..." */
} crumb;

typedef struct Pane {
    int           idx;
    Tab           tabs[TABS_MAX];
    int           ntabs, tab;
    uint64_t      tabs_epoch;
    crumb         crumbs[CRUMBS_MAX];
    int           ncrumbs;
    HWND          hwnd, edit, distro_label;
    pathentry_worker *address_worker;
    uint64_t address_sequence;
    int address_resolving, address_editing;
    wchar_t address_distro[64];
    bbuf          bb;
    int           cw, ch;
    wchar_t       published_caption[400];
    int           published_sort, published_descending, published_size;

    fpe_view     *view;
    fpe_view_opts opts;
    wchar_t      *path;

    uint64_t nav_ticket, refresh_ticket;
    int geometry_pending;
    wchar_t *nav_path;
    wchar_t  nav_select[260];
    int      nav_push, nav_shown, loading;

    uint8_t *sel; /* by snapshot entry index, so it survives re-sorting */
    uint32_t sel_cap, nsel;
    uint32_t cursor, anchor;
    int      top, wheel;
    int      thumbnails, thumb_size; /* per-pane layout; size in logical pixels */

    wchar_t filter[256];
    int     filter_len;

    wchar_t *back[HIST_MAX], *fwd[HIST_MAX];
    int      nback, nfwd;

    wchar_t status[512];
    int     status_err;

    HWND          rename_edit;
    const fn_def *rename_held; /* file.rename runs while the box is open */
    wchar_t       rename_old[260];
    wchar_t want_select[260]; /* select (and maybe rename) this name once a reload shows it */
    int     want_rename;

    int      drop_row;            /* folder row highlighted under a drag, -1 none */
    uint64_t drop_scroll_tick;
    int      down_row, down_x, down_y, down_armed, down_deferred, down_button, right_suppress; /* drag detection */
    uint64_t down_revision; /* the row belongs only to this view revision */
} Pane;

enum { NODE_FOLDER = 0, NODE_QUICK, NODE_DRIVE, NODE_PIN, NODE_LINUX, NODE_DISTRO };

typedef struct tnode {
    struct tnode  *parent;
    struct tnode **kids;
    uint32_t       nkids;
    wchar_t       *path;  /* display path */
    wchar_t       *label; /* owned for roots, points into path otherwise */
    int            depth;
    int            icon; /* system image index for drives and quick roots, -1 unknown */
    uint8_t        kind, expanded, loaded, loading, failed;
    uint8_t        ti; /* type icon: folder, drive, home, ... */
} tnode;

typedef struct tpend {
    uint64_t ticket;
    wchar_t *path;
} tpend;

static struct {
    HWND     hwnd;
    bbuf     bb;
    int      cw, ch;
    tnode  **roots;
    uint32_t nroots;
    tnode  **rows;
    uint32_t nrows, rows_cap;
    int      roots_ready;
    int      top, wheel;
    tnode   *cursor;  /* keyboard focus row */
    tnode   *current; /* folder shown in the active pane */
    tpend   *pend;
    uint32_t npend, pend_cap;
    wchar_t *reveal;
    tnode   *drop;  /* node under a drag */
    uint64_t drop_since;
    tnode   *down;  /* drag detection */
    int      down_x, down_y, down_button, right_suppress;
} T;

static struct {
    HWND   hwnd;
    WNDPROC edit_proc;
    HFONT  font;
    HBRUSH edit_brush;
    UINT   dpi;
    int    row_h, edit_h, header_h, status_h, tab_h, pad, split;
    int    cw, ch;
    bbuf   bb;

    fpe_engine *engine;
    Pane        panes[2];
    int         active, dual, show_tree, show_hidden;
    int         tree_w;
    double      ratio;
    int         drag; /* 0 none, 1 tree splitter, 2 pane splitter */
    int         gap_tree_x, gap_pane_x;
    int body_x,body_y;

    wchar_t job_text[256];
    UINT    cf_effect;
    int     com_ready;
    WNDPROC rename_proc_orig;
    HWND    focus_save; /* focused child when the app was deactivated */
    double  startup_ms; /* process creation to first frame on screen */
    wchar_t settings[MAX_PATH + 32]; /* the settings file in use ("" = defaults only) */
    wchar_t session[MAX_PATH + 32];  /* the session file ("" = none: nothing restored or saved) */
    int     folder_on;
    int     crumbs;                  /* view.breadcrumbs is on: the address edit shows only while typing */
    int     thumb_on, thumb_held, thumb_default, thumb_default_size;
    uint32_t thumb_cache_mb;
    int     icon_style;              /* ICONS_*, from view.icons */
    wchar_t *closed[16];             /* recently closed tabs, newest last */
    int      nclosed;
    wchar_t *recent[SES_RECENT_MAX]; /* folders shown recently, newest first (nav.recent) */
    int      nrecent;

    /* PDF preview (preview.pdf): shown in place of the right pane */
    int      me_on,me_volume,me_muted; UINT me_linger_ms;
    int      tx_on, tx_wrap, tx_lines; uint32_t tx_limit_mb, tx_encoding; UINT tx_linger_ms;
    int      im_on; uint32_t im_cache_mb, im_background; UINT im_linger_ms;
    int      ar_on,ar_kind,ar_readers;double ar_ratio;wchar_t *ar_path;uint64_t ar_size;int ar_gap,ar_left,ar_width;
    int      pv_want_kind; uint64_t pv_want_size; int64_t pv_want_mtime;
    int      pv_on;       /* switched on and the helper is there */
    int      pv_shown, pv_swapped, pv_lingering;
    wchar_t *svg_source;
    wchar_t *pv_want;     /* the PDF under the cursor */
    imagebrowse_worker *image_worker;imagebrowse_list *image_list;
    Pane *image_source;fpe_view *image_view;uint64_t image_revision,image_sequence;
    uint32_t image_row,image_origin;int image_walk,image_moving,image_preserve,image_list_state;
    Pane    *pv_src;      /* the pane it is in */
    UINT     pv_linger_ms;
    uint32_t pv_colors;   /* PDFP_COLORS_*: [preview.pdf] colors, then the palette's choice */
    uint32_t pv_remember; /* [preview.pdf] remember: documents whose position is kept */
    int      wsl_columns; /* [wsl.helper] columns */

    /* debug mode */
    int      debug_cli;   /* --debug / --profile-out: profiling stays on even with the panel hidden */
    int      debug_panel;
    wchar_t *profile_out; /* --profile-out=FILE (.csv for machine-readable output) */
    int      closing, destroyed; /* keep the UI pump alive until async OLE cleanup finishes */
    uint32_t modal_seq;   /* bumped whenever a nested message loop runs (dialogs, menus, drags) */
    double   worst_ms;    /* slowest UI message since the last reset */
    UINT     worst_msg;
    wchar_t  worst_cls[32];
    HWND     dbg;
    bbuf     dbg_bb;
    HFONT    mono;
    int      dbg_cw, dbg_ch, dbg_top, dbg_wheel, dbg_lines;
} g;
static git_model g_git_model;
static int g_git_badges;
static int g_dock_position=AD_TREE_BOTTOM,g_dock_labels;
static uint32_t g_dock_mask=AD_ALL;
static ad_geometry g_dock_geometry;

typedef struct mark {
    const char *label;
    double      ms; /* since process creation */
} mark;

static struct {
    Pane *source, *target;
    int from, slot, x, y, started;
    uint64_t epoch;
} g_tab_drag;
static int g_file_drag_right;
static folderviews g_folder_views;
static wchar_t g_folder_file[MAX_PATH + 32];

static mark g_marks[16];
static int g_nmarks;

/* ---- small helpers ------------------------------------------------------- */

static int S(int v) { return MulDiv(v, (int)g.dpi, 96); }

static wchar_t *wdup(const wchar_t *s)
{
    size_t n = wcslen(s) + 1;
    wchar_t *d = malloc(n * sizeof(wchar_t));
    if (d) memcpy(d, s, n * sizeof(wchar_t));
    return d;
}

static wchar_t *join(const wchar_t *dir, const wchar_t *name)
{
    size_t a = wcslen(dir), b = wcslen(name);
    int sep = a && dir[a - 1] != L'\\';
    wchar_t *p = malloc((a + sep + b + 1) * sizeof(wchar_t));
    if (!p) return NULL;
    memcpy(p, dir, a * sizeof(wchar_t));
    if (sep) p[a] = L'\\';
    memcpy(p + a + sep, name, (b + 1) * sizeof(wchar_t));
    return p;
}

static int same_path(const wchar_t *a, const wchar_t *b) { return fpe_path_equal(a,b); }

/* path equals prefix or lies below it */
static int has_prefix(const wchar_t *path, const wchar_t *prefix)
{
    size_t lp = wcslen(prefix);
    if (!lp || _wcsnicmp(path, prefix, lp) != 0) return 0;
    return path[lp] == 0 || path[lp] == L'\\' || prefix[lp - 1] == L'\\';
}

/* Moves keyboard focus inside the window. SetFocus on a window that isn't
 * in the foreground activates it and can take the foreground from the app
 * the user is typing in. Real input always brings the window forward first,
 * so otherwise just remember the target for WM_SETFOCUS. */
static void focus(HWND h)
{
    if (GetForegroundWindow() == g.hwnd) SetFocus(h);
    else g.focus_save = h;
}

/* Nested message loops wait on the user; they must not count as UI stalls. */
UINT app_menu_track(HMENU menu,HWND owner,POINT point)
{
    g.modal_seq++;UINT choice=menu_track(menu,owner,point);g.modal_seq++;return choice;
}
static int modal_box(const wchar_t *msg, const wchar_t *title, UINT flags)
{
    g.modal_seq++;
    int r = MessageBoxW(g.hwnd, msg, title, flags);
    g.modal_seq++;
    return r;
}

static void main_status_redraw(void)
{
    RECT footer={0,g.ch-g.status_h,g.cw,g.ch};InvalidateRect(g.hwnd,&footer,FALSE);
}

static void set_status(Pane *p, int err, const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(p->status, 511, fmt, ap);
    va_end(ap);
    p->status[511] = 0;
    p->status_err = err;
    RECT footer={0,p->ch-g.status_h,p->cw,p->ch};
    InvalidateRect(p->hwnd,&footer,FALSE);
}

static void win_err(wchar_t *out, size_t cap, DWORD err)
{
    out[0] = 0;
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0, out, (DWORD)cap, NULL);
    size_t n = wcslen(out);
    while (n && (out[n - 1] == L'\n' || out[n - 1] == L'\r' || out[n - 1] == L'.')) out[--n] = 0;
}

static void fmt_size(wchar_t *b, size_t cap, uint64_t v)
{
    static const wchar_t *u[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double d = (double)v;
    int i = 0;
    while (d >= 1024 && i < 4) { d /= 1024; i++; }
    if (i == 0) _snwprintf(b, cap, L"%llu B", (unsigned long long)v);
    else _snwprintf(b, cap, L"%.1f %ls", d, u[i]);
}

static void fmt_time(wchar_t *b, size_t cap, int64_t ft)
{
    FILETIME f = { (DWORD)ft, (DWORD)(ft >> 32) };
    SYSTEMTIME utc, lt;
    FileTimeToSystemTime(&f, &utc);
    SystemTimeToTzSpecificLocalTime(NULL, &utc, &lt);
    _snwprintf(b, cap, L"%04u-%02u-%02u  %02u:%02u", lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute);
}

static double since_start_ms(void)
{
    FILETIME created, x1, x2, x3, now;
    GetProcessTimes(GetCurrentProcess(), &created, &x1, &x2, &x3);
    GetSystemTimePreciseAsFileTime(&now);
    ULARGE_INTEGER a = { { created.dwLowDateTime, created.dwHighDateTime } };
    ULARGE_INTEGER b = { { now.dwLowDateTime, now.dwHighDateTime } };
    return (double)(b.QuadPart - a.QuadPart) / 10000.0;
}

/* Startup milestones: a handful of timestamps, recorded unconditionally. */
static void startup_mark(const char *label)
{
    if (g_nmarks < (int)(sizeof g_marks / sizeof g_marks[0])) {
        g_marks[g_nmarks].label = label;
        g_marks[g_nmarks].ms = since_start_ms();
        g_nmarks++;
    }
}

/* ---- drawing primitives ------------------------------------------------------ */

/* Big enough for w x hh. While a window is resized, its bitmap is made a
 * little bigger than needed so every step doesn't make a new one; one much
 * bigger than needed is given back. */
static int bbuf_ensure(bbuf *b, HWND h, int w, int hh)
{
    if(w<1)w=1;if(hh<1)hh=1;
    if(b->dc&&w<=b->w&&hh<=b->h&&b->w-w<=384&&b->h-hh<=384)return 0;
    if(b->dc){w=(w+127)&~127;hh=(hh+127)&~127;}
    HDC screen=GetDC(h);
    if(!b->dc)b->dc=CreateCompatibleDC(screen);
    /* CPU-backed pixels avoid device-dependent bitmap synchronization during
     * text rendering and presentation. Allocate before discarding the frame. */
    BITMAPINFO info={.bmiHeader={sizeof(BITMAPINFOHEADER),w,-hh,1,32,BI_RGB}};
    void *pixels=NULL;HBITMAP bitmap=b->dc?CreateDIBSection(screen,&info,DIB_RGB_COLORS,&pixels,NULL,0):NULL;
    ReleaseDC(h,screen);
    if(!bitmap)return -1;
    HGDIOBJ previous=SelectObject(b->dc,bitmap);
    if(!previous||previous==HGDI_ERROR){DeleteObject(bitmap);return -1;}
    if(b->bmp)DeleteObject(b->bmp);else b->old=previous;
    b->bmp=bitmap;b->w=w;b->h=hh;SetBkMode(b->dc,TRANSPARENT);
    return 1;
}

static void bbuf_discard(bbuf *b)
{
    int64_t t=fpe_prof_begin();
    if(b->bmp){SelectObject(b->dc,b->old);DeleteObject(b->bmp);b->bmp=NULL;}
    b->w=b->h=0;b->release_queued=0;
    FPE_PROF_END("paint.buffer.release",t);
}
static void bbuf_presented(bbuf *b,HWND window)
{
    /* Windows owns the displayed surface. A later paint redraws its damage;
     * retaining another full client bitmap would duplicate idle memory. */
    if(!b->release_queued){
        if(PostMessageW(window,WM_APP_RELEASE_BUFFER,0,0))b->release_queued=1;
        else bbuf_discard(b);
    }
}

static void fill(HDC dc, int x, int y, int w, int h, COLORREF c)
{
    RECT r = { x, y, x + w, y + h };
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, GetStockObject(DC_BRUSH));
}

static void frame(HDC dc, int x, int y, int w, int h, COLORREF c)
{
    RECT r = { x, y, x + w, y + h };
    SetDCBrushColor(dc, c);
    FrameRect(dc, &r, GetStockObject(DC_BRUSH));
}

static void text(HDC dc, const wchar_t *s, int len, int x, int y, int w, int h, COLORREF c, UINT align)
{
    if (w <= 0) return;
    RECT r = { x, y, x + w, y + h };
    SetTextColor(dc, c);
    DrawTextW(dc, s, len, &r, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

/* A type icon in the 16-px (scaled) icon slot of a row. */
static void draw_type_icon(HDC dc, ti_kind k, int x, int y, int h, uint32_t flags, COLORREF color)
{
    ti_draw(dc, k, x, y + (h - S(16)) / 2, S(16), flags, color);
}

/* With view.icons style = mixed, programs and shortcuts keep the icons
 * their makers gave them: for them the icon is the identity. */
static int shell_icon_for(ti_kind k)
{
    return shell_ready() &&
           (g.icon_style == ICONS_WINDOWS || (g.icon_style == ICONS_MIXED && (k == TI_PROGRAM || k == TI_SHORTCUT)));
}

/* A system image list icon centred in the 16-px (scaled) icon slot. */
static void draw_shell_icon(HDC dc, int idx, int x, int y, int faded, int link)
{
    static int link_overlay = -2;
    int sz = shell_icon_size();
    int ix = x + (S(16) - sz) / 2, iy = y + (g.row_h - sz) / 2;
    shell_icon_draw(dc, idx, ix, iy, faded);
    if (link) {
        if (link_overlay == -2) link_overlay = shell_icon_stock(SIID_LINK);
        shell_icon_draw(dc, link_overlay, ix, iy, 0);
    }
}

/* Type icons (view.icons); Windows' icons where the style asks for them and
 * the shell has them. Type icons need nothing, so the first frame already
 * shows the final ones. */
static void draw_entry_icon(HDC dc, int x, int y, const fpe_entry *e, const wchar_t *dir, const wchar_t *name)
{
    int link = (e->attrs & FILE_ATTRIBUTE_REPARSE_POINT) && (e->reparse_tag & 0x20000000u);
    int hidden = (e->attrs & FILE_ATTRIBUTE_HIDDEN) != 0;
    ti_kind k = ti_of_name(name, e->name_len, e->ext, e->attrs);
    if (shell_icon_for(k)) {
        int idx = shell_icon_entry(dir, name, e->name_len, e->ext, e->attrs);
        if (idx >= 0) {
            draw_shell_icon(dc, idx, x, y, hidden, link || k == TI_SHORTCUT);
            return;
        }
    }
    draw_type_icon(dc, k, x, y, g.row_h, (hidden ? TI_FADED : 0) | (link ? TI_LINK : 0), 0);
}

/* Small solid triangle: right-pointing when collapsed, down when expanded. */
static void draw_arrow(HDC dc, int x, int y, int expanded, COLORREF c)
{
    int cy = y + g.row_h / 2, s = S(4);
    for (int i = 0; i <= s; i++) {
        if (expanded) fill(dc, x - s + i, cy - s / 2 + i, 2 * (s - i) + 1, 1, c);
        else fill(dc, x - s / 2 + i, cy - s + i, 1, 2 * (s - i) + 1, c);
    }
}

static void on_loaded(void *user, uint64_t ticket, fpe_err err, fpe_dir *dir)
{
    load_msg *m = malloc(sizeof *m);
    if (!m) { fpe_dir_release(dir); return; }
    m->ticket = ticket;
    m->err = err;
    m->dir = dir;
    if (!PostMessageW(g.hwnd, WM_APP_LOADED, 0, (LPARAM)m)) {
        fpe_dir_release(dir);
        free(m);
    }
}

static struct {Pane *pane;uint64_t ticket;wchar_t *path;uint32_t line,column;uint64_t bytes;int64_t mtime;wchar_t expected[CF_QUERY];} g_content_preview;
static void content_preview_forget(void);
static void content_preview_try(Pane *pane,uint64_t ticket);
static Pane *active_pane(void) { return &g.panes[g.active]; }
static Pane *other_pane(Pane *p) { return &g.panes[p->idx ^ 1]; }

static void tree_reveal(const wchar_t *path);
static void tree_on_snapshot(fpe_dir *d);
static void update_title(void);
static void set_active(int idx);
static void term_note_pane(int force);
static void layout(void);
static void rename_begin(Pane *p, uint32_t row);
static void rename_end(Pane *p, int commit);
static void p_apply_want(Pane *p);
static void ctx_free(fn_ctx *c);
static void pane_ctx(fn_ctx *c, Pane *p, int from, int items);

/* ======================================================================== */
/* Pane                                                                      */
/* ======================================================================== */

static uint32_t p_count(const Pane *p) { return p->view ? fpe_view_count(p->view) : 0; }
static int p_tabs_h(const Pane *p) { return p->ntabs > 1 ? g.tab_h : 0; } /* the strip shows only with 2+ tabs */
static int p_addr_y(const Pane *p) { return p_tabs_h(p) + g.pad; }
static int p_header_y(const Pane *p) { return p_addr_y(p) + g.edit_h + g.pad; }
static int p_list_top(const Pane *p) { return p_header_y(p) + g.header_h; }

static int p_distro_width(const Pane *p) { return p->address_distro[0]?(p->cw/3<S(140)?p->cw/3:S(140)):0; }
static void p_address_text(Pane *p,int typing)
{
    wchar_t distro[64],linux[PATHENTRY_BYTES+1];
    if(!typing && p->address_editing)return;
    SetWindowTextW(p->edit,(typing||!g.crumbs)&&pathentry_display(p->path,distro,64,linux,PATHENTRY_BYTES+1)?linux:p->path?p->path:L"");
}
static void p_address_cancel(Pane *p)
{
    ++p->address_sequence;pathentry_cancel(p->address_worker);p->address_resolving=0;
    SetPropW(p->hwnd,L"Address.Resolving",NULL);
}
static void p_place_address_control(Pane *p,HWND control,int x,int y,int w,int h)
{
    RECT r;
    if(GetWindowRect(control,&r)) {
        MapWindowPoints(NULL,p->hwnd,(POINT *)&r,2);
        if(r.left==x && r.top==y && r.right-r.left==w && r.bottom-r.top==h)return;
    }
    if(MoveWindow(control,x,y,w,h,FALSE))InvalidateRect(control,NULL,FALSE);
}
static void p_place_edit(Pane *p)
{
    int64_t trace = fpe_prof_begin();
    int x = g.pad + S(6), y = p_addr_y(p) + S(5), w = p->cw - 2 * g.pad - S(12), h = g.edit_h - S(8);
    int badge=p_distro_width(p);x+=badge;w-=badge;if(w<1)w=1;
    if(p->distro_label) {
        p_place_address_control(p,p->distro_label,g.pad+S(6),y,badge>S(8)?badge-S(8):1,h);
        /* IsWindowVisible also checks ancestors. A hidden pane must retain
         * its badge's own visibility for when the pane is shown again. */
        int visible=(GetWindowLongPtrW(p->distro_label,GWL_STYLE)&WS_VISIBLE)!=0;
        if(visible!=(badge!=0))ShowWindow(p->distro_label,badge?SW_SHOW:SW_HIDE);
    }
    p_place_address_control(p,p->edit,x,y,w,h);
    FPE_PROF_END("pane.address_layout", trace);
}
static int p_list_bottom(const Pane *p) { return p->ch - g.status_h; }

static thumb_grid p_grid(const Pane *p)
{
    int edge = S(p->thumb_size ? p->thumb_size : 160);
    if (edge > THUMB_EDGE_MAX)
        edge = THUMB_EDGE_MAX;
    return thumb_grid_make(p->cw, p_list_bottom(p) - p_list_top(p), edge, S(8), g.row_h * 2);
}
static int p_columns_count(const Pane *p) { return p->thumbnails ? p_grid(p).columns : 1; }
static int p_cell_h(const Pane *p) { return p->thumbnails ? p_grid(p).cell_h : g.row_h; }
static int p_scroll_count(const Pane *p)
{
    int c = p_columns_count(p);
    return ((int)p_count(p) + c - 1) / c;
}
static void thumbnails_collect(void);
static void tab_drag_cancel(void);
static int is_pdf(const fpe_entry *e, const wchar_t *name);

static int p_page(const Pane *p)
{
    if (p->thumbnails) { thumb_grid grid=p_grid(p); return grid.rows*grid.columns; }
    int r = (p_list_bottom(p) - p_list_top(p)) / g.row_h;
    return r < 1 ? 1 : r;
}

static void p_clamp(Pane *p)
{
    if (p->thumbnails) { p->top=thumb_grid_top(p_grid(p),p_count(p),p->top); return; }
    int max_top = (int)p_count(p) - p_page(p);
    if (p->top > max_top) p->top = max_top;
    if (p->top < 0) p->top = 0;
}

static void p_ensure_visible(Pane *p, uint32_t row)
{
    if ((int)row < p->top) p->top = (int)row;
    else if ((int)row >= p->top + p_page(p)) p->top = ((int)row / p_columns_count(p) - p_page(p) / p_columns_count(p) + 1) * p_columns_count(p);
    p_clamp(p);
}

static void preview_note(Pane *p);

static void p_redraw(Pane *p)
{
    preview_note(p);
    if (g.thumb_on && g.hwnd && (g.panes[0].thumbnails || g.panes[1].thumbnails || g.thumb_held))
        SetTimer(g.hwnd, TIMER_THUMB, 25, NULL);
    wchar_t title[400] = L"Details";
    if (p->thumbnails) {
        const wchar_t *name=p_count(p)&&p->cursor<p_count(p)?fpe_view_name(p->view,p->cursor):L"";
        _snwprintf(title,400,L"Thumbnails %d | Columns %d | Top %d | Selected %u | Cursor %ls",
            p->thumb_size,p_columns_count(p),p->top,p->nsel,name);title[399]=0;
    }
    /* These values are owned here. Avoid repeating native writes (and their
     * UI locks/notifications) on selection, refresh and geometry redraws.
     * Remember only successful publications, so failures can be retried. */
    if (wcscmp(p->published_caption,title) && SetWindowTextW(p->hwnd,title))
        wcscpy(p->published_caption,title);
    int sort=(int)p->opts.key+1, descending=p->opts.descending+1;
    if (p->published_sort!=sort && SetPropW(p->hwnd,L"FolderView.Sort",(HANDLE)(uintptr_t)sort))
        p->published_sort=sort;
    if (p->published_descending!=descending && SetPropW(p->hwnd,L"FolderView.Descending",(HANDLE)(uintptr_t)descending))
        p->published_descending=descending;
    if (p->published_size!=p->thumb_size && SetPropW(p->hwnd,L"FolderView.Size",(HANDLE)(uintptr_t)p->thumb_size))
        p->published_size=p->thumb_size;
    SCROLLINFO si = { sizeof si, SIF_ALL | SIF_DISABLENOSCROLL, 0, p_scroll_count(p) ? p_scroll_count(p) - 1 : 0,
                      (UINT)(p_page(p)/p_columns_count(p)), p->top/p_columns_count(p), 0 };
    /* SetScrollInfo clamps the page to the range. Compare that same value,
     * otherwise every redraw of a small folder repeats the native update. */
    if(si.nPage>(UINT)(si.nMax-si.nMin)+1u)si.nPage=(UINT)(si.nMax-si.nMin)+1u;
    /* Update only changed values. Queue nonclient painting with the normal
     * frame; synchronous scroll-bar painting would compound navigation latency. */
    SCROLLINFO now = { sizeof now, SIF_ALL };
    if (!GetScrollInfo(p->hwnd, SB_VERT, &now) || now.nMin != si.nMin || now.nMax != si.nMax || now.nPage != si.nPage ||
        now.nPos != si.nPos) {
        int64_t t = fpe_prof_begin();
        SetScrollInfo(p->hwnd, SB_VERT, &si, FALSE);
        RedrawWindow(p->hwnd,NULL,NULL,RDW_INVALIDATE|RDW_FRAME);
        FPE_PROF_END("pane.scrollbar", t);
    }
    InvalidateRect(p->hwnd, NULL, FALSE);
    actiondock_refresh();
}

/* Native size batches only publish geometry. Child controls and scrollbars
 * finish in separate counted dispatches, before any pane paint. Live resizing
 * flushes these updates explicitly before its complete frame is painted. */
static void p_finish_geometry(Pane *p)
{
    if(!p->geometry_pending)return;
    int64_t t=fpe_prof_begin();p->geometry_pending=0;
    p_place_edit(p);p_redraw(p);
    FPE_PROF_END("pane.finish_geometry",t);
}

static void p_sel_reset(Pane *p, uint32_t n)
{
    if (n > p->sel_cap) {
        uint8_t *s = realloc(p->sel, n);
        if (!s) return;
        p->sel = s;
        p->sel_cap = n;
    }
    if (p->sel) memset(p->sel, 0, p->sel_cap);
    p->nsel = 0;
}

static int p_is_sel(const Pane *p, uint32_t row)
{
    uint32_t e = fpe_view_entry_index(p->view, row);
    return e != UINT32_MAX && e < p->sel_cap && p->sel[e];
}

static void p_set_sel(Pane *p, uint32_t row, int on)
{
    uint32_t e = fpe_view_entry_index(p->view, row);
    if (e == UINT32_MAX || e >= p->sel_cap || p->sel[e] == on) return;
    p->sel[e] = (uint8_t)on;
    if (on) p->nsel++;
    else p->nsel--;
}

static void p_clear_sel(Pane *p)
{
    if (p->sel) memset(p->sel, 0, p->sel_cap);
    p->nsel = 0;
}

static void p_select_range(Pane *p, uint32_t a, uint32_t b)
{
    p_clear_sel(p);
    if (a > b) { uint32_t t = a; a = b; b = t; }
    for (uint32_t r = a; r <= b && r < p_count(p); r++) p_set_sel(p, r, 1);
}

static void p_move_cursor(Pane *p, int64_t to, int shift, int ctrl)
{
    uint32_t n = p_count(p);
    if (!n) return;
    if (to < 0) to = 0;
    if (to >= n) to = n - 1;
    p->cursor = (uint32_t)to;
    if (shift) {
        p_select_range(p, p->anchor, p->cursor);
    } else if (!ctrl) {
        p_clear_sel(p);
        p_set_sel(p, p->cursor, 1);
        p->anchor = p->cursor;
    }
    p_ensure_visible(p, p->cursor);
    p_redraw(p);
}

/* Full paths of the selected visible rows, or of the cursor row. */
/* dirs (may be NULL): how many of them are folders */
static uint32_t p_targets(Pane *p, wchar_t ***out, uint32_t *dirs)
{
    uint32_t n = p_count(p), k = 0, nd = 0;
    *out = NULL;
    if (dirs) *dirs = 0;
    if (!n || !p->path) return 0;
    uint32_t want = p->nsel ? p->nsel : 1;
    wchar_t **v = calloc(want, sizeof *v);
    if (!v) return 0;
    if (p->nsel) {
        for (uint32_t r = 0; r < n && k < want; r++)
            if (p_is_sel(p, r)) {
                v[k++] = join(p->path, fpe_view_name(p->view, r));
                nd += fpe_entry_is_dir(fpe_view_at(p->view, r)) != 0;
            }
    } else if (p->cursor < n) {
        v[k++] = join(p->path, fpe_view_name(p->view, p->cursor));
        nd += fpe_entry_is_dir(fpe_view_at(p->view, p->cursor)) != 0;
    }
    if (dirs) *dirs = nd;
    *out = v;
    return k;
}

static void free_targets(wchar_t **v, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) free(v[i]);
    free(v);
}

/* ---- pane navigation --------------------------------------------------------- */

static void hist_push(wchar_t **stack, int *n, const wchar_t *path)
{
    if (*n == HIST_MAX) {
        free(stack[0]);
        memmove(stack, stack + 1, (HIST_MAX - 1) * sizeof *stack);
        (*n)--;
    }
    stack[(*n)++] = wdup(path);
}

static void p_refresh(Pane *p);

static void on_folder_changed(void *user)
{
    p_refresh(user);
}

static void p_restart_watch(Pane *p)
{
    folderwatch_set((unsigned)p->idx, p->path, on_folder_changed, p);
}

/* Moves path to the front of the recent folders (Go to lists them). */
static void recent_note(const wchar_t *path)
{
    static const fn_def *on;
    if (!on) on = fn_find("nav.recent");
    if (!path || !fn_active(on)) return;
    if (g.nrecent && same_path(g.recent[0], path)) return;
    int at = 0;
    while (at < g.nrecent && !same_path(g.recent[at], path)) at++;
    wchar_t *keep = at < g.nrecent ? g.recent[at] : wdup(path);
    if (!keep) return;
    if (at == g.nrecent) {
        if (g.nrecent == SES_RECENT_MAX) free(g.recent[--g.nrecent]);
        at = g.nrecent++;
    }
    memmove(&g.recent[1], &g.recent[0], (size_t)at * sizeof g.recent[0]);
    g.recent[0] = keep;
}

static int folder_view_enabled(void) { return g.folder_on && g.session[0]; }
static void folder_view_values(Pane *p, const wchar_t *path);
static void folder_view_note(Pane *p, uint32_t fields);

static void p_show_dir(Pane *p, fpe_dir *d, const wchar_t *select, int push)
{
    int64_t t = fpe_prof_begin();
    rename_end(p, 1); /* commits against the folder it was started in */
    const wchar_t *np = fpe_dir_path(d);
    fpe_view_opts old_opts = p->opts;
    int old_mode = p->thumbnails, old_size = p->thumb_size;
    folder_view_values(p, np);
    Tab *restoring=&p->tabs[p->tab];
    if(restoring->workspace_restore&&restoring->path&&same_path(restoring->path,np)){
        p->opts.key=restoring->opts.key;p->opts.descending=restoring->opts.descending;
    }
    fpe_view *next = NULL;
    if (!p->view || old_opts.key != p->opts.key || old_opts.descending != p->opts.descending) {
        /* Build the new sort once, and commit only after allocation succeeds. */
        next = fpe_view_create(d, &p->opts);
        if (!next) {
            p->opts = old_opts;
            p->thumbnails = old_mode;
            p->thumb_size = old_size;
            set_status(p, 1, L"Not enough memory to show this folder");
            FPE_PROF_END("pane.show_dir", t);
            return;
        }
    }
    if (push && p->path && !same_path(np, p->path)) {
        hist_push(p->back, &p->nback, p->path);
        while (p->nfwd) free(p->fwd[--p->nfwd]);
    }

    p->filter_len = 0;
    p->filter[0] = 0;
    if (next) {
        if (p->view) fpe_view_destroy(p->view);
        p->view = next;
    } else {
        fpe_view_set_filter(p->view, NULL);
        fpe_view_set_dir(p->view, d);
    }

    free(p->path);
    p->path = wdup(np);
    free(p->tabs[p->tab].path);
    p->tabs[p->tab].path = wdup(np);
    p->tabs[p->tab].workspace_restore=0;p->tabs[p->tab].load_error=0;
    recent_note(p->path);
    p_sel_reset(p, fpe_dir_count(d));

    uint32_t row = select && *select ? fpe_view_find(p->view, select) : UINT32_MAX;
    p->cursor = p->anchor = row == UINT32_MAX ? 0 : row;
    p->top = row == UINT32_MAX ? 0 : (int)row - p_page(p) / 2;
    p_clamp(p);
    if (row != UINT32_MAX) p_set_sel(p, row, 1);

    wchar_t distro[64]={0},native[PATHENTRY_BYTES+1];
    pathentry_display(p->path,distro,64,native,PATHENTRY_BYTES+1);
    if(wcscmp(p->address_distro,distro)) {
        wcscpy(p->address_distro,distro);
        wchar_t badge[80];_snwprintf(badge,80,L"WSL: %ls",distro);badge[79]=0;
        SetWindowTextW(p->distro_label,badge);
    }
    p_place_edit(p);p_address_text(p,0);
    SetPropW(p->hwnd,L"Address.Error",NULL);
    p_restart_watch(p);
    p->status[0] = 0;
    p_redraw(p);

    tree_on_snapshot(d);
    if (p->idx == g.active) {
        update_title();
        tree_reveal(p->path);
    }
    p_apply_want(p);
    FPE_PROF_END("pane.show_dir", t);
}

static void p_refresh_in_place(Pane *p, fpe_dir *d)
{
    int64_t t = fpe_prof_begin();
    wchar_t cur[260] = L"";
    const wchar_t *cn = fpe_view_name(p->view, p->cursor);
    if (cn) wcsncpy(cur, cn, 259);
    int offset = (int)p->cursor - p->top;

    wchar_t *keep[RESTORE_SEL_MAX];
    uint32_t nkeep = 0;
    if (p->nsel && p->nsel <= RESTORE_SEL_MAX)
        for (uint32_t r = 0; r < p_count(p) && nkeep < RESTORE_SEL_MAX; r++)
            if (p_is_sel(p, r)) keep[nkeep++] = wdup(fpe_view_name(p->view, r));

    fpe_view_set_dir(p->view, d);
    p_sel_reset(p, fpe_dir_count(d));
    for (uint32_t i = 0; i < nkeep; i++) {
        uint32_t r = fpe_view_find(p->view, keep[i]);
        if (r != UINT32_MAX) p_set_sel(p, r, 1);
        free(keep[i]);
    }

    uint32_t r = cur[0] ? fpe_view_find(p->view, cur) : UINT32_MAX;
    if (r != UINT32_MAX) p->cursor = r;
    else if (p->cursor >= p_count(p)) p->cursor = p_count(p) ? p_count(p) - 1 : 0;
    p->top = (int)p->cursor - offset;
    p_clamp(p);
    p_redraw(p);
    tree_on_snapshot(d);
    p_apply_want(p);
    FPE_PROF_END("pane.refresh", t);
}

static void p_navigate(Pane *p, const wchar_t *path, const wchar_t *select, int push)
{
    if(g_content_preview.pane==p)content_preview_forget();
    p->down_armed = p->down_deferred = 0; /* navigation may await a new snapshot */
    int64_t t = fpe_prof_begin();
    p_address_cancel(p);
    free(p->nav_path);
    p->nav_path = wdup(path);
    wcsncpy(p->nav_select, select ? select : L"", 259);
    p->nav_select[259] = 0;
    p->nav_push = push;
    p->nav_shown = 0;

    /* Cached: paint it now, let the load below quietly bring it up to date. */
    int restoring=p->tabs[p->tab].workspace_restore;
    if(restoring&&fpe_wsl_path_kind(path)&&!fn_active(fn_find("wsl.helper"))){p->nav_ticket=0;p->loading=0;p->tabs[p->tab].load_error=ERROR_NOT_SUPPORTED;SetPropW(p->hwnd,L"Address.Loading",NULL);SetPropW(p->hwnd,L"Address.Error",(HANDLE)(uintptr_t)ERROR_NOT_SUPPORTED);set_status(p,1,L"Enable WSL integration to open this saved folder");p_redraw(p);return;}
    fpe_dir *d = restoring?NULL:fpe_engine_cached(g.engine, path);
    p->nav_ticket = fpe_engine_load(g.engine, path, on_loaded, NULL);
    SetPropW(p->hwnd,L"Address.Loading",(HANDLE)(uintptr_t)(p->nav_ticket!=0));
    if (d) {
        p_show_dir(p, d, p->nav_select, push);
        fpe_dir_release(d);
        p->nav_shown = 1;
    } else {
        p->loading = 1;
        SetTimer(p->hwnd, TIMER_LOADING, 150, NULL);
    }
    FPE_PROF_END("pane.navigate", t);
}

static int p_handle_loaded(Pane *p, load_msg *m)
{
    if (m->ticket == p->nav_ticket) {
        p->nav_ticket = 0;SetPropW(p->hwnd,L"Address.Loading",NULL);
        p->loading = 0;
        KillTimer(p->hwnd, TIMER_LOADING);
        if (m->err) {
            if(p->tabs[p->tab].workspace_restore)p->tabs[p->tab].load_error=m->err;
            wchar_t e[256];
            win_err(e, 256, m->err);
            set_status(p, 1, L"Cannot open %ls: %ls", p->nav_path, e);
            SetPropW(p->hwnd,L"Address.Error",(HANDLE)(uintptr_t)m->err);
            p_address_text(p,0);
            p_redraw(p);
        } else if (p->nav_shown && same_path(fpe_dir_path(m->dir), p->path)) {
            p_refresh_in_place(p, m->dir);
        } else {
            p_show_dir(p, m->dir, p->nav_select, p->nav_push);
        }
        content_preview_try(p,m->ticket);
        return 1;
    }
    if (m->ticket == p->refresh_ticket) {
        p->refresh_ticket = 0;
        if (!m->err && same_path(fpe_dir_path(m->dir), p->path)) {if(p->view)p_refresh_in_place(p,m->dir);else p_show_dir(p,m->dir,NULL,0);}
        else if (m->err) {if(p->tabs[p->tab].workspace_restore)p->tabs[p->tab].load_error=m->err;set_status(p, 1, L"This folder is no longer available");}
        return 1;
    }
    return 0;
}

static void p_refresh(Pane *p)
{
    if(p->tabs[p->tab].workspace_restore&&p->path&&fpe_wsl_path_kind(p->path)&&!fn_active(fn_find("wsl.helper"))){set_status(p,1,L"Enable WSL integration to open this saved folder");return;}
    if (p->path) p->refresh_ticket = fpe_engine_load(g.engine, p->path, on_loaded, NULL);
}

static void p_go_up(Pane *p)
{
    if (!p->path) return;
    size_t n = wcslen(p->path);
    if (n <= 3) return; /* drive root */
    const wchar_t *slash = wcsrchr(p->path, L'\\');
    if (!slash || slash == p->path) return;
    size_t keep = (size_t)(slash - p->path);
    if (keep == 2 && p->path[1] == L':') keep = 3;
    if (p->path[0] == L'\\' && p->path[1] == L'\\' && wcschr(p->path + 2, L'\\') == slash) return; /* \\server */
    wchar_t *parent = malloc((keep + 1) * sizeof(wchar_t));
    if (!parent) return;
    memcpy(parent, p->path, keep * sizeof(wchar_t));
    parent[keep] = 0;
    wchar_t *child = wdup(slash + 1);
    p_navigate(p, parent, child, 1);
    free(child);
    free(parent);
}

static void p_go_back(Pane *p)
{
    if (!p->nback) return;
    wchar_t *path = p->back[--p->nback];
    hist_push(p->fwd, &p->nfwd, p->path);
    p_navigate(p, path, NULL, 0);
    free(path);
}

static void p_go_forward(Pane *p)
{
    if (!p->nfwd) return;
    wchar_t *path = p->fwd[--p->nfwd];
    hist_push(p->back, &p->nback, p->path);
    p_navigate(p, path, NULL, 0);
    free(path);
}

/* ---- tabs ------------------------------------------------------------------------- */

/* A restored tab has no authority to display an older folder's listing.
 * Keep its exact target/address even if the asynchronous load fails. */
static void workspace_address(Pane *p){wchar_t native[PATHENTRY_BYTES+1],distro[64]={0},badge[80];pathentry_display(p->path,distro,64,native,PATHENTRY_BYTES+1);wcscpy(p->address_distro,distro);swprintf(badge,80,L"WSL: %ls",distro);SetWindowTextW(p->distro_label,badge);p_address_text(p,0);p_place_edit(p);}
static void workspace_placeholder(Pane *p){
    wchar_t *path=wdup(p->tabs[p->tab].path);if(!path)return;
    p_address_cancel(p);p->nav_ticket=p->refresh_ticket=0;p->loading=0;
    KillTimer(p->hwnd,TIMER_LOADING);SetPropW(p->hwnd,L"Address.Loading",NULL);
    free(p->nav_path);p->nav_path=NULL;
    if(p->view){fpe_view_destroy(p->view);p->view=NULL;}
    free(p->path);p->path=path;p_sel_reset(p,0);p->cursor=p->anchor=0;p->top=0;
    p->filter_len=0;p->filter[0]=0;p->down_armed=p->down_deferred=0;
    folderwatch_clear((unsigned)p->idx);
    workspace_address(p);p_redraw(p);
}

/* The shown tab's state lives in the pane itself; these move it in and out. */
static void tab_save(Pane *p)
{
    Tab *t = &p->tabs[p->tab];
    memcpy(t->back, p->back, sizeof p->back);
    memcpy(t->fwd, p->fwd, sizeof p->fwd);
    t->nback = p->nback;
    t->nfwd = p->nfwd;
    p->nback = p->nfwd = 0;
    t->opts = p->opts;
    const wchar_t *cn = p->view ? fpe_view_name(p->view, p->cursor) : NULL;
    wcsncpy(t->cursor, cn ? cn : L"", 259);
    t->cursor[259] = 0;
}

static void tab_show(Pane *p, int i)
{
    p->down_armed = p->down_deferred = 0;
    Tab *t = &p->tabs[i];
    p->tab = i;
    memcpy(p->back, t->back, sizeof p->back);
    memcpy(p->fwd, t->fwd, sizeof p->fwd);
    p->nback = t->nback;
    p->nfwd = t->nfwd;
    t->nback = t->nfwd = 0;
    p->opts = t->opts;
    p->opts.show_hidden = p->opts.show_system = (uint8_t)g.show_hidden;
    if(t->workspace_restore)workspace_placeholder(p);
    if (p->view) fpe_view_set_opts(p->view, &p->opts);
    if (t->path) p_navigate(p, t->path, t->cursor, 0);
    if(t->workspace_restore&&!p->nav_ticket&&!p->view&&!t->load_error){t->load_error=ERROR_NOT_ENOUGH_MEMORY;set_status(p,1,L"Workspace folder could not be opened; retry with Refresh");}
    p_redraw(p);
}

static void tab_strip_changed(Pane *p)
{
    p_place_edit(p);
    p_clamp(p);
    p_redraw(p);
}

/* A new tab right after the shown one; the shown tab's sort order carries over. */
static void tab_new(Pane *p, const wchar_t *path, int show)
{
    if (!path) return;
    if (p->ntabs >= TABS_MAX) {
        set_status(p, 1, L"%d tabs is the limit", TABS_MAX);
        return;
    }
    int at = p->tab + 1;
    memmove(&p->tabs[at + 1], &p->tabs[at], (size_t)(p->ntabs - at) * sizeof(Tab));
    memset(&p->tabs[at], 0, sizeof(Tab));
    p->tabs[at].path = wdup(path);
    p->tabs[at].opts = p->opts;
    p->ntabs++;
    p->tabs_epoch++;
    if (p->ntabs == 2) tab_strip_changed(p);
    if (show) {
        tab_save(p);
        tab_show(p, at);
    } else {
        p_redraw(p);
    }
}

static void tab_free(Tab *t)
{
    free(t->path);
    for (int i = 0; i < t->nback; i++) free(t->back[i]);
    for (int i = 0; i < t->nfwd; i++) free(t->fwd[i]);
    memset(t, 0, sizeof *t);
}

static void tab_close(Pane *p, int i)
{
    if (p->ntabs <= 1 || i < 0 || i >= p->ntabs) return;
    int shown = i == p->tab;
    if (shown) tab_save(p); /* its history is freed with it */
    if (p->tabs[i].path) {
        if (g.nclosed == 16) {
            free(g.closed[0]);
            memmove(g.closed, g.closed + 1, 15 * sizeof *g.closed);
            g.nclosed--;
        }
        g.closed[g.nclosed++] = wdup(p->tabs[i].path);
    }
    tab_free(&p->tabs[i]);
    memmove(&p->tabs[i], &p->tabs[i + 1], (size_t)(p->ntabs - i - 1) * sizeof(Tab));
    memset(&p->tabs[p->ntabs - 1], 0, sizeof(Tab));
    p->ntabs--;
    p->tabs_epoch++;
    if (p->ntabs == 1) tab_strip_changed(p);
    if (shown) tab_show(p, i < p->ntabs ? i : p->ntabs - 1);
    else if (i < p->tab) p->tab--;
    p_redraw(p);
}

static void tab_step(Pane *p, int dir)
{
    if (p->ntabs < 2) return;
    tab_save(p);
    tab_show(p, (p->tab + dir + p->ntabs) % p->ntabs);
}

static void tab_select(Pane *p, int i)
{
    if (i == p->tab || i < 0 || i >= p->ntabs) return;
    tab_save(p);
    tab_show(p, i);
}

static void tab_reopen(Pane *p)
{
    if (!g.nclosed) return;
    wchar_t *path = g.closed[--g.nclosed];
    tab_new(p, path, 1);
    free(path);
}

/* x positions of tab i: [x0, x1); the close box is the last S(20) px. */
static int tab_width(const Pane *p)
{
    int w = (p->cw - 2 * g.pad) / (p->ntabs ? p->ntabs : 1);
    return w > S(200) ? S(200) : w;
}

/* Tab under client x (in the strip), or -1; *close: on its close box. */
static int tab_at(const Pane *p, int x, int *close)
{
    int w = tab_width(p);
    if (w <= 0 || x < g.pad) return -1;
    int i = (x - g.pad) / w;
    if (i >= p->ntabs) return -1;
    *close = w >= S(48) && x >= g.pad + (i + 1) * w - S(20);
    return i;
}

static const wchar_t *path_label(const wchar_t *path)
{
    const wchar_t *s = path ? wcsrchr(path, L'\\') : NULL;
    return s && s[1] ? s + 1 : path ? path : L"";
}

/* Tab dragging moves only the tab's owned state. Directory snapshots and
 * watchers remain with the shown panes, so inactive tabs stay lightweight. */
static void tab_drag_cancel(void)
{
    HWND source=g_tab_drag.source?g_tab_drag.source->hwnd:NULL;
    memset(&g_tab_drag,0,sizeof g_tab_drag);
    if(source && GetCapture()==source)ReleaseCapture();
    for(int i=0;i<2;++i)if(g.panes[i].hwnd)InvalidateRect(g.panes[i].hwnd,NULL,FALSE);
}
static int tab_drag_valid_source(void)
{
    Pane *p=g_tab_drag.source;
    return p && p->tabs_epoch==g_tab_drag.epoch && p->ntabs>1 &&
           g_tab_drag.from>=0 && g_tab_drag.from<p->ntabs && fn_active(fn_find("tab.drag")) &&
           !(g_tab_drag.from==p->tab && p->loading);
}
static void tab_drag_over(POINT screen)
{
    g_tab_drag.target=NULL;
    if(tab_drag_valid_source())for(int i=0;i<2;++i){
        Pane *p=&g.panes[i];RECT r;
        if(!IsWindowVisible(p->hwnd)||!GetWindowRect(p->hwnd,&r)||!PtInRect(&r,screen))continue;
        POINT pt=screen;ScreenToClient(p->hwnd,&pt);
        if(p!=g_tab_drag.source && (p->ntabs>=TABS_MAX || p->loading))break;
        if(p==g_tab_drag.source && pt.y>=p_tabs_h(p))break;
        if(p!=g_tab_drag.source && pt.y>=p->ch-g.status_h)break;
        int w=tab_width(p),slot=p->ntabs;
        if(p_tabs_h(p) && pt.y<p_tabs_h(p) && w>0){
            slot=(pt.x-g.pad+w/2)/w;
            if(slot<0)slot=0;if(slot>p->ntabs)slot=p->ntabs;
        }
        g_tab_drag.target=p;g_tab_drag.slot=slot;break;
    }
    SetCursor(LoadCursorW(NULL,(LPCWSTR)(g_tab_drag.target?IDC_SIZEALL:IDC_NO)));
    for(int i=0;i<2;++i)InvalidateRect(g.panes[i].hwnd,NULL,FALSE);
}
static void tab_drag_begin(Pane *p,int index,int x,int y)
{
    tab_drag_cancel();
    if(!fn_active(fn_find("tab.drag"))){tab_select(p,index);return;}
    g_tab_drag.source=p;g_tab_drag.from=index;g_tab_drag.x=x;g_tab_drag.y=y;
    g_tab_drag.epoch=p->tabs_epoch;SetCapture(p->hwnd);
}
static void tab_drag_finish(POINT screen)
{
    if(!g_tab_drag.started){
        Pane *p=g_tab_drag.source;int index=g_tab_drag.from;
        int valid=p && p->tabs_epoch==g_tab_drag.epoch && index<p->ntabs;
        tab_drag_cancel();if(valid)tab_select(p,index);return;
    }
    tab_drag_over(screen);
    Pane *src=g_tab_drag.source,*dst=g_tab_drag.target;
    int from=g_tab_drag.from,slot=g_tab_drag.slot;
    int valid=tab_drag_valid_source();tab_drag_cancel();
    if(!valid||!dst)return;
    rename_end(src,1);
    if(src==dst){
        int to=slot>from?slot-1:slot;
        if(to==from)return;
        Tab moved=src->tabs[from];int active=src->tab;
        if(to<from)memmove(src->tabs+to+1,src->tabs+to,(from-to)*sizeof(Tab));
        else memmove(src->tabs+from,src->tabs+from+1,(to-from)*sizeof(Tab));
        src->tabs[to]=moved;
        if(active==from)src->tab=to;
        else if(to<from && active>=to && active<from)src->tab++;
        else if(to>from && active>from && active<=to)src->tab--;
        src->tabs_epoch++;p_redraw(src);return;
    }
    rename_end(dst,1);
    int shown=src->tab==from,old_active=src->tab;
    if(shown)tab_save(src);
    tab_save(dst);
    Tab moved=src->tabs[from];
    memmove(src->tabs+from,src->tabs+from+1,(src->ntabs-from-1)*sizeof(Tab));
    memset(src->tabs+--src->ntabs,0,sizeof(Tab));src->tabs_epoch++;
    memmove(dst->tabs+slot+1,dst->tabs+slot,(dst->ntabs-slot)*sizeof(Tab));
    dst->tabs[slot]=moved;dst->ntabs++;dst->tabs_epoch++;
    if(shown)tab_show(src,from<src->ntabs?from:src->ntabs-1);
    else src->tab=old_active>from?old_active-1:old_active;
    tab_show(dst,slot);
    tab_strip_changed(src);tab_strip_changed(dst);focus(dst->hwnd);
}

static void p_paint_tabs(Pane *p, HDC dc)
{
    fill(dc, 0, 0, p->cw, g.tab_h, C_TREE_BG);
    int w = tab_width(p);
    for (int i = 0; i < p->ntabs; i++) {
        int x = g.pad + i * w, on = i == p->tab;
        int dragging=g_tab_drag.started && g_tab_drag.source==p && g_tab_drag.from==i;
        fill(dc, x, S(3), w - S(2), g.tab_h - S(3), dragging ? C_SEL : on ? C_BG : C_HEADER);
        if (on && p->idx == g.active) fill(dc, x, S(3), w - S(2), S(2), C_ACCENT);
        if(p->tabs[i].load_error)text(dc,L"!",1,x+S(4),S(3),S(10),g.tab_h-S(3),C_ERROR,DT_LEFT);
        text(dc, path_label(p->tabs[i].path), -1, x + S(p->tabs[i].load_error?16:8), S(3), w - (w>=S(48)?S(32):S(10)) - S(p->tabs[i].load_error?8:0), g.tab_h - S(3), p->tabs[i].load_error?C_ERROR:on ? C_TEXT : C_DIM,
             DT_LEFT);
        if(w>=S(48))text(dc, L"\x00D7", 1, x + w - S(22), S(3), S(18), g.tab_h - S(3), C_DIM, DT_CENTER);
    }
}

static void p_open_row(Pane *p, uint32_t row)
{
    const fpe_entry *e = fpe_view_at(p->view, row);
    if (!e) return;
    wchar_t *full = join(p->path, fpe_view_name(p->view, row));
    if (!full) return;
    if (fpe_entry_is_dir(e)) {
        p_navigate(p, full, NULL, 1);
    } else {
        if (!g.com_ready) {
            CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            g.com_ready = 1;
        }
        if(fpe_wsl_requires_native(full))set_status(p,1,L"This Linux name requires native operations; Windows cannot open it safely");
        else ShellExecuteW(g.hwnd,NULL,full,NULL,p->path,SW_SHOWNORMAL);
    }
    free(full);
}

static void p_navigate_typed(Pane *p, const wchar_t *typed)
{
    /* Read-only acknowledgment of a submitted address, distinct from text
     * still being edited and from background refreshes. */
    uintptr_t request=(uintptr_t)GetPropW(p->hwnd,L"Address.Request")+1;
    SetPropW(p->hwnd,L"Address.Request",(HANDLE)request);
    int kind=pathentry_kind(typed);
    if(kind) {
        p_address_cancel(p);p->nav_ticket=0;SetPropW(p->hwnd,L"Address.Loading",NULL);p->loading=0;KillTimer(p->hwnd,TIMER_LOADING);
        SetPropW(p->hwnd,L"Address.Error",NULL);
        if(kind<0 || wcslen(typed)>=PATHENTRY_MAX || !fn_active(fn_find("wsl.helper"))) {
            DWORD error=kind<0?ERROR_INVALID_NAME:wcslen(typed)>=PATHENTRY_MAX?ERROR_FILENAME_EXCED_RANGE:ERROR_NOT_SUPPORTED;
            set_status(p,1,kind<0?L"Use ~ or ~/path for the Linux user's home":error==ERROR_NOT_SUPPORTED?L"Enable WSL integration to enter a Linux path":L"Linux path is too long");
            SetPropW(p->hwnd,L"Address.Error",(HANDLE)(uintptr_t)error);p_address_text(p,0);p_redraw(p);return;
        }
        if(!p->address_worker)p->address_worker=pathentry_start(p->hwnd,PM_ADDRESS_DONE);
        int root=!_stricmp(fn_option(fn_find("wsl.helper"),"user"),"root");
        if(!p->address_worker || !pathentry_submit(p->address_worker,p->path,typed,root,p->address_sequence)) {
            set_status(p,1,L"Cannot start Linux path resolution");SetPropW(p->hwnd,L"Address.Error",(HANDLE)(uintptr_t)ERROR_NOT_ENOUGH_MEMORY);p_address_text(p,0);p_redraw(p);return;
        }
        p->address_resolving=p->loading=1;SetPropW(p->hwnd,L"Address.Resolving",(HANDLE)1);
        set_status(p,0,L"Resolving Linux path in %ls...",p->address_distro[0]?p->address_distro:L"the current or default WSL distro");
        SetTimer(p->hwnd,TIMER_LOADING,150,NULL);p_redraw(p);return;
    }
    wchar_t expanded[4096];
    DWORD n = ExpandEnvironmentStringsW(typed, expanded, 4096);
    p_navigate(p, n && n <= 4096 ? expanded : typed, NULL, 1);
}

/* ---- pane sorting + filtering ------------------------------------------------- */

static void p_keep_cursor_after(Pane *p, void (*change)(Pane *))
{
    wchar_t cur[260] = L"";
    const wchar_t *cn = fpe_view_name(p->view, p->cursor);
    if (cn) wcsncpy(cur, cn, 259);
    change(p);
    uint32_t r = cur[0] ? fpe_view_find(p->view, cur) : UINT32_MAX;
    p->cursor = r != UINT32_MAX ? r : 0;
    p->anchor = p->cursor;
    p_ensure_visible(p, p->cursor);
    p_redraw(p);
}

static void apply_opts(Pane *p) { fpe_view_set_opts(p->view, &p->opts); }
static void apply_filter(Pane *p) { fpe_view_set_filter(p->view, p->filter_len ? p->filter : NULL); }

/* Preferences contain only explicit choices. Unset fields follow the current
 * defaults, and disabling thumbnail support keeps its saved mode intact. */
static void folder_view_values(Pane *p, const wchar_t *path)
{
    if (!folder_view_enabled()) return;
    fv_value v = {0};
    fv_get(&g_folder_views, path, &v);
    p->opts.key = (v.mask & FV_SORT) ? (fpe_sort_key)v.sort : FPE_SORT_NAME;
    p->opts.descending = (v.mask & FV_SORT) ? (uint8_t)v.descending : 0;
    p->thumbnails = g.thumb_on && ((v.mask & FV_MODE) ? (int)v.mode : g.thumb_default);
    p->thumb_size = (v.mask & FV_SIZE) ? (int)v.size : g.thumb_default_size;
    if (!p->thumb_size) p->thumb_size = 160;
}
static void folder_view_apply(Pane *p)
{
    if (!p->path || !folder_view_enabled()) return;
    fpe_sort_key old_sort = p->opts.key;
    int old_desc = p->opts.descending;
    rename_end(p, 1);
    folder_view_values(p, p->path);
    if (p->view && (old_sort != p->opts.key || old_desc != p->opts.descending))
        p_keep_cursor_after(p, apply_opts);
    else { p_clamp(p); p_ensure_visible(p, p->cursor); p_redraw(p); }
}
static void folder_view_note(Pane *p, uint32_t fields)
{
    if (!p->path || !folder_view_enabled()) return;
    fv_value v = {fields, (uint32_t)p->thumbnails, (uint32_t)p->thumb_size,
                  (uint32_t)p->opts.key, p->opts.descending};
    if (fv_set(&g_folder_views, p->path, v) < 0) {
        set_status(p, 1, L"Could not remember this folder's view");
        return;
    }
    for (int i = 0; i < 2; ++i)
        if (&g.panes[i] != p && fv_same_path(g.panes[i].path, p->path)) folder_view_apply(&g.panes[i]);
}
static void configure_folder_views(void)
{
    g.folder_on = fn_active(fn_find("view.folder_memory"));
    if (g.hwnd && folder_view_enabled())
        for (int i = 0; i < 2; ++i) folder_view_apply(&g.panes[i]);
}
static int folder_reset_applies(const fn_ctx *c)
{
    return c->pane && ((Pane *)c->pane)->path && folder_view_enabled();
}
static void run_folder_reset(fn_ctx *c)
{
    Pane *p = c->pane;
    if (!p || !folder_view_enabled()) return;
    if (fv_reset(&g_folder_views, p->path) < 0) {
        set_status(p, 1, L"Could not reset this folder's view; close and reopen to clear pending resets");
        return;
    }
    for (int i = 0; i < 2; ++i)
        if (fv_same_path(g.panes[i].path, p->path)) folder_view_apply(&g.panes[i]);
    set_status(p, 0, L"This folder uses the default view again");
}

static void p_set_sort(Pane *p, fpe_sort_key k)
{
    if (!p->view) return;
    int64_t t = fpe_prof_begin();
    if (p->opts.key == k) {
        p->opts.descending = !p->opts.descending;
    } else {
        p->opts.key = k;
        p->opts.descending = k == FPE_SORT_SIZE || k == FPE_SORT_MTIME || k == FPE_SORT_CTIME;
    }
    p_keep_cursor_after(p, apply_opts);
    folder_view_note(p, FV_SORT);
    FPE_PROF_END("pane.sort", t);
}

static void p_filter_changed(Pane *p)
{
    if (!p->view) return;
    int64_t t = fpe_prof_begin();
    p_keep_cursor_after(p, apply_filter);
    FPE_PROF_END("pane.filter", t);
}

/* ======================================================================== */
/* Jobs + clipboard                                                          */
/* ======================================================================== */

static ops_model g_operations;
typedef struct operation_request {
    struct operation_request *next;
    uint64_t token;
    wchar_t **sources, *dest, *tree_root;
    uint32_t count;
    int consume_cut;
} operation_request;
static operation_request *g_op_requests;
typedef struct archive_source_pin {struct archive_source_pin *next;uint64_t token;void (*release)(void *);void *owner;} archive_source_pin;
static archive_source_pin *g_archive_sources;
static void archive_sources_release(uint64_t token,int all){archive_source_pin **at=&g_archive_sources;while(*at){archive_source_pin *p=*at;if(all||p->token==token){*at=p->next;p->release(p->owner);free(p);}else at=&p->next;}}

/* Called only after engine destruction, outside the window/message path. */
static void archive_sources_finish(void){archive_sources_release(0,1);arc_cleanup_drain();}

static uint64_t g_op_sent_at;
static const wchar_t *operations_app(void)
{
    static wchar_t path[MAX_PATH];
    if (!path[0]) {
        GetModuleFileNameW(NULL, path, MAX_PATH);
        wchar_t *slash = wcsrchr(path, L'\\');
        if (slash) wcscpy(slash + 1, L"folderwing-operations.exe");
    }
    return path;
}
static void operations_send_one(const ops_entry *e)
{
    const wchar_t *shown = sidepanel_app();
    if (!shown || _wcsicmp(shown, operations_app()) || !sidepanel_running(operations_app())) return;
    uint32_t size;
    void *buf = ops_encode(e, &size);
    if (buf) sidepanel_send(operations_app(), PANELP_OP_UPDATE, buf, size);
    free(buf);
}
static void operations_send(void)
{
    if (!sidepanel_running(operations_app())) return;
    sidepanel_send(operations_app(), PANELP_OP_RESET, NULL, 0);
    for (uint32_t i = 0; i < g_operations.n; i++) operations_send_one(g_operations.entries + i);
    uint64_t token = g_operations.n ? g_operations.entries[g_operations.n - 1].h.token : 0;
    sidepanel_send(operations_app(), PANELP_OP_SELECT, &token, sizeof token);
}
static void run_operations_show(fn_ctx *c)
{
    if (GetFileAttributesW(operations_app()) == INVALID_FILE_ATTRIBUTES ||
        sidepanel_toggle(g.hwnd, operations_app(), 0) < 0)
        app_status(NULL, 1, L"Operations could not open (folderwing-operations.exe is needed)");
    else operations_send();
}
static void on_job(void *user, const fpe_job_progress *p)
{
    size_t n = p->current ? wcslen(p->current) : 0;
    job_msg *m = calloc(1, sizeof *m + (n + 1) * sizeof(wchar_t));
    if (!m) return;
    m->p = *p;
    m->token = (uint64_t)(uintptr_t)user;
    if (n) memcpy(m->path, p->current, (n + 1) * sizeof(wchar_t));
    m->p.current = m->path;
    if (!PostMessageW(g.hwnd, WM_APP_JOB, 0, (LPARAM)m)) free(m);
}
static void on_job_event(void *user, const fpe_job_event *event)
{
    size_t n = event->path ? wcslen(event->path) : 0;
    job_msg *m = calloc(1, sizeof *m + (n + 1) * sizeof(wchar_t));
    if (!m) return;
    m->event = *event;
    m->token = (uint64_t)(uintptr_t)user;
    m->diagnostic = 1;
    if (n) memcpy(m->path, event->path, (n + 1) * sizeof(wchar_t));
    m->event.path = n ? m->path : NULL;
    if (!PostMessageW(g.hwnd, WM_APP_JOB, 0, (LPARAM)m)) free(m);
}
static void operation_request_free(operation_request *r)
{
    for (uint32_t i = 0; i < r->count; i++) free(r->sources[i]);
    free(r->sources); free(r->dest); free(r->tree_root); free(r);
}
static void operation_consume_cut(const operation_request *r)
{
    if (!r->consume_cut || !OpenClipboard(g.hwnd)) return;
    HDROP drop = (HDROP)GetClipboardData(CF_HDROP);
    HANDLE he = GetClipboardData(g.cf_effect);
    DWORD *effect = he ? GlobalLock(he) : NULL;
    int same = effect && (*effect & 2) && !(*effect & 1) && drop &&
               DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0) == r->count;
    if (effect) GlobalUnlock(he);
    for (uint32_t i = 0; same && i < r->count; i++) {
        UINT len = DragQueryFileW(drop, i, NULL, 0);
        wchar_t *path = malloc(((size_t)len + 1) * sizeof(wchar_t));
        same = path && DragQueryFileW(drop, i, path, len + 1) && same_path(path, r->sources[i]);
        free(path);
    }
    if (same) EmptyClipboard();
    CloseClipboard();
}
static void operation_start(ops_entry *e, operation_request *r, fpe_conflict policy)
{
    e->h.policy = policy;
    SetLastError(0);
    uint64_t id = r->tree_root ? fpe_job_submit_tree(g.engine,r->tree_root,(const wchar_t *const *)r->sources,r->count,r->dest,policy,on_job,on_job_event,(void *)(uintptr_t)e->h.token) : fpe_job_submit_ex(g.engine, (fpe_op)e->h.op, (const wchar_t *const *)r->sources,
        r->count, r->dest, policy, on_job, on_job_event, (void *)(uintptr_t)e->h.token);
    if (id) operation_consume_cut(r);
    e->h.job = id;
    e->h.started = GetTickCount64();
    e->h.state = id ? FPE_JOB_QUEUED : FPE_JOB_FAILED;
    if (!id) {
        DWORD error=GetLastError();if(!error)error=ERROR_NOT_ENOUGH_MEMORY;
        e->h.state = FPE_JOB_QUEUED;
        fpe_job_event v = { 0, FPE_JOB_ERROR, error, r->dest };
        ops_diagnostic(e, &v);
        e->h.state = FPE_JOB_FAILED;
        e->h.errors = 1; e->h.last_error = error; e->h.ended = GetTickCount64();
        app_status(NULL, 1, L"Could not queue the operation");
    }
    operations_send_one(e);
}
/* All app-originated jobs enter here, including clipboard and drop targets.
 * Transfers await an explicit conflict choice; delete/recycle already have
 * their applicable confirmation and start immediately. */
static int operation_submit_root(fpe_op op, const wchar_t *const *paths, uint32_t n, const wchar_t *dest, int consume_cut,const wchar_t *root)
{
    if (!n) return 0;
    const fn_def *d = fn_find("operations.show");
    if (!fn_active(d)) {
        if(root)return 0;
        uint64_t id = fpe_job_submit(g.engine, op, paths, n, dest, FPE_CONFLICT_RENAME, on_job, NULL);
        if (id && consume_cut) {
            operation_request clip = { .sources = (wchar_t **)paths, .count = n, .consume_cut = 1 };
            operation_consume_cut(&clip);
        }
        if (!id) app_status(NULL, 1, L"Could not queue the operation");
        return id != 0;
    }
    operation_request *r = calloc(1, sizeof *r);
    if (!r) { app_status(NULL, 1, L"Could not prepare the operation"); return 0; }
    r->sources = calloc(n, sizeof(wchar_t *));
    r->dest = wdup(dest ? dest : L"");
    r->tree_root=root?wdup(root):NULL;
    if (!r->sources || !r->dest || (root&&!r->tree_root)) { operation_request_free(r); app_status(NULL, 1, L"Could not prepare the operation"); return 0; }
    for (uint32_t i = 0; i < n; i++) {
        r->sources[i] = wdup(paths[i]);
        r->count++;
        if (!r->sources[i]) { operation_request_free(r); app_status(NULL, 1, L"Could not prepare the operation"); return 0; }
    }
    wchar_t *first=NULL;if(root){size_t cap=wcslen(root)+wcslen(paths[0])+2;first=malloc(cap*sizeof *first);if(first)swprintf(first,cap,L"%ls\\%ls",root,paths[0]);}
    if(root&&!first){operation_request_free(r);return 0;}
    ops_entry *e = ops_add(&g_operations, op, first?first:paths[0], n, dest);free(first);
    if (!e) { operation_request_free(r); app_status(NULL, 1, L"Too many pending operations, or not enough memory"); return 0; }
    if(root){
        size_t chars=64;for(uint32_t i=0;i<n;i++)chars+=wcslen(paths[i])+2;
        if(chars>OPS_DETAIL_CHARS){e->h.state=FPE_JOB_CANCELLED;e->h.ended=GetTickCount64();operation_request_free(r);return 0;}
        wchar_t *review=calloc(chars,sizeof *review);if(!review){e->h.state=FPE_JOB_CANCELLED;operation_request_free(r);return 0;}
        wcscpy(review,L"Preserve relative folders. Selected files:\r\n");
        size_t at=wcslen(review);for(uint32_t i=0;i<n;i++){size_t len=wcslen(paths[i]);memcpy(review+at,paths[i],len*sizeof *review);at+=len;review[at++]=L'\r';review[at++]=L'\n';}
        free(e->details);e->details=review;e->h.items_total=n;
    }
    r->token = e->h.token;
    r->consume_cut = consume_cut;
    const char *choice = fn_option(d, "conflicts");
    e->h.policy = !_stricmp(choice, "skip") ? FPE_CONFLICT_SKIP : !_stricmp(choice, "replace") ? FPE_CONFLICT_OVERWRITE : FPE_CONFLICT_RENAME;
    int transfer = op == FPE_OP_COPY || op == FPE_OP_MOVE;
    if (transfer) {
        if (GetFileAttributesW(operations_app()) == INVALID_FILE_ATTRIBUTES ||
            sidepanel_show(g.hwnd, operations_app(), 0, 1) < 0) {
            e->h.state = FPE_JOB_CANCELLED; e->h.ended = GetTickCount64();
            operation_request_free(r);
            app_status(NULL, 1, L"Transfer not started: Operations could not open"); return 0;
        }
        r->next = g_op_requests; g_op_requests = r;
        operations_send();
        _snwprintf(g.job_text, 255, L"%ls ready: choose conflicts and Start in Operations", ops_verb(op));
        main_status_redraw();
    } else {
        operation_start(e, r, FPE_CONFLICT_RENAME);
        operation_request_free(r);
        if (GetFileAttributesW(operations_app()) != INVALID_FILE_ATTRIBUTES &&
            sidepanel_show(g.hwnd, operations_app(), 0, 0) >= 0) operations_send();
    }
    return 1;
}
static int operation_submit(fpe_op op,const wchar_t *const *paths,uint32_t n,const wchar_t *dest,int consume_cut)
{return operation_submit_root(op,paths,n,dest,consume_cut,NULL);}
int app_stage_tree_copy(const wchar_t *root,const wchar_t *const *relative,uint32_t n,const wchar_t *dest)
{
    if(!fn_active(fn_find("operations.show"))){app_status(NULL,1,L"Enable Operations before staging comparison copies");return 0;}
    return operation_submit_root(FPE_OP_COPY,relative,n,dest,0,root);
}
int app_stage_copy(const wchar_t *const *paths,uint32_t n,const wchar_t *dest)
{
    if(!fn_active(fn_find("operations.show"))){app_status(NULL,1,L"Enable Operations before staging comparison copies");return 0;}
    return operation_submit(FPE_OP_COPY,paths,n,dest,0);
}
int app_stage_archive_copy(const wchar_t *source,const wchar_t *dest,void (*release)(void *),void *owner){
 archive_source_pin *pin=calloc(1,sizeof *pin);if(!pin)return 0;
 const wchar_t *paths[]={source};if(!app_stage_copy(paths,1,dest)){free(pin);return 0;}
 pin->token=g_op_requests->token;pin->release=release;pin->owner=owner;pin->next=g_archive_sources;g_archive_sources=pin;return 1;
}
static void operations_action(const ops_action *a)
{
    if (a->action == OPS_CLEAR) { ops_clear(&g_operations); operations_send(); return; }
    ops_entry *e = ops_find(&g_operations, a->token);
    if (!e || ops_finished(e->h.state)) return;
    if (a->action == OPS_START && (e->h.state != OPS_PENDING || a->policy > FPE_CONFLICT_OVERWRITE)) return;
    operation_request **at = &g_op_requests;
    while (*at && (*at)->token != a->token) at = &(*at)->next;
    if (e->h.state == OPS_PENDING && *at) {
        operation_request *r = *at;
        if (a->action == OPS_START) operation_start(e, r, (fpe_conflict)a->policy);
        else if (a->action == OPS_CANCEL) { e->h.state = FPE_JOB_CANCELLED; e->h.ended = GetTickCount64(); }
        else return;
        *at = r->next; operation_request_free(r); if(ops_finished(e->h.state))archive_sources_release(e->h.token,0);operations_send_one(e);
    } else if (a->action == OPS_CANCEL && e->h.job) {
        e->h.cancelling = 1;
        fpe_job_cancel(g.engine, e->h.job);
        operations_send_one(e);
    }
}

static void handle_job(job_msg *m)
{
    ops_entry *e = ops_find(&g_operations, m->token);
    if (m->diagnostic) {
        ops_diagnostic(e, &m->event);
        free(m);
        return;
    }
    const fpe_job_progress *p = &m->p;
    ops_update(e, p);
    if(ops_finished(p->state))archive_sources_release(m->token,0);
    uint64_t now = GetTickCount64();
    if (e && (ops_finished(p->state) || now - g_op_sent_at >= 150)) {
        operations_send_one(e);
        g_op_sent_at = now;
    }
    static const wchar_t *verbs[] = { L"Copying", L"Moving", L"Deleting", L"Recycling" };
    wchar_t done[32], total[32];
    fmt_size(done, 32, p->bytes_done);
    fmt_size(total, 32, p->bytes_total);
    switch (p->state) {
    case FPE_JOB_SCANNING:
        _snwprintf(g.job_text, 255, L"%ls: counting %u items", verbs[p->op], p->items_total);
        break;
    case FPE_JOB_RUNNING:
        if (p->bytes_total)
            _snwprintf(g.job_text, 255, L"%ls %u/%u  %ls / %ls", verbs[p->op], p->items_done, p->items_total, done, total);
        else
            _snwprintf(g.job_text, 255, L"%ls %u/%u", verbs[p->op], p->items_done, p->items_total);
        break;
    case FPE_JOB_DONE:
        if (p->errors) _snwprintf(g.job_text, 255, L"%ls finished with %u error(s)", verbs[p->op], p->errors);
        else _snwprintf(g.job_text, 255, L"%ls done (%u items, %u skipped)", verbs[p->op], p->items_done, p->skipped);
        SetTimer(g.hwnd, TIMER_JOBTEXT, 4000, NULL);
        break;
    case FPE_JOB_CANCELLED:
        _snwprintf(g.job_text, 255, L"%ls cancelled", verbs[p->op]);
        SetTimer(g.hwnd, TIMER_JOBTEXT, 4000, NULL);
        break;
    case FPE_JOB_FAILED: {
        wchar_t e[200];
        win_err(e, 200, p->last_error);
        _snwprintf(g.job_text, 255, L"%ls failed: %ls", verbs[p->op], e);
        SetTimer(g.hwnd, TIMER_JOBTEXT, 6000, NULL);
        break;
    }
    default: break;
    }
    g.job_text[255] = 0;
    free(m);
    main_status_redraw();
}

static struct {
    uint64_t request, until;
    HWND owner;
    DWORD sequence;
    UINT format, extra_format;
    HGLOBAL data, extra;
    wchar_t success[160];
} g_clipboard;

void app_clipboard_cancel(HWND owner)
{
    if (owner && owner != g_clipboard.owner) return;
    KillTimer(g.hwnd, TIMER_CLIPBOARD);
    if (g_clipboard.data) GlobalFree(g_clipboard.data);
    if (g_clipboard.extra) GlobalFree(g_clipboard.extra);
    g_clipboard.data = g_clipboard.extra = NULL;
    g_clipboard.owner = NULL;
    ++g_clipboard.request; /* also invalidates results still being prepared */
}

uint64_t app_clipboard_begin(HWND owner)
{
    app_clipboard_cancel(NULL);
    g_clipboard.owner = owner;
    g_clipboard.sequence = GetClipboardSequenceNumber();
    return g_clipboard.request;
}

static void clipboard_publish(void)
{
    if (!g_clipboard.data) return;
    const wchar_t *error = NULL;
    if (!IsWindow(g_clipboard.owner)) error = L"Copy cancelled because its window closed";
    else if (GetClipboardSequenceNumber() != g_clipboard.sequence)
        error = L"Copy cancelled because the clipboard changed";
    else if (GetTickCount64() >= g_clipboard.until)
        error = L"The clipboard is busy; try copying again";
    else if (OpenClipboard(g_clipboard.owner)) {
        /* Check again with the clipboard locked: another publisher may have
         * won between the first check and OpenClipboard. */
        if (GetClipboardSequenceNumber() != g_clipboard.sequence)
            error = L"Copy cancelled because the clipboard changed";
        else if (!EmptyClipboard()) error = L"The clipboard could not be cleared";
        else {
            int ok = SetClipboardData(g_clipboard.format, g_clipboard.data) != NULL;
            if (ok) g_clipboard.data = NULL; /* Windows owns only accepted handles */
            if (ok && g_clipboard.extra) {
                ok = SetClipboardData(g_clipboard.extra_format, g_clipboard.extra) != NULL;
                if (ok) g_clipboard.extra = NULL;
            }
            if (!ok) {
                EmptyClipboard(); /* do not leave a cut without its move effect */
                error = L"The item could not be copied to the clipboard";
            }
        }
        CloseClipboard();
        if (!error) {
            app_status(NULL, 0, L"%ls", g_clipboard.success);
            app_clipboard_cancel(NULL);
            return;
        }
    } else if (!SetTimer(g.hwnd, TIMER_CLIPBOARD, 40, NULL))
        error = L"Clipboard retry could not be scheduled; try copying again";
    if (error) {
        app_clipboard_cancel(NULL);
        app_status(NULL, 1, L"%ls", error);
    }
}

void app_clipboard_submit(uint64_t request, UINT format, HGLOBAL data,
                          UINT extra_format, HGLOBAL extra, const wchar_t *success)
{
    if (request != g_clipboard.request || !g_clipboard.owner) {
        if (data) GlobalFree(data);
        if (extra) GlobalFree(extra);
        return;
    }
    if (!data || (extra_format && !extra)) {
        if (data) GlobalFree(data);
        if (extra) GlobalFree(extra);
        app_clipboard_cancel(NULL);
        app_status(NULL, 1, L"Not enough memory to copy to the clipboard");
        return;
    }
    g_clipboard.data = data; g_clipboard.extra = extra;
    g_clipboard.format = format; g_clipboard.extra_format = extra_format;
    g_clipboard.until = GetTickCount64() + 1500;
    wcsncpy(g_clipboard.success, success, 159); g_clipboard.success[159] = 0;
    app_status(NULL, 0, L"Copying to the clipboard...");
    clipboard_publish();
}

static void clip_set(Pane *pn, wchar_t *const *v, uint32_t n, int cut)
{
    if (!n) return;
    uint64_t request = app_clipboard_begin(g.hwnd);
    size_t chars = 1;
    for (uint32_t i = 0; i < n; i++) chars += wcslen(v[i]) + 1;

    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + chars * sizeof(wchar_t));
    HGLOBAL he = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (h && he) {
        DROPFILES *df = GlobalLock(h);
        DWORD *eff = GlobalLock(he);
        if (!df || !eff) {
            if (df) GlobalUnlock(h);
            if (eff) GlobalUnlock(he);
            GlobalFree(h); GlobalFree(he);
            app_clipboard_submit(request, CF_HDROP, NULL, 0, NULL, L"");
            return;
        }
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        wchar_t *p = (wchar_t *)(df + 1);
        for (uint32_t i = 0; i < n; i++) {
            size_t l = wcslen(v[i]);
            memcpy(p, v[i], l * sizeof(wchar_t));
            p += l + 1;
        }
        GlobalUnlock(h);
        *eff = cut ? 2 /* DROPEFFECT_MOVE */ : 5 /* DROPEFFECT_COPY | DROPEFFECT_LINK */;
        GlobalUnlock(he);

    }
    wchar_t success[80];
    _snwprintf(success, 80, L"%ls %u item(s)", cut ? L"Cut" : L"Copied", n);
    app_clipboard_submit(request, CF_HDROP, h, g.cf_effect, he, success);
}

static void clip_paste_into(const wchar_t *dest)
{
    if (!dest || !OpenClipboard(g.hwnd)) return;
    DWORD effect = 1;
    HANDLE he = GetClipboardData(g.cf_effect);
    if (he) {
        DWORD *p = GlobalLock(he);
        if (p) { effect = *p; GlobalUnlock(he); }
    }
    HDROP drop = (HDROP)GetClipboardData(CF_HDROP);
    UINT n = drop ? DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0) : 0;
    wchar_t **v = n ? calloc(n, sizeof *v) : NULL;
    UINT k = 0;
    for (UINT i = 0; v && i < n; i++) {
        UINT len = DragQueryFileW(drop, i, NULL, 0);
        wchar_t *s = malloc((len + 1) * sizeof(wchar_t));
        if (s && DragQueryFileW(drop, i, s, len + 1)) v[k++] = s;
        else free(s);
    }
    int move = (effect & 2) && !(effect & 1);
    CloseClipboard();

    if (k) operation_submit(move ? FPE_OP_MOVE : FPE_OP_COPY, (const wchar_t *const *)v, k, dest, move);
    for (UINT i = 0; i < k; i++) free(v[i]);
    free(v);
}

static void delete_paths(wchar_t *const *v, uint32_t n, int permanent)
{
    if (!n) return;
    if (permanent) {
        wchar_t q[600];
        if (n == 1) _snwprintf(q, 600, L"Permanently delete \"%ls\"?\n\nThis cannot be undone.", v[0]);
        else _snwprintf(q, 600, L"Permanently delete %u items?\n\nThis cannot be undone.", n);
        q[599] = 0;
        if (modal_box(q, L"Delete", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    }
    operation_submit(permanent ? FPE_OP_DELETE : FPE_OP_RECYCLE, (const wchar_t *const *)v, n, NULL, 0);
}

/* F5 / F6: copy or move the selection into the folder shown in the other pane. */
static void p_transfer(Pane *p, int move)
{
    Pane *o = other_pane(p);
    if (!g.dual || !o->path || !p->path) return;
    if (same_path(p->path, o->path)) {
        set_status(p, 1, L"Both panes show the same folder");
        return;
    }
    wchar_t **v;
    uint32_t n = p_targets(p, &v, NULL);
    if (!n) return;
    if (!fn_active(fn_find("operations.show"))) {
        wchar_t question[1200];
        _snwprintf(question, 1200, L"%ls %u item(s) to\n%ls ?", move ? L"Move" : L"Copy", n, o->path);
        question[1199] = 0;
        if (modal_box(question, move ? L"Move" : L"Copy", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
            free_targets(v, n); return;
        }
    }
    operation_submit(move ? FPE_OP_MOVE : FPE_OP_COPY, (const wchar_t *const *)v, n, o->path, 0);
    free_targets(v, n);
}

/* ======================================================================== */
/* Pane painting + input                                                     */
/* ======================================================================== */

typedef struct cols {
    int icon_x, name_x, name_w, type_x, type_w, size_x, size_w, date_x, date_w;
    int perm_x, perm_w, own_x, own_w; /* Linux folders (WSL) only */
} cols;

static int p_linux(const Pane *p) { return p->view && fpe_dir_is_linux(fpe_view_dir(p->view)); }

/* Linux names keep characters Windows names can't hold (":", "?", tabs...)
 * as U+F000 + the character, which fonts draw as boxes. On screen they are
 * shown as themselves, control characters as their symbols (U+2400 + c). */
static const wchar_t *shown_name(const wchar_t *name,int n,wchar_t *buf,int cap,int linux)
{
    (void)n;if(!linux)return name;
    fpe_lx_display(name,buf,(size_t)cap,1);return buf;
}

/* Type column of Linux special files; NULL for ordinary ones */
static const wchar_t *linux_kind(const fpe_entry *e)
{
    if (!(e->attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return NULL;
    switch (e->reparse_tag) {
    case 0x80000023u: return L"Socket";
    case 0x80000024u: return L"Pipe";
    case 0x80000025u: return L"Device";
    case 0x80000026u: return L"Device";
    default: return NULL;
    }
}

/* Narrow panes (dual mode on small windows) drop Owner and Permissions,
 * then Type, then Modified. */
static cols p_columns(const Pane *p)
{
    cols c;
    int w = p->cw;
    c.date_w = w >= S(430) ? S(140) : 0;
    c.type_w = w >= S(580) ? S(70) : 0;
    c.size_w = S(80);
    int lx = g.wsl_columns && w >= S(680) && p_linux(p);
    c.perm_w = lx ? S(84) : 0;
    c.own_w = lx ? S(76) : 0;
    c.icon_x = g.pad + S(4);
    c.name_x = c.icon_x + S(22);
    int x = w - g.pad;
    if (c.date_w) { x -= c.date_w; c.date_x = x; x -= S(10); }
    else c.date_x = w;
    x -= c.size_w;
    c.size_x = x;
    x -= S(10);
    if (c.type_w) { x -= c.type_w; c.type_x = x; x -= S(10); }
    else c.type_x = c.size_x;
    if (c.own_w) { x -= c.own_w; c.own_x = x; x -= S(10); }
    else c.own_x = c.type_x;
    if (c.perm_w) { x -= c.perm_w; c.perm_x = x; x -= S(10); }
    else c.perm_x = c.own_x;
    c.name_w = x - c.name_x;
    if (c.name_w < S(60)) c.name_w = S(60);
    return c;
}

/* The path as crumbs: "C:" > "Users" > "Rose". When they don't fit, the
 * leading ones fold into "...", which opens the editor like the empty space
 * after the last crumb. */
static void p_paint_crumbs(Pane *p, HDC dc, int is_active)
{
    const wchar_t *path = p->path;
    if (!path) return;
    int n = (int)wcslen(path), ns = 0, i = 0;
    int s0[CRUMBS_MAX], s1[CRUMBS_MAX], plen[CRUMBS_MAX]; /* text [s0, s1); the path up to plen chars */
    int linux=p->address_distro[0]!=0;
    if(linux) {const wchar_t *root=wcschr(path+(!_wcsnicmp(path,L"\\\\wsl$\\",7)?7:16),L'\\');int e=root?(int)(root-path):n;s0[0]=s1[0]=0;plen[0]=e;ns=1;i=e+1;}
    else if (n >= 2 && path[1] == L':') {
        s0[0] = 0, s1[0] = 2, plen[0] = n >= 3 ? 3 : 2, ns = 1, i = 3;
    } else if (n > 2 && path[0] == L'\\' && path[1] == L'\\') { /* \\server\share is one crumb */
        const wchar_t *a = wcschr(path + 2, L'\\'), *b = a ? wcschr(a + 1, L'\\') : NULL;
        int e = b ? (int)(b - path) : n;
        s0[0] = 0, s1[0] = e, plen[0] = e, ns = 1, i = e + 1;
    }
    while (i < n && ns < CRUMBS_MAX) {
        int j = i;
        while (j < n && path[j] != L'\\') j++;
        if (j > i) { s0[ns] = i; s1[ns] = j; plen[ns] = j; ns++; }
        i = j + 1;
    }
    if (!ns) return;

    static const wchar_t sep[] = L"  \x203A  ", ell[] = L"\x2026";
    SIZE sz;
    GetTextExtentPoint32W(dc, sep, 5, &sz);
    int sepw = sz.cx, w[CRUMBS_MAX], total = 0;
    GetTextExtentPoint32W(dc, ell, 1, &sz);
    int ellw = sz.cx;
    for (int k = 0; k < ns; k++) {
        GetTextExtentPoint32W(dc,linux&&k==0?L"/":path+s0[k],linux&&k==0?1:s1[k]-s0[k],&sz);
        w[k] = sz.cx;
        total += sz.cx + (k ? sepw : 0);
    }
    int x = g.pad + S(8)+p_distro_width(p), avail = p->cw - 2 * g.pad - S(24)-p_distro_width(p), first = 0;
    while (total > avail && first < ns - 1) {
        total -= w[first] + sepw;
        if (!first) total += ellw + sepw;
        first++;
    }
    int y = p_addr_y(p);
    if (first) {
        p->crumbs[p->ncrumbs++] = (crumb){ x, x + ellw, 0 };
        text(dc, ell, 1, x, y, ellw, g.edit_h, C_DIM, DT_LEFT);
        x += ellw;
        text(dc, sep, 5, x, y, sepw, g.edit_h, C_DIM, DT_LEFT);
        x += sepw;
    }
    for (int k = first; k < ns && p->ncrumbs < CRUMBS_MAX; k++) {
        int last = k == ns - 1, cw = w[k] < avail ? w[k] : avail;
        p->crumbs[p->ncrumbs++] = (crumb){ x, x + cw, plen[k] };
        text(dc, linux&&k==0?L"/":path+s0[k], linux&&k==0?1:s1[k]-s0[k], x, y, cw, g.edit_h, last && is_active ? C_TEXT : C_DIM, DT_LEFT);
        x += cw;
        if (!last) {
            text(dc, sep, 5, x, y, sepw, g.edit_h, C_DIM, DT_LEFT);
            x += sepw;
        }
    }
}

/* The crumb under client x, or -1. */
static int crumb_at(const Pane *p, int x)
{
    for (int k = 0; k < p->ncrumbs; k++)
        if (x >= p->crumbs[k].x0 - S(4) && x < p->crumbs[k].x1 + S(4)) return k;
    return -1;
}

/* The folder a crumb stands for (owned), or NULL for "..." and the current folder. */
static wchar_t *crumb_path(const Pane *p, int k, wchar_t *child, size_t child_cap)
{
    if (k < 0 || !p->path || !p->crumbs[k].len) return NULL;
    size_t len = (size_t)p->crumbs[k].len, n = wcslen(p->path);
    if (len >= n) return NULL;
    wchar_t *s = malloc((len + 1) * sizeof(wchar_t));
    if (!s) return NULL;
    memcpy(s, p->path, len * sizeof(wchar_t));
    s[len] = 0;
    if (child) { /* the folder we come from, to select it */
        const wchar_t *c = p->path + len + (p->path[len] == L'\\');
        const wchar_t *e = wcschr(c, L'\\');
        size_t cl = e ? (size_t)(e - c) : wcslen(c);
        if (cl >= child_cap) cl = child_cap - 1;
        memcpy(child, c, cl * sizeof(wchar_t));
        child[cl] = 0;
    }
    return s;
}

static void p_paint_tiles(Pane *p, HDC dc, int focused, int is_active)
{
    thumb_grid grid = p_grid(p);
    int edge = S(p->thumb_size);
    if (edge > THUMB_EDGE_MAX)
        edge = THUMB_EDGE_MAX;
    if(g_tab_drag.started && g_tab_drag.target==p && p_tabs_h(p)){
        int x=g.pad+g_tab_drag.slot*tab_width(p);if(x>=p->cw-S(3))x=p->cw-S(3);
        fill(dc,x,S(2),S(3),g.tab_h-S(2),C_ACCENT);
    }
    int y0 = p_list_top(p), y1 = p_list_bottom(p);
    HDC pixels = CreateCompatibleDC(dc);
    int checker_edge = 2 * S(8);
    void *checker_pixels = NULL;
    BITMAPINFO checker_info = {
        .bmiHeader = {sizeof(BITMAPINFOHEADER), checker_edge, -checker_edge, 1, 32, BI_RGB}};
    HBITMAP checker_bitmap = CreateDIBSection(NULL, &checker_info, DIB_RGB_COLORS, &checker_pixels, NULL, 0);
    HBRUSH checker = NULL;
    if (checker_bitmap) {
        for (int yy = 0; yy < checker_edge; ++yy)
            for (int xx = 0; xx < checker_edge; ++xx)
                ((uint32_t *)checker_pixels)[yy * checker_edge + xx] =
                    ((xx / S(8) + yy / S(8)) & 1) ? 0x004A4E54 : 0x00363A40;
        checker = CreatePatternBrush(checker_bitmap);
        DeleteObject(checker_bitmap);
    }
    for (uint32_t r = (uint32_t)p->top; r < p_count(p); ++r) {
        int pos = (int)r - p->top, x = (pos % grid.columns) * grid.cell_w,
            y = y0 + (pos / grid.columns) * grid.cell_h;
        if(y>=y1)break;
        if(!RectVisible(dc,&(RECT){x,y,x+grid.cell_w,y+grid.cell_h}))continue;
        const fpe_entry *e = fpe_view_at(p->view, r);
        const wchar_t *name = fpe_view_name(p->view, r);
        int w = grid.cell_w - S(4), h = grid.cell_h - S(4);
        COLORREF tc = (e->attrs & FILE_ATTRIBUTE_HIDDEN) ? C_DIM : C_TEXT;
        if (p_is_sel(p, r))
            fill(dc, x + S(2), y + S(2), w, h, is_active ? C_SEL : C_SEL_IDLE);
        if (r == p->cursor && focused)
            frame(dc, x + S(2), y + S(2), w, h, C_CURSOR);
        if ((int)r == p->drop_row) {
            fill(dc, x + S(2), y + S(2), w, h, C_SEL);
            frame(dc, x + S(2), y + S(2), w, h, C_ACCENT);
        }
        wchar_t gitmark[4];if(g_git_badges&&git_badge(&g_git_model,p->path,name,fpe_entry_is_dir(e),gitmark))text(dc,gitmark,-1,x+S(6),y+S(3),S(32),S(19),C_ACCENT,DT_LEFT);
        int drawn = 0;
        wchar_t *path = !fpe_entry_is_dir(e) ? join(p->path, name) : NULL;
        thumb_entry *t = path ? thumbnails_get(path, e->size, e->mtime, (uint32_t)edge) : NULL;
        free(path);
        if (t && t->data && pixels) {
            int tx = x + (grid.cell_w - (int)t->w) / 2, ty = y + S(8) + (edge - (int)t->h) / 2;
            /* Transparency stays visible over a neutral checkerboard. */
            if (!is_pdf(e, name) && checker) {
                HGDIOBJ old_brush = SelectObject(dc, checker);
                POINT origin;
                SetBrushOrgEx(dc, tx, ty, &origin);
                PatBlt(dc, tx, ty, (int)t->w, (int)t->h, PATCOPY);
                SetBrushOrgEx(dc, origin.x, origin.y, NULL);
                SelectObject(dc, old_brush);
            }
            HGDIOBJ old = SelectObject(pixels, (HBITMAP)t->data);
            BLENDFUNCTION blend = {AC_SRC_OVER, 0, (BYTE)((e->attrs & FILE_ATTRIBUTE_HIDDEN) ? 130 : 255),
                                   AC_SRC_ALPHA};
            AlphaBlend(dc, tx, ty, (int)t->w, (int)t->h, pixels, 0, 0, (int)t->w, (int)t->h, blend);
            SelectObject(pixels, old);
            drawn = 1;
        }
        if (!drawn) {
            int sz = S(42), slot = (grid.cell_w - sz) / 2;
            int link = (e->attrs & FILE_ATTRIBUTE_REPARSE_POINT) && (e->reparse_tag & 0x20000000u);
            ti_kind kind = ti_of_name(name, e->name_len, e->ext, e->attrs);
            int icon = shell_icon_for(kind) ? shell_icon_entry(p->path, name, e->name_len, e->ext, e->attrs) : -1;
            if (icon >= 0) {
                int own = shell_icon_size();
                shell_icon_draw(dc, icon, x + (grid.cell_w - own) / 2, y + S(8) + (edge - own) / 2,
                                (e->attrs & FILE_ATTRIBUTE_HIDDEN) != 0);
            } else {
                ti_draw(dc, kind, x + slot, y + S(8) + (edge - sz) / 2, sz,
                        ((e->attrs & FILE_ATTRIBUTE_HIDDEN) ? TI_FADED : 0) | (link ? TI_LINK : 0), 0);
            }
        }
        wchar_t shown[260];
        RECT label = {x + S(8), y + S(8) + edge, x + grid.cell_w - S(8), y + grid.cell_h - S(4)};
        SetTextColor(dc, tc);
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, shown_name(name,e->name_len,shown,260,p_linux(p)), -1, &label,
                  DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    if (checker)
        DeleteObject(checker);
    if (pixels)
        DeleteDC(pixels);
}

static void p_paint(Pane *p, HDC dc)
{
    SelectObject(dc, g.font);
    cols c = p_columns(p);
    int is_active = p->idx == g.active;
    int focused = GetFocus() == p->hwnd;
    fill(dc, 0, 0, p->cw, p->ch, C_BG);

    RECT damage;GetClipBox(dc,&damage);
    int hy=p_header_y(p);
    if(damage.top<p_list_top(p)) {
    if (p->ntabs > 1) p_paint_tabs(p, dc);
    /* address bar backdrop (the edit control sits on top of it while typing) */
    fill(dc, g.pad, p_addr_y(p), p->cw - 2 * g.pad, g.edit_h, C_EDIT);
    if (g.dual && is_active && p->ntabs < 2) fill(dc, 0, 0, p->cw, S(2), C_ACCENT);
    if(damage.top<p_addr_y(p)+g.edit_h&&damage.bottom>p_addr_y(p)){
        p->ncrumbs=0;
        if(g.crumbs&&!IsWindowVisible(p->edit))p_paint_crumbs(p,dc,is_active);
    }

    fill(dc, 0, hy, p->cw, g.header_h, C_HEADER);
    static const struct { fpe_sort_key k; const wchar_t *label; } hdr[] = {
        { FPE_SORT_NAME, L"Name" }, { FPE_SORT_EXT, L"Type" }, { FPE_SORT_SIZE, L"Size" }, { FPE_SORT_MTIME, L"Modified" },
    };
    int hx[] = { c.name_x, c.type_x, c.size_x, c.date_x }, hw[] = { c.name_w, c.type_w, c.size_w, c.date_w };
    if (c.perm_w) {
        text(dc, L"Permissions", -1, c.perm_x, hy, c.perm_w, g.header_h, C_DIM, DT_LEFT);
        text(dc, L"Owner", -1, c.own_x, hy, c.own_w, g.header_h, C_DIM, DT_LEFT);
    }
    for (int i = 0; i < 4; i++) {
        if (!hw[i]) continue;
        wchar_t l[32];
        int on = p->opts.key == hdr[i].k;
        _snwprintf(l, 32, L"%ls%ls", hdr[i].label, on ? (p->opts.descending ? L" \x25BE" : L" \x25B4") : L"");
        text(dc, l, -1, hx[i], hy, hw[i], g.header_h, on ? C_TEXT : C_DIM, i == 2 ? DT_RIGHT : DT_LEFT);
    }

    }
    int y0 = p_list_top(p), y1 = p_list_bottom(p);
    uint32_t n = p_count(p);
    RECT dirty;GetClipBox(dc,&dirty);
    int list_clip=SaveDC(dc);IntersectClipRect(dc,0,y0,p->cw,y1);
    if(p->thumbnails){if(dirty.top<y1&&dirty.bottom>y0)p_paint_tiles(p,dc,focused,is_active);}
    else for (uint32_t r=(uint32_t)(p->top+(dirty.top>y0?(dirty.top-y0)/g.row_h:0));r<n;r++) {
        int y=y0+((int)r-p->top)*g.row_h;
        if(y>=y1||y>=dirty.bottom)break;
        FPE_PROF_COUNT("paint.pane.rows");
        const fpe_entry *e = fpe_view_at(p->view, r);
        const wchar_t *name = fpe_view_name(p->view, r);
        if (p_is_sel(p, r)) fill(dc, 0, y, p->cw, g.row_h, is_active ? C_SEL : C_SEL_IDLE);
        if (r == p->cursor && focused) frame(dc, S(2), y, p->cw - S(4), g.row_h, C_CURSOR);
        if ((int)r == p->drop_row) {
            fill(dc, 0, y, p->cw, g.row_h, C_SEL);
            frame(dc, S(1), y, p->cw - S(2), g.row_h, C_ACCENT);
        }

        COLORREF tc = (e->attrs & FILE_ATTRIBUTE_HIDDEN) ? C_DIM : C_TEXT;
        draw_entry_icon(dc, c.icon_x, y, e, p->path, name);
        wchar_t shown[260];
        wchar_t gitmark[4];int marked=g_git_badges&&git_badge(&g_git_model,p->path,name,fpe_entry_is_dir(e),gitmark);int badge_w=marked?S(28):0;
        if(marked)text(dc,gitmark,-1,c.name_x,y,badge_w,g.row_h,C_ACCENT,DT_LEFT);
        text(dc, shown_name(name,e->name_len,shown,260,p_linux(p)), -1, c.name_x+badge_w, y, c.name_w-badge_w, g.row_h, tc, DT_LEFT);

        wchar_t buf[64];
        if (fpe_entry_is_dir(e)) {
            text(dc, L"Folder", -1, c.type_x, y, c.type_w, g.row_h, C_DIM, DT_LEFT);
        } else {
            int el = e->name_len - e->ext;
            const wchar_t *kind = linux_kind(e);
            if (c.type_w) {
                if (kind) {
                    text(dc, kind, -1, c.type_x, y, c.type_w, g.row_h, C_DIM, DT_LEFT);
                } else if (el > 0 && el < 12) {
                    for (int i = 0; i < el; i++) buf[i] = towupper(name[e->ext + i]);
                    text(dc, buf, el, c.type_x, y, c.type_w, g.row_h, C_DIM, DT_LEFT);
                } else {
                    text(dc, L"File", -1, c.type_x, y, c.type_w, g.row_h, C_DIM, DT_LEFT);
                }
            }
            fmt_size(buf, 64, e->size);
            text(dc, buf, -1, c.size_x, y, c.size_w, g.row_h, C_DIM, DT_RIGHT);
        }
        if (c.date_w) {
            fmt_time(buf, 64, e->mtime);
            text(dc, buf, -1, c.date_x, y, c.date_w, g.row_h, C_DIM, DT_LEFT);
        }
        fpe_unix u;
        if (c.perm_w && fpe_dir_unix(fpe_view_dir(p->view), e, &u)) {
            fpe_unix_mode_string(u.mode, buf);
            text(dc, buf, 10, c.perm_x, y, c.perm_w, g.row_h, C_DIM, DT_LEFT);
            if (u.owner) text(dc, u.owner, -1, c.own_x, y, c.own_w, g.row_h, C_DIM, DT_LEFT);
            else _snwprintf(buf, 64, L"%u", u.uid), text(dc, buf, -1, c.own_x, y, c.own_w, g.row_h, C_DIM, DT_LEFT);
        }
    }
    if (!n && p->view) {
        text(dc, p->filter_len ? L"No items match the filter" : L"This folder is empty", -1, 0, y0 + S(20), p->cw,
             g.row_h, C_DIM, DT_CENTER);
    }
    RestoreDC(dc,list_clip);
    if (p->drop_row == -2 || (g_tab_drag.started && g_tab_drag.target==p && !p_tabs_h(p))) { /* a drag would drop into this pane's folder */
        frame(dc, 0, y0, p->cw, y1 - y0, C_ACCENT);
        frame(dc, 1, y0 + 1, p->cw - 2, y1 - y0 - 2, C_ACCENT);
    }

    /* pane footer */
    int sy = p->ch - g.status_h;
    fill(dc, 0, sy, p->cw, g.status_h, C_STATUS);
    if(damage.bottom>sy){
    wchar_t left[700];
    int k = _snwprintf(left, 700, L"%u items", n);
    if (p->nsel) k += _snwprintf(left + k, 700 - k, L"   %u selected", p->nsel);
    if (p->loading) k += _snwprintf(left + k, 700 - k, L"   Loading\x2026");
    if (p->status[0]) k += _snwprintf(left + k, 700 - k, L"   %ls", p->status);
    fpe_unix u;
    if (!p->status[0] && p->cursor < n && fpe_dir_unix(fpe_view_dir(p->view), fpe_view_at(p->view, p->cursor), &u) &&
        u.link)
        k += _snwprintf(left + k, 700 - k, L"   \x2192 %.300ls", u.link);
    left[699] = 0;
    text(dc, left, -1, g.pad * 2, sy, p->cw - g.pad * 4 - S(90), g.status_h, p->status_err ? C_ERROR : C_DIM, DT_LEFT);
    if (p->view) {
        fpe_dir_stats st;
        fpe_dir_get_stats(fpe_view_dir(p->view), &st);
        wchar_t rt[32];
        _snwprintf(rt, 32, L"%ls %.1f ms", p_linux(p) ? L"Linux" : L"read", st.read_us / 1000.0);
        text(dc, rt, -1, p->cw - g.pad * 2 - S(90), sy, S(90), g.status_h, C_DIM, DT_RIGHT);
    }

    }
    if (p->filter_len&&damage.top<hy+g.header_h&&damage.bottom>hy) {
        wchar_t f[300];
        _snwprintf(f, 300, L"  filter: %ls  ", p->filter);
        f[299] = 0;
        SIZE sz;
        GetTextExtentPoint32W(dc, f, (int)wcslen(f), &sz);
        int fx = p->cw - g.pad - sz.cx - S(8), fy = hy + (g.header_h - sz.cy) / 2 - S(1);
        fill(dc, fx, fy, sz.cx, sz.cy + S(2), C_SEL);
        text(dc, f, -1, fx, fy, sz.cx, sz.cy + S(2), C_TEXT, DT_LEFT);
    }
}

static int p_row_at(Pane *p, int x, int y)
{
    if (y < p_list_top(p) || y >= p_list_bottom(p)) return -1;
    if(p->thumbnails)return thumb_grid_hit(p_grid(p),p_count(p),p->top,x,y-p_list_top(p));
    int r = p->top + (y - p_list_top(p)) / g.row_h;
    return r < (int)p_count(p) ? r : -1;
}

static void p_focus_address(Pane *p)
{
    if(p->address_resolving) {p_address_cancel(p);p->loading=0;KillTimer(p->hwnd,TIMER_LOADING);set_status(p,0,L"Linux path resolution cancelled");}
    p->address_editing=1;p_address_text(p,1);
    ShowWindow(p->edit, SW_SHOW);
    focus(p->edit);
    SendMessageW(p->edit, EM_SETSEL, 0, -1);
}

/* With breadcrumbs the editor is only up while typing. */
static void p_end_address(Pane *p)
{
    p->address_editing=0;
    if (!g.crumbs || !IsWindowVisible(p->edit)) return;
    ShowWindow(p->edit, SW_HIDE);
    p_address_text(p,0);
    p_redraw(p);
}

/* ---- rename + new items ------------------------------------------------------------ */

/* Renames and creations run on the thread pool: on a slow share they can
 * take seconds, and the UI thread never waits on the filesystem. */
enum { FSOP_RENAME, FSOP_NEW_FOLDER, FSOP_NEW_FILE, FSOP_SHORTCUT, FSOP_SYMLINK };

typedef struct fsop {
    int           kind, pane;
    const fn_def *held; /* the function that asked: it keeps running until this is done */
    wchar_t      *dir, *from, *to;
    wchar_t       name[260]; /* the resulting name */
    wchar_t       want[260]; /* FSOP_NEW_FILE: the name asked for (empty: New file.txt) */
    wchar_t      *tmpl;      /* FSOP_NEW_FILE: copied, else the file is empty */
    DWORD         err;
    wchar_t     **sources;
    uint32_t      count, completed;
    int           relative;
} fsop;

static const char *const fsop_owner[] = { "file.rename", "file.new_folder", "file.new_file", "file.shortcut", "file.symlink" };

static void fsop_free(fsop *o)
{
    if (o->held) fn_release(o->held);
    free(o->dir);
    free(o->from);
    free(o->to);
    free(o->tmpl);
    for(uint32_t i=0;i<o->count;++i)free(o->sources[i]);
    free(o->sources);
    free(o);
}

static void CALLBACK fsop_work(PTP_CALLBACK_INSTANCE inst, void *arg)
{
    fsop *o = arg;
    int64_t t = fpe_prof_begin();
    if(o->kind==FSOP_SYMLINK){
        for(uint32_t k=0;k<o->count;++k){
            const wchar_t *name=o->count==1?o->want:path_label(o->sources[k]);
            wchar_t *full=join(o->dir,name);
            DWORD error=full?fpe_wsl_symlink(o->sources[k],full,o->relative):ERROR_NOT_ENOUGH_MEMORY;
            if(!error){o->completed++;wcsncpy(o->name,name,259);o->name[259]=0;}
            else if(!o->err){o->err=error;o->from=full?wdup(full):NULL;}
            free(full);
        }
    }else if(o->kind==FSOP_SHORTCUT){
        for(uint32_t k=0;k<o->count;++k){
            const wchar_t *base=path_label(o->sources[k]);wchar_t name[260];DWORD err=ERROR_ALREADY_EXISTS;
            for(int index=1;index<1000 && (err==ERROR_ALREADY_EXISTS || err==ERROR_FILE_EXISTS);++index){
                if(index==1)_snwprintf(name,260,L"%.220ls - Shortcut.lnk",base);
                else _snwprintf(name,260,L"%.220ls - Shortcut (%d).lnk",base,index);
                wchar_t *full=join(o->dir,name);
                err=full?shell_create_shortcut(o->sources[k],full):ERROR_NOT_ENOUGH_MEMORY;
                free(full);
            }
            if(!err){o->completed++;wcscpy(o->name,name);}
            else if(!o->err)o->err=err;
        }
    } else if (o->kind == FSOP_RENAME) {
        o->err = (fpe_wsl_requires_native(o->from)||fpe_wsl_requires_native(o->to))&&!fpe_wsl_native(o->from)?ERROR_NOT_SUPPORTED:fpe_wsl_native(o->from) ? fpe_wsl_rename(o->from,o->to) : MoveFileExW(o->from, o->to, 0) ? 0 : GetLastError();
    } else {
        int folder = o->kind == FSOP_NEW_FOLDER;
        const wchar_t *base = folder ? L"New folder" : L"New file", *ext = folder ? L"" : L".txt";
        wchar_t stem[260];
        if (o->want[0]) { /* "New note.md" counts up as "New note (2).md" */
            wcscpy(stem, o->want);
            wchar_t *dot = wcsrchr(stem, L'.');
            ext = dot && dot > stem ? wcsrchr(o->want, L'.') : L"";
            if (dot && dot > stem) *dot = 0;
            base = stem;
        }
        o->err = ERROR_ALREADY_EXISTS;
        for (int i = 1; i < 1000 && (o->err == ERROR_ALREADY_EXISTS || o->err == ERROR_FILE_EXISTS); i++) {
            if (i == 1) _snwprintf(o->name, 260, L"%ls%ls", base, ext);
            else _snwprintf(o->name, 260, L"%ls (%d)%ls", base, i, ext);
            wchar_t *full = join(o->dir, o->name);
            if (!full) { o->err = ERROR_OUTOFMEMORY; break; }
            if(fpe_wsl_requires_native(full)&&!fpe_wsl_native(full))o->err=ERROR_NOT_SUPPORTED;
            else if (fpe_wsl_native(full)) {
                o->err = fpe_wsl_create(full,folder,o->tmpl);
            } else if (folder) {
                o->err = CreateDirectoryW(full, NULL) ? 0 : GetLastError();
            } else if (o->tmpl) {
                o->err = CopyFileW(o->tmpl, full, TRUE) ? 0 : GetLastError();
                /* a read-only template would make a read-only file */
                if (!o->err) SetFileAttributesW(full, GetFileAttributesW(full) & ~(DWORD)FILE_ATTRIBUTE_READONLY);
            } else {
                HANDLE h = CreateFileW(full, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
                o->err = h != INVALID_HANDLE_VALUE ? 0 : GetLastError();
                if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            }
            free(full);
        }
    }
    FPE_PROF_END("fsop.run", t);
    if (!PostMessageW(g.hwnd, WM_APP_FSOP, 0, (LPARAM)o)) {
        o->held = NULL; /* the window is gone; the registry belongs to the UI thread */
        fsop_free(o);
    }
}

/* owner: the function that asked (NULL: the one for kind) */
static void fsop_submit(Pane *p, int kind, const wchar_t *from, const wchar_t *to, const wchar_t *new_name,
                        const wchar_t *tmpl, const fn_def *owner)
{
    if (!p->path) return;
    fsop *o = calloc(1, sizeof *o);
    if (!o) return;
    o->kind = kind;
    o->pane = p->idx;
    o->dir = wdup(p->path);
    o->from = from ? join(p->path, from) : NULL;
    o->to = to ? join(p->path, to) : NULL;
    if (new_name) wcsncpy(kind == FSOP_NEW_FILE ? o->want : o->name, new_name, 259);
    if (tmpl && !(o->tmpl = wdup(tmpl))) {
        fsop_free(o);
        return;
    }
    if (!owner) owner = fn_find(fsop_owner[kind]);
    if (owner && !fn_hold(owner)) o->held = owner;
    if (!TrySubmitThreadpoolCallback(fsop_work, o, NULL)) fsop_work(NULL, o);
}

static void fsop_done(fsop *o)
{
    Pane *p = &g.panes[o->pane];
    if(o->kind==FSOP_SHORTCUT||o->kind==FSOP_SYMLINK){
        const wchar_t *kind=o->kind==FSOP_SYMLINK?L"symbolic link":L"shortcut";
        if(o->err){wchar_t e[256];win_err(e,256,o->err);
            if(o->kind==FSOP_SYMLINK&&o->from){wchar_t label[260];const wchar_t *base=path_label(o->from);
                set_status(p,1,L"Created %u of %u symbolic links: %.160ls - %ls",o->completed,o->count,shown_name(base,(int)wcslen(base),label,260,fpe_wsl_native(o->from)),e);
            }else set_status(p,1,L"Created %u of %u %lss: %ls",o->completed,o->count,kind,e);
        }
        else set_status(p,0,L"Created %u %ls%ls",o->completed,kind,o->completed==1?L"":L"s");
        if(o->completed && same_path(p->path,o->dir)){
            wcscpy(p->want_select,o->name);p->want_rename=0;p_refresh(p);
        }
    } else if (o->err) {
        wchar_t e[256];
        win_err(e, 256, o->err);
        static const wchar_t *what[] = { L"rename", L"create the folder", L"create the file" };
        set_status(p, 1, L"Cannot %ls: %ls", what[o->kind], e);
    } else if (same_path(p->path, o->dir)) {
        /* the watcher would get there too; this makes it immediate */
        wcsncpy(p->want_select, o->name, 259);
        p->want_select[259] = 0;
        p->want_rename = o->kind != FSOP_RENAME;
        p_refresh(p);
    }
    Pane *q = other_pane(p);
    if (same_path(q->path, o->dir)) p_refresh(q);
    fsop_free(o);
}

static int shortcut_submit(Pane *p,const wchar_t *dest,const wchar_t *const *paths,uint32_t n)
{
    if(!n||n>256||fpe_wsl_native(dest)||!fn_active(fn_find("file.shortcut")))return 0;
    fsop *o=calloc(1,sizeof *o);
    if(!o)return 0;
    o->kind=FSOP_SHORTCUT;o->pane=p->idx;o->dir=wdup(dest);o->sources=calloc(n,sizeof *o->sources);
    if(!o->dir||!o->sources){fsop_free(o);return 0;}
    for(uint32_t i=0;i<n;++i){
        o->sources[o->count++]=wdup(paths[i]);
        if(!o->sources[i]){fsop_free(o);return 0;}
    }
    const fn_def *owner=fn_find("file.shortcut");
    if(fn_hold(owner)){fsop_free(o);return 0;}
    o->held=owner;
    if(!TrySubmitThreadpoolCallback(fsop_work,o,NULL)){fsop_free(o);return 0;}
    return 1;
}

static int symlink_submit(Pane *p,const wchar_t *dest,const wchar_t *const *paths,uint32_t n)
{
    if(!n||n>256||!fn_active(fn_find("file.symlink")))return 0;
    for(uint32_t i=0;i<n;i++)if(!fpe_wsl_symlink_compatible(paths[i],dest))return 0;
    fsop *o=calloc(1,sizeof *o);if(!o)return 0;
    o->kind=FSOP_SYMLINK;o->pane=p->idx;o->dir=wdup(dest);o->sources=calloc(n,sizeof *o->sources);
    if(!o->dir||!o->sources){fsop_free(o);return 0;}
    for(uint32_t i=0;i<n;i++){
        o->sources[o->count++]=wdup(paths[i]);if(!o->sources[i]){fsop_free(o);return 0;}
    }
    const fn_def *owner=fn_find("file.symlink");if(fn_hold(owner)){fsop_free(o);return 0;}o->held=owner;
    wchar_t name[260];const wchar_t *base=path_label(o->sources[0]);
    if(fpe_wsl_native(o->sources[0])){if(!fpe_lx_literal(base,name,260,0)){fsop_free(o);return 0;}}
    else{wcsncpy(name,base,259);name[259]=0;}
    g.modal_seq++;int chosen=linkdialog_choose(g.hwnd,g.font,o->sources[0],o->dir,n,&o->relative,name);g.modal_seq++;
    if(!chosen){fsop_free(o);return -1;}
    if(n==1)wcscpy(o->want,name);
    if(!TrySubmitThreadpoolCallback(fsop_work,o,NULL)){fsop_free(o);return 0;}
    set_status(p,0,L"Creating %u symbolic link%ls...",n,n==1?L"":L"s");return 1;
}

static void p_new_item(Pane *p, int file)
{
    if (!p->path) return;
    fsop_submit(p, file ? FSOP_NEW_FILE : FSOP_NEW_FOLDER, NULL, NULL, NULL, NULL, NULL);
}

static void p_apply_want(Pane *p)
{
    if (!p->want_select[0] || !p->view) return;
    uint32_t r = fpe_view_find(p->view, p->want_select);
    if (r == UINT32_MAX) return;
    p->want_select[0] = 0;
    p_clear_sel(p);
    p_set_sel(p, r, 1);
    p->cursor = p->anchor = r;
    p_ensure_visible(p, r);
    p_redraw(p);
    if (p->want_rename) {
        p->want_rename = 0;
        if (fn_active(fn_find("file.rename"))) rename_begin(p, r);
    }
}

static LRESULT CALLBACK rename_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    Pane *p = (Pane *)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_KEYDOWN:
        if (w == VK_RETURN || w == VK_TAB) { rename_end(p, 1); return 0; }
        if (w == VK_ESCAPE) { rename_end(p, 0); return 0; }
        break;
    case WM_CHAR:
        if (w == L'\r' || w == 27 || w == L'\t') return 0;
        int linux=fpe_wsl_native(p->path);
        if (w < 128 && (linux?w==L'/':strchr("\\/:*?\"<>|", (int)w)!=NULL)) {
            set_status(p, 1, linux?L"A Linux name can't contain /":L"A name can't contain  \\ / : * ? \" < > |");
            return 0;
        }
        break;
    case WM_KILLFOCUS: {
        /* Clicking elsewhere in the window keeps the new name, as in
         * Explorer; switching to another app leaves the box open (the main
         * window itself only holds focus briefly while being activated and
         * hands it back). The edit can't be destroyed from inside its own
         * focus change. */
        HWND to = (HWND)w;
        if (to && IsChild(g.hwnd, to)) PostMessageW(p->hwnd, PM_RENAME_DONE, 0, (LPARAM)h);
        break;
    }
    }
    return CallWindowProcW(g.rename_proc_orig, h, m, w, l);
}

static void rename_begin(Pane *p, uint32_t row)
{
    if (!p->view || row >= p_count(p) || !p->path) return;
    rename_end(p, 1);
    const fpe_entry *e = fpe_view_at(p->view, row);
    const wchar_t *name = fpe_view_name(p->view, row);
    wcsncpy(p->rename_old, name, 259);
    p->rename_old[259] = 0;
    p_ensure_visible(p, row);
    p_redraw(p);

    cols c = p_columns(p);
    int y = p_list_top(p) + ((int)row - p->top) * g.row_h;
    if(p->thumbnails){thumb_grid grid=p_grid(p);int pos=(int)row-p->top;
        c.name_x=(pos%grid.columns)*grid.cell_w+S(8);c.name_w=grid.cell_w-S(16);
        int edge=S(p->thumb_size);if(edge>THUMB_EDGE_MAX)edge=THUMB_EDGE_MAX;
        y=p_list_top(p)+(pos/grid.columns)*grid.cell_h+S(8)+edge;
    }
    wchar_t edit_name[260];wcsncpy(edit_name,name,259);edit_name[259]=0;
    if(fpe_wsl_native(p->path)&&!fpe_lx_literal(name,edit_name,260,0))return;
    /* Configure the native edit while hidden, so its initial visible frame
     * already has the final font and subclass instead of painting twice. */
    int64_t create=fpe_prof_begin();
    HWND h = CreateWindowExW(0, L"EDIT", edit_name, WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, c.name_x - S(4),
                             y + S(1), c.name_w + S(8), g.row_h - S(2), p->hwnd, NULL, GetModuleHandleW(NULL), NULL);
    FPE_PROF_END("rename.create_edit",create);
    if (!h) return;
    SendMessageW(h, WM_SETFONT, (WPARAM)g.font, FALSE);
    SendMessageW(h, EM_SETLIMITTEXT, 255, 0);
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)p);
    g.rename_proc_orig = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)rename_proc);
    p->rename_edit = h;
    const fn_def *rn = fn_find("file.rename");
    p->rename_held = rn && !fn_hold(rn) ? rn : NULL;
    /* select the name without its extension, as Explorer does */
    int stem=(int)wcslen(edit_name);const wchar_t *dot=wcsrchr(edit_name,L'.');
    if(!fpe_entry_is_dir(e)&&dot&&dot>edit_name)stem=(int)(dot-edit_name);
    int64_t show=fpe_prof_begin();
    ShowWindow(h,SW_SHOWNOACTIVATE);
    focus(h);
    SendMessageW(h, EM_SETSEL, 0, stem);
    FPE_PROF_END("rename.show_edit",show);
}

static void rename_commit(Pane *p, wchar_t *buf)
{
    int linux=fpe_wsl_path_kind(p->path);
    size_t n=wcslen(buf);wchar_t *s=buf;
    if(!linux){
        while(n&&(buf[n-1]==L' '||buf[n-1]==L'.'))buf[--n]=0;
        while(*s==L' ')s++;
    }
    if(!*s)return;
    if((linux&&(!wcscmp(s,L".")||!wcscmp(s,L"..")||wcschr(s,L'/')))||(!linux&&wcspbrk(s,L"\\/:*?\"<>|"))){
        set_status(p,1,linux?L"Use a Linux filename without /, . or ..":L"A name can't contain  \\ / : * ? \" < > |");return;
    }
    wchar_t encoded[260];
    if(linux){if(!linkdialog_name(s,encoded)){set_status(p,1,L"Use a Linux filename of at most 255 bytes");return;}s=encoded;}
    if(!wcscmp(s,p->rename_old))return;
    fsop_submit(p, FSOP_RENAME, p->rename_old, s, s, NULL, NULL);
}

static void rename_end(Pane *p, int commit)
{
    HWND h = p->rename_edit;
    if (!h) return;
    p->rename_edit = NULL;
    wchar_t buf[300];
    GetWindowTextW(h, buf, 300);
    if (GetFocus() == h) focus(p->hwnd);
    DestroyWindow(h);
    if (commit) rename_commit(p, buf);
    /* after the commit took its own hold, so file.rename runs without a gap */
    if (p->rename_held) fn_release(p->rename_held);
    p->rename_held = NULL;
}

/* ---- function contexts + context menus ------------------------------------------------- */

typedef struct pane_menu_capture {
    wchar_t *path;
    fpe_dir *listing;
    fpe_view *view;
    uint64_t tabs_epoch;
    int tab;
    uint32_t cursor, nsel;
    int ready;
} pane_menu_capture;

static void ctx_free(fn_ctx *c)
{
    if (c->paths) free_targets(c->paths, c->n);
    c->paths = NULL;
    pane_menu_capture *capture = c->menu_capture;
    if (capture) {
        fpe_dir_release(capture->listing);
        free(capture->path);
        free(capture);
        c->menu_capture = NULL;
    }
}

static uint32_t pane_gather(fn_ctx *c)
{
    uint32_t n = p_targets((Pane *)c->pane, &c->paths, &c->dirs);
    if (!n) c->state &= ~(FN_S_ITEMS | FN_S_ONE | FN_S_DIR); /* the selected rows are filtered out */
    return n;
}

/* items: the selection (or the cursor row) is targeted, not just the folder.
 * Nothing is allocated until a function asks for the paths. */
static void pane_ctx(fn_ctx *c, Pane *p, int from, int items)
{
    memset(c, 0, sizeof *c);
    c->where = FN_PANE;
    c->from = from;
    if (from == FN_FROM_MENU) c->menu_revision = fn_revision();
    c->pane = p;
    c->dir = c->dest = p->path;
    c->gather = pane_gather;
    if (g.dual) c->state |= FN_S_DUAL;
    if (p->path) c->state |= FN_S_FOLDER;
    if (items && p->path && p_count(p)) {
        c->n = p->nsel ? p->nsel : 1;
        c->state |= FN_S_ITEMS;
        if (c->n == 1) {
            c->state |= FN_S_ONE;
            const fpe_entry *e = fpe_view_at(p->view, p->cursor);
            if (e && fpe_entry_is_dir(e)) c->state |= FN_S_DIR;
        }
    }
    if (from == FN_FROM_MENU && p->path) {
        pane_menu_capture *capture = calloc(1, sizeof *capture);
        if (!capture) return;
        capture->path = wdup(p->path);
        if (!capture->path) { free(capture); return; }
        capture->listing = p->view ? fpe_dir_retain(fpe_view_dir(p->view)) : NULL;
        capture->view = p->view;
        capture->tabs_epoch = p->tabs_epoch;
        capture->tab = p->tab;
        capture->cursor = p->cursor;
        capture->nsel = p->nsel;
        c->menu_capture = capture;
        c->dir = c->dest = capture->path;
        if (c->n) {
            uint32_t expected = c->n;
            fn_paths(c);
            if (c->n != expected || !c->paths) return;
            for (uint32_t i = 0; i < c->n; i++)
                if (!c->paths[i]) return;
        }
        capture->ready = 1;
    }
}

int app_menu_context_current(const fn_ctx *c)
{
    if (c && c->from == FN_FROM_MENU && c->menu_revision != fn_revision()) return 0;
    if (!c || c->where != FN_PANE || c->from != FN_FROM_MENU) return 1;
    const pane_menu_capture *capture = c->menu_capture;
    Pane *p = c->pane;
    if (!capture || !capture->ready || !p || !p->path || !p->view || p->nav_ticket || p->loading ||
        p->tab != capture->tab || p->tabs_epoch != capture->tabs_epoch ||
        p->view != capture->view || fpe_view_dir(p->view) != capture->listing ||
        p->cursor != capture->cursor || p->nsel != capture->nsel ||
        !same_path(p->path, capture->path)) return 0;
    if (!c->n) return 1;
    wchar_t **paths = NULL;
    uint32_t dirs = 0;
    uint32_t n = p_targets(p, &paths, &dirs);
    int valid = n == c->n && dirs == c->dirs;
    for (uint32_t i = 0; valid && i < n; i++)
        valid = paths[i] && c->paths[i] && !wcscmp(paths[i], c->paths[i]);
    if (paths) free_targets(paths, n);
    return valid;
}

/* A tree folder; the pane acted on is the active one. */
static void tree_ctx(fn_ctx *c, const tnode *n, int from)
{
    memset(c, 0, sizeof *c);
    c->where = FN_TREE;
    c->from = from;
    if (from == FN_FROM_MENU) c->menu_revision = fn_revision();
    c->pane = active_pane();
    if (g.dual) c->state |= FN_S_DUAL;
    if (!n || n->kind == NODE_LINUX) return;
    /* a copy: a reload can free the node while a menu is open */
    c->paths = calloc(1, sizeof *c->paths);
    if (c->paths) c->paths[0] = wdup(n->path);
    if (!c->paths || !c->paths[0]) {
        free(c->paths);
        c->paths = NULL;
        return;
    }
    c->n = 1;
    c->dirs = 1;
    c->dir = c->dest = c->paths[0];
    c->state |= FN_S_ITEMS | FN_S_ONE | FN_S_DIR | FN_S_FOLDER;
    if (!n->parent || n->kind == NODE_DISTRO) c->state |= FN_S_ROOT;
}

static uint8_t key_mods(int alt)
{
    return (uint8_t)((GetKeyState(VK_CONTROL) < 0 ? FN_CTRL : 0) | (GetKeyState(VK_SHIFT) < 0 ? FN_SHIFT : 0) |
                     (alt || GetKeyState(VK_MENU) < 0 ? FN_ALT : 0));
}

/* A handled key's character (or Alt+key's WM_SYSCHAR, which would beep) is
 * already queued by TranslateMessage: drop it so it doesn't also type into
 * the filter. */
static void eat_char(HWND h, int sys)
{
    MSG m;
    PeekMessageW(&m, h, sys ? WM_SYSCHAR : WM_CHAR, sys ? WM_SYSDEADCHAR : WM_DEADCHAR, PM_REMOVE);
}

/* Control characters without a handled key down (posted ones, or layouts
 * where the key differs): Ctrl+\ arrives as 0x1C. Returns the key or 0. */
static uint8_t ctrl_char_key(WPARAM ch)
{
    if (ch == 0 || ch >= 32 || ch == 8 || ch == 9 || ch == 10 || ch == 13 || ch == 27) return 0;
    SHORT v = VkKeyScanW((WCHAR)(ch + 0x40));
    return v == -1 ? 0 : LOBYTE(v);
}

static int pane_key(Pane *p, WPARAM vk, int sys, LPARAM l)
{
    fn_ctx c;
    pane_ctx(&c, p, FN_FROM_KEY, 1);
    int r = fn_key((uint8_t)vk, key_mods(sys && (l & (1 << 29))), &c);
    ctx_free(&c);
    if (r) eat_char(p->hwnd, sys);
    return r;
}

static int tree_key(WPARAM vk, int sys, LPARAM l)
{
    fn_ctx c;
    tree_ctx(&c, T.cursor ? T.cursor : T.current, FN_FROM_KEY);
    int r = fn_key((uint8_t)vk, key_mods(sys && (l & (1 << 29))), &c);
    ctx_free(&c);
    if (r) eat_char(T.hwnd, sys);
    return r;
}

static void pane_run(Pane *p, const char *id, int from)
{
    fn_ctx c;
    pane_ctx(&c, p, from, 1);
    fn_run_id(id, &c);
    ctx_free(&c);
}

/* Runs a function on one folder that isn't the selection (a crumb, a row
 * under the mouse). Takes ownership of `path`. */
static void path_run(Pane *p, const char *id, wchar_t *path, int from)
{
    fn_ctx c;
    pane_ctx(&c, p, from, 0);
    c.paths = calloc(1, sizeof *c.paths);
    if (!c.paths) { free(path); return; }
    c.paths[0] = path;
    c.n = 1;
    c.dirs = 1;
    c.dest = path;
    c.state |= FN_S_ITEMS | FN_S_ONE | FN_S_DIR;
    fn_run_id(id, &c);
    ctx_free(&c);
}

#define MENU_MORE (FN_MENU_ID0 - 1) /* "More from Windows…" */

/* Explorer's menu for c: its items after the ones in m (if any), shell
 * extensions and all. Returns the app item picked, or 0 with an
 * intercepted shell verb in `verb`. */
static UINT explorer_menu(HMENU m, fn_ctx *c, POINT pt, int extended, char *verb, size_t verb_cap)
{
    const fn_def *ex = fn_find("menu.explorer");
    if (fn_begin(ex)) return 0;
    char intercept[256];
    fn_verbs(c, intercept, sizeof intercept);
    g.modal_seq++;
    UINT id = shell_context_menu(g.hwnd, m, c->dir, (const wchar_t *const *)c->paths, c->n, pt, extended, intercept,
                                 verb, verb_cap);
    g.modal_seq++;
    fn_end(ex);
    return id;
}

/* menu.context style = ours: the app's items drawn by the app, with
 * Explorer's a click away ("More from Windows…", only built then).
 * native: Windows draws the app's items followed by Explorer's.
 * menu.explorer = off keeps every shell extension out of the process.
 * Returns the app item picked, or 0 with an intercepted shell verb in `verb`. */
static UINT show_context_menu(uint32_t kind, fn_ctx *c, POINT pt, char *verb, size_t verb_cap)
{
    verb[0] = 0;
    UINT id = 0;
    int extended = GetKeyState(VK_SHIFT) < 0;
    int explorer = fn_active(fn_find("menu.explorer"));
    const fn_def *mc = fn_find("menu.context");
    int ours = fn_active(mc) && _stricmp(fn_option(mc, "style"), "native") != 0;
    HMENU m = CreatePopupMenu();
    fn_menu(m, kind | (ours ? FN_MENU_OURS : 0), c);
    if (!ours && explorer) {
        id = explorer_menu(m, c, pt, extended, verb, verb_cap);
    } else {
        if (ours && explorer) {
            if (GetMenuItemCount(m) > 0) AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AppendMenuW(m, MF_STRING, MENU_MORE, L"More from Windows\u2026");
        }
        if (GetMenuItemCount(m) > 0) {
            g.modal_seq++;
            id = ours ? menu_track(m, g.hwnd, pt)
                      : (UINT)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, g.hwnd, NULL);
            g.modal_seq++;
        }
        if (id == MENU_MORE) {
            HMENU shell = CreatePopupMenu();
            SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_APPSTARTING));
            id = explorer_menu(shell, c, pt, extended || GetKeyState(VK_SHIFT) < 0, verb, verb_cap);
            DestroyMenu(shell);
        }
    }
    DestroyMenu(m);
    return id;
}

static void run_menu_choice(fn_ctx *c, UINT id, const char *verb)
{
    if ((id || verb[0]) && !app_menu_context_current(c)) return;
    if (id) {
        fn_menu_run(id, c);
    } else if (verb[0]) {
        c->from = FN_FROM_VERB;
        fn_verb(verb, c);
    }
}

/* on_item: the menu is for the selection (or cursor row); otherwise for the
 * folder itself. */
static void p_context_menu(Pane *p, POINT screen, int on_item)
{
    if (!p->path) return;
    rename_end(p, 1);
    fn_ctx c;
    pane_ctx(&c, p, FN_FROM_MENU, on_item);
    if (!app_menu_context_current(&c)) { ctx_free(&c); return; }
    fn_paths(&c);
    if (c.state & FN_S_DIR) c.dest = c.paths[0]; /* paste into the folder clicked */
    char verb[32];
    UINT id = show_context_menu(c.n ? FN_MENU_ITEMS : FN_MENU_FOLDER, &c, screen, verb, sizeof verb);
    run_menu_choice(&c, id, verb);
    ctx_free(&c);
}

/* Right click selects the row under the mouse unless it is already part of
 * the selection, so the menu acts on what is highlighted. */
static void p_right_click(Pane *p, int x, int y)
{
    int r = p_row_at(p, x, y);
    if (r < 0) {
        p_clear_sel(p);
    } else if (!p_is_sel(p, (uint32_t)r)) {
        p_clear_sel(p);
        p_set_sel(p, (uint32_t)r, 1);
        p->anchor = (uint32_t)r;
    }
    if (r >= 0) p->cursor = (uint32_t)r;
    p_redraw(p);
    UpdateWindow(p->hwnd);
}

static void p_context_key_or_mouse(Pane *p, LPARAM l)
{
    POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
    if (l == -1) { /* keyboard (menu key, Shift+F10): at the cursor row */
        int on = p_count(p) > 0;
        int row = on ? (int)p->cursor - p->top : 0;
        if (row < 0 || row >= p_page(p)) row = 0;
        cols c = p_columns(p);
        pt.x = c.name_x + S(20);
        pt.y = p_list_top(p) + row * g.row_h + g.row_h;
        if(p->thumbnails){thumb_grid grid=p_grid(p);pt.x=(row%grid.columns)*grid.cell_w+grid.cell_w/2;
            pt.y=p_list_top(p)+(row/grid.columns)*grid.cell_h+grid.cell_h/2;
        }
        ClientToScreen(p->hwnd, &pt);
        p_context_menu(p, pt, on);
        return;
    }
    POINT cl = pt;
    ScreenToClient(p->hwnd, &cl);
    if (cl.y < p_list_top(p)) return; /* address bar, header */
    p_context_menu(p, pt, p_row_at(p, cl.x, cl.y) >= 0);
}

/* ---- drag and drop ------------------------------------------------------------------------------ */

/* The folder a drop at client y would land in: a folder row under the
 * mouse, else the pane's own folder. *row gets the highlighted row or -2. */
static wchar_t *p_drop_dest(Pane *p, int x, int y, int *row)
{
    int r = p_row_at(p, x, y);
    *row = -2;
    if (r >= 0 && fpe_entry_is_dir(fpe_view_at(p->view, (uint32_t)r))) {
        *row = r;
        return join(p->path, fpe_view_name(p->view, (uint32_t)r));
    }
    return p->path ? wdup(p->path) : NULL;
}

static DWORD drop_choices(const wchar_t *const *paths,uint32_t n,const wchar_t *dest,DWORD allowed)
{
    if(g.closing||g.destroyed||!dest||!paths||!n)return DROPEFFECT_NONE;
    DWORD choices=shell_drop_choices(paths,n,dest,allowed);
    if(fpe_wsl_native(dest)){
        choices&=~DROPEFFECT_LINK;
        if(n&&n<=256&&(allowed&DROPEFFECT_LINK)&&fn_active(fn_find("file.symlink"))){
            int compatible=1;for(uint32_t i=0;i<n&&compatible;i++)compatible=fpe_wsl_symlink_compatible(paths[i],dest);
            if(compatible)choices|=DROPEFFECT_LINK;
        }
    }else if(n>256||!fn_active(fn_find("file.shortcut")))choices&=~DROPEFFECT_LINK;
    return choices;
}
static DWORD drop_hover_effect(const wchar_t *const *paths,uint32_t n,const wchar_t *dest,DWORD keys,DWORD allowed)
{
    if(g.closing||g.destroyed)return DROPEFFECT_NONE;
    DWORD effect=shell_drop_effect(paths,n,dest,keys,allowed);
    if(keys&MK_RBUTTON){
        DWORD choices=drop_choices(paths,n,dest,allowed);
        if(!(effect&choices))effect=(choices&DROPEFFECT_COPY)?DROPEFFECT_COPY:(choices&DROPEFFECT_MOVE)?DROPEFFECT_MOVE:(choices&DROPEFFECT_LINK);
    }
    return effect;
}
static DWORD drop_commit(Pane *p,POINT screen,const wchar_t *dest,DWORD effect,DWORD keys,DWORD allowed,
                         const wchar_t *const *paths,uint32_t n)
{
    if(g.closing||g.destroyed||!dest)return DROPEFFECT_NONE;
    if(keys&MK_RBUTTON){
        DWORD choices=drop_choices(paths,n,dest,allowed);
        if(!choices)return DROPEFFECT_NONE;
        HMENU menu=CreatePopupMenu();
        if(!menu)return DROPEFFECT_NONE;
        AppendMenuW(menu,MF_STRING|((choices&DROPEFFECT_COPY)?0:MF_GRAYED),DROPEFFECT_COPY,L"Copy here");
        AppendMenuW(menu,MF_STRING|((choices&DROPEFFECT_MOVE)?0:MF_GRAYED),DROPEFFECT_MOVE,L"Move here");
        AppendMenuW(menu,MF_STRING|((choices&DROPEFFECT_LINK)?0:MF_GRAYED),DROPEFFECT_LINK,fpe_wsl_native(dest)?L"Create symbolic link here":L"Create shortcut here");
        AppendMenuW(menu,MF_SEPARATOR,0,NULL);AppendMenuW(menu,MF_STRING,8,L"Cancel");
        SetMenuDefaultItem(menu,effect,FALSE);
        g.modal_seq++;UINT picked=menu_track(menu,g.hwnd,screen);g.modal_seq++;DestroyMenu(menu);
        effect=(picked==DROPEFFECT_COPY || picked==DROPEFFECT_MOVE || picked==DROPEFFECT_LINK)?picked&choices:0;
    }
    if(!effect)return DROPEFFECT_NONE;
    if(effect==DROPEFFECT_LINK){
        if(fpe_wsl_native(dest)){
            int result=symlink_submit(p,dest,paths,n);
            if(result<=0){if(!result)set_status(p,1,L"Could not queue symbolic link creation");return DROPEFFECT_NONE;}
        }else if(!shortcut_submit(p,dest,paths,n)){set_status(p,1,L"Could not queue shortcut creation");return DROPEFFECT_NONE;}
    }else operation_submit(effect==DROPEFFECT_MOVE?FPE_OP_MOVE:FPE_OP_COPY,paths,n,dest,0);
    return effect;
}

static DWORD p_drop_over(void *ctx, POINT pt, DWORD keys, DWORD allowed, const wchar_t *const *paths, uint32_t n)
{
    Pane *p = ctx;
    if (!p->view) return DROPEFFECT_NONE;
    int scrolled = 0;
    uint64_t now = GetTickCount64();
    if (now - p->drop_scroll_tick > 50) { /* scroll when hovering near the edges */
        int old = p->top;
        if (pt.y < p_list_top(p) + p_cell_h(p) / 4) p->top-=p_columns_count(p);
        else if (pt.y > p_list_bottom(p) - p_cell_h(p) / 4) p->top+=p_columns_count(p);
        p_clamp(p);
        scrolled = old != p->top;
        p->drop_scroll_tick = now;
    }
    int row;
    wchar_t *dest = p_drop_dest(p, pt.x, pt.y, &row);
    DWORD e = drop_hover_effect(paths, n, dest, keys, allowed);
    free(dest);
    if (!e) row = -1;
    if (row != p->drop_row || scrolled) {
        p->drop_row = row;
        p_redraw(p);
    }
    return e;
}

static void p_drop_leave(void *ctx)
{
    Pane *p = ctx;
    if (p->drop_row != -1) {
        p->drop_row = -1;
        InvalidateRect(p->hwnd, NULL, FALSE);
    }
}

static DWORD p_drop(void *ctx, POINT pt, DWORD effect, DWORD keys, DWORD allowed,
                    const wchar_t *const *paths, uint32_t n)
{
    Pane *p=ctx;int row;wchar_t *dest=p_drop_dest(p,pt.x,pt.y,&row);
    POINT screen=pt;ClientToScreen(p->hwnd,&screen);
    DWORD chosen=drop_commit(p,screen,dest,effect,keys,allowed,paths,n);
    free(dest);return chosen;
}

static const shell_drop_handler pane_drop = { p_drop_over, p_drop_leave, p_drop };

/* item.drag_out: the drag runs its own loop until the drop or cancel. */
static void run_drag_out(fn_ctx *c)
{
    if (!fn_paths(c) || !c->n) return;
    HWND h = c->where == FN_TREE ? T.hwnd : ((Pane *)c->pane)->hwnd;
    g.modal_seq++;
    shell_drag_out(h, (const wchar_t *const *)c->paths, c->n, g_file_drag_right);
    g.modal_seq++;
}

/* ======================================================================== */
/* Tree                                                                      */
/* ======================================================================== */

static void tree_free_node(tnode *n);

static void tree_free_kids(tnode *n)
{
    for (uint32_t i = 0; i < n->nkids; i++) tree_free_node(n->kids[i]);
    free(n->kids);
    n->kids = NULL;
    n->nkids = 0;
}

static void tree_free_node(tnode *n)
{
    tree_free_kids(n);
    if (T.cursor == n) T.cursor = NULL;
    if (T.current == n) T.current = NULL;
    if (T.drop == n) T.drop = NULL;
    if (T.down == n) T.down = NULL;
    if (n->kind != NODE_FOLDER) free(n->label);
    free(n->path);
    free(n);
}

static tnode *tree_new(tnode *parent, const wchar_t *path, const wchar_t *label, int kind)
{
    tnode *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->parent = parent;
    n->path = wdup(path);
    if (!n->path) { free(n); return NULL; }
    n->kind = (uint8_t)kind;
    n->ti = kind == NODE_DRIVE ? TI_DRIVE : TI_FOLDER;
    n->icon = -1;
    n->depth = parent ? parent->depth + 1 : 0;
    if (kind == NODE_FOLDER) {
        const wchar_t *s = wcsrchr(n->path, L'\\');
        n->label = s && s[1] ? (wchar_t *)s + 1 : n->path;
    } else {
        n->label = wdup(label);
        if (!n->label) { free(n->path); free(n); return NULL; }
    }
    return n;
}

static void tree_flatten(tnode *n)
{
    if (T.nrows == T.rows_cap) {
        uint32_t cap = T.rows_cap ? T.rows_cap * 2 : 256;
        tnode **r = realloc(T.rows, cap * sizeof *r);
        if (!r) return;
        T.rows = r;
        T.rows_cap = cap;
    }
    T.rows[T.nrows++] = n;
    if (n->expanded)
        for (uint32_t i = 0; i < n->nkids; i++) tree_flatten(n->kids[i]);
}

static int tree_page(void)
{
    int r = (T.ch - g.pad) / g.row_h;
    return r < 1 ? 1 : r;
}

static void tree_clamp(void)
{
    int max_top = (int)T.nrows - tree_page();
    if (T.top > max_top) T.top = max_top;
    if (T.top < 0) T.top = 0;
}

static int tree_row_of(const tnode *n)
{
    for (uint32_t i = 0; i < T.nrows; i++)
        if (T.rows[i] == n) return (int)i;
    return -1;
}

static void tree_ensure_visible(const tnode *n)
{
    int r = tree_row_of(n);
    if (r < 0) return;
    if (r < T.top) T.top = r;
    else if (r >= T.top + tree_page()) T.top = r - tree_page() + 1;
    tree_clamp();
}

static void tree_redraw(void)
{
    if (!T.hwnd) return;
    SCROLLINFO si = { sizeof si, SIF_ALL | SIF_DISABLENOSCROLL, 0, T.nrows ? (int)T.nrows - 1 : 0,
                      (UINT)tree_page(), T.top, 0 };
    if(si.nPage>(UINT)(si.nMax-si.nMin)+1u)si.nPage=(UINT)(si.nMax-si.nMin)+1u;
    SCROLLINFO now = { sizeof now, SIF_ALL };
    if (!GetScrollInfo(T.hwnd, SB_VERT, &now) || now.nMin != si.nMin || now.nMax != si.nMax || now.nPage != si.nPage ||
        now.nPos != si.nPos) {
        int64_t trace = fpe_prof_begin();
        SetScrollInfo(T.hwnd, SB_VERT, &si, FALSE);
        RedrawWindow(T.hwnd, NULL, NULL, RDW_INVALIDATE | RDW_FRAME);
        FPE_PROF_END("tree.scrollbar", trace);
    }
    InvalidateRect(T.hwnd, NULL, FALSE);
}

static void tree_rebuild(void)
{
    int64_t t = fpe_prof_begin();
    T.nrows = 0;
    for (uint32_t i = 0; i < T.nroots; i++) tree_flatten(T.roots[i]);
    tree_clamp();
    tree_redraw();
    FPE_PROF_END("tree.rebuild", t);
}

/* Replace n's children with the folders in d. Both lists are in the same
 * natural order, so a merge walk keeps existing nodes (and whatever is
 * expanded below them) in linear time. */
static void tree_apply(tnode *n, fpe_dir *d)
{
    int64_t t = fpe_prof_begin();
    fpe_view_opts o = { FPE_SORT_NAME, 0, 1, (uint8_t)g.show_hidden, (uint8_t)g.show_hidden };
    fpe_view *v = fpe_view_create(d, &o);
    if (!v) return;
    uint32_t total = fpe_view_count(v), k = 0;
    while (k < total && fpe_entry_is_dir(fpe_view_at(v, k))) k++;

    tnode **kids = k ? calloc(k, sizeof *kids) : NULL;
    if (k && !kids) { fpe_view_destroy(v); return; }
    uint32_t j = 0, nk = 0;
    for (uint32_t i = 0; i < k; i++) {
        const wchar_t *name = fpe_view_name(v, i);
        tnode *reuse = NULL;
        while (j < n->nkids) {
            int c = fpe_compare_names(n->kids[j]->label, name);
            if (c < 0) { tree_free_node(n->kids[j++]); continue; }
            if (c == 0) reuse = n->kids[j++];
            break;
        }
        if (reuse) {
            kids[nk++] = reuse;
        } else {
            wchar_t *path = join(n->path, name);
            tnode *c = path ? tree_new(n, path, NULL, NODE_FOLDER) : NULL;
            free(path);
            if (c) kids[nk++] = c;
        }
    }
    while (j < n->nkids) tree_free_node(n->kids[j++]);
    free(n->kids);
    n->kids = kids;
    n->nkids = nk;
    n->loaded = 1;
    n->failed = 0;
    fpe_view_destroy(v);
    FPE_PROF_END("tree.apply", t);
}

/* Normalize the WSL server alias only; preserve quoted/raw Linux components. */
static wchar_t *tree_wsl_path(const wchar_t *path)
{
 const wchar_t *prefixes[]={L"\\\\wsl.localhost\\",L"\\\\wsl$\\",L"\\\\?\\UNC\\wsl.localhost\\",L"\\\\?\\UNC\\wsl$\\"};const wchar_t *rest=NULL;
 for(unsigned i=0;i<4;i++){size_t n=wcslen(prefixes[i]);if(!_wcsnicmp(path,prefixes[i],n)){rest=path+n;break;}}if(!rest||!*rest)return NULL;
 size_t n=wcslen(rest);int root=wcschr(rest,L'\\')==NULL;wchar_t *result=malloc((16+n+root+1)*sizeof *result);if(result)swprintf(result,16+n+root+1,L"\\\\wsl.localhost\\%ls%ls",rest,root?L"\\":L"");return result;
}
static int tree_path_prefix(const wchar_t *path,const wchar_t *prefix)
{
 if(!_wcsnicmp(prefix,L"\\\\wsl.localhost\\",16)){
  const wchar_t *end=wcschr(prefix+16,L'\\');size_t head=end?(size_t)(end-prefix):wcslen(prefix),n=wcslen(prefix);
  if(_wcsnicmp(path,prefix,head)||wcslen(path)<n||wcsncmp(path+head,prefix+head,n-head))return 0;
  return !path[n]||path[n]==L'\\'||prefix[n-1]==L'\\';
 }
 return has_prefix(path,prefix);
}

static tnode *tree_best_root(const wchar_t *path)
{
    tnode *best = NULL;
    size_t best_len = 0;
    for (uint32_t i = 0; i < T.nroots; i++) {
        tnode *root=T.roots[i];
        uint32_t count=root->kind==NODE_LINUX?root->nkids:1;
        for(uint32_t k=0;k<count;k++){tnode *candidate=root->kind==NODE_LINUX?root->kids[k]:root;size_t l=wcslen(candidate->path);
            if(l>best_len&&tree_path_prefix(path,candidate->path)){best=candidate;best_len=l;}}

    }
    return best;
}

static tnode *tree_find(const wchar_t *path)
{
    wchar_t *canonical=tree_wsl_path(path);if(canonical)path=canonical;
    tnode *n = tree_best_root(path);
    while (n && !same_path(n->path, path)) {
        tnode *next = NULL;
        for (uint32_t i = 0; i < n->nkids && !next; i++)
            if (tree_path_prefix(path, n->kids[i]->path)) next = n->kids[i];
        n = next;
    }
    free(canonical);
    return n;
}

static void tree_request(tnode *n)
{
    if (n->kind == NODE_LINUX || n->loading) return;
    fpe_dir *c = fpe_engine_cached(g.engine, n->path);
    if (c) {
        tree_apply(n, c);
        fpe_dir_release(c);
    }
    uint64_t t = fpe_engine_load(g.engine, n->path, on_loaded, NULL);
    if (!t) return;
    if (T.npend == T.pend_cap) {
        uint32_t cap = T.pend_cap ? T.pend_cap * 2 : 16;
        tpend *p = realloc(T.pend, cap * sizeof *p);
        if (!p) return;
        T.pend = p;
        T.pend_cap = cap;
    }
    T.pend[T.npend].ticket = t;
    T.pend[T.npend].path = wdup(n->path);
    T.npend++;
    n->loading = 1;
}

/* Expand the ancestors of the active pane's folder and highlight it. Steps
 * that need a folder read continue when that read completes. */
static void tree_reveal_step_impl(void);

static void tree_reveal_step(void)
{
    if (!T.reveal) return;
    int64_t t = fpe_prof_begin();
    tree_reveal_step_impl();
    FPE_PROF_END("tree.reveal", t);
}

static void tree_reveal_step_impl(void)
{
    tnode *n = tree_best_root(T.reveal);
    for(tnode *parent=n?n->parent:NULL;parent;parent=parent->parent)parent->expanded=1;
    while (n && !same_path(n->path, T.reveal)) {
        if (!n->loaded) {
            n->expanded = 1;
            tree_request(n);
            if (!n->loaded) { tree_rebuild(); return; }
        }
        n->expanded = 1;
        tnode *next = NULL;
        for (uint32_t i = 0; i < n->nkids && !next; i++)
            if (tree_path_prefix(T.reveal, n->kids[i]->path)) next = n->kids[i];
        if (!next) break;
        n = next;
    }
    T.current = n;
    free(T.reveal);
    T.reveal = NULL;
    if (n && GetFocus() != T.hwnd) T.cursor = n;
    tree_rebuild();
    if (n) tree_ensure_visible(n);
    tree_redraw();
}

static void tree_reveal(const wchar_t *path)
{
    if (!path) return;
    free(T.reveal);
    T.reveal = tree_wsl_path(path);
    if(!T.reveal)T.reveal = wdup(path);
    tree_reveal_step();
}

static int tree_handle_loaded(load_msg *m)
{
    for (uint32_t i = 0; i < T.npend; i++) {
        if (T.pend[i].ticket != m->ticket) continue;
        wchar_t *path = T.pend[i].path;
        T.pend[i] = T.pend[--T.npend];
        tnode *n = tree_find(path);
        if (n) {
            n->loading = 0;
            if (!m->err) {
                tree_apply(n, m->dir);
            } else {
                tree_free_kids(n);
                n->loaded = 1;
                n->failed = 1;
            }
        }
        free(path);
        tree_rebuild();
        tree_reveal_step();
        return 1;
    }
    return 0;
}

/* Pane reloads double as tree updates for the same folder. */
static void tree_on_snapshot(fpe_dir *d)
{
    tnode *n = tree_find(fpe_dir_path(d));
    if (n && n->loaded) {
        tree_apply(n, d);
        tree_rebuild();
    }
}

static void tree_reapply_loaded(tnode *n)
{
    if (n->kind != NODE_LINUX && n->loaded && !n->failed) {
        fpe_dir *c = fpe_engine_cached(g.engine, n->path);
        if (c) {
            tree_apply(n, c);
            fpe_dir_release(c);
        } else {
            tree_request(n);
        }
    }
    for (uint32_t i = 0; i < n->nkids; i++) tree_reapply_loaded(n->kids[i]);
}

static void tree_toggle(tnode *n)
{
    if (n->expanded) {
        n->expanded = 0;
        /* keep the cursor visible if it was inside the collapsed branch */
        for (tnode *c = T.cursor; c; c = c->parent)
            if (c->parent == n) { T.cursor = n; break; }
    } else {
        n->expanded = 1;
        if (!n->loaded) tree_request(n);
    }
    tree_rebuild();
}

static void tree_open(tnode *n)
{
    if (!n || n->kind == NODE_LINUX) return;
    Pane *p = active_pane();
    if (!same_path(n->path, p->path)) p_navigate(p, n->path, NULL, 1);
}

static tnode *tree_add_root(const wchar_t *path, const wchar_t *label, int kind)
{
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES && kind != NODE_DRIVE) return NULL;
    for (uint32_t i = 0; i < T.nroots; i++)
        if (same_path(T.roots[i]->path, path)) return NULL;
    tnode **r = realloc(T.roots, (T.nroots + 1) * sizeof *r);
    if (!r) return NULL;
    T.roots = r;
    tnode *n = tree_new(NULL, path, label, kind);
    if (n) T.roots[T.nroots++] = n;
    return n;
}

/* The grouping row has no filesystem target. Distros are implicit tree roots. */
static void tree_sync_wsl(void)
{
 if(!T.roots_ready)return;
 wsl_tree_distro distros[WSL_TREE_LIMIT];uint32_t count=fn_active(fn_find("wsl.helper"))?wsl_tree_discover(distros,WSL_TREE_LIMIT):0;
 uint32_t at=0;while(at<T.nroots&&T.roots[at]->kind!=NODE_LINUX)at++;tnode *old=at<T.nroots?T.roots[at]:NULL;
 if(old&&count==old->nkids){int same=1;for(uint32_t i=0;i<count;i++)if(wcscmp(old->kids[i]->path,distros[i].root))same=0;if(same)return;}
 if(!old&&!count)return;
 tnode *group=NULL;if(count){group=tree_new(NULL,L"",L"Linux",NODE_LINUX);if(!group)return;group->loaded=group->expanded=1;group->ti=TI_LINUX;group->kids=calloc(count,sizeof *group->kids);if(!group->kids){tree_free_node(group);return;}
  for(uint32_t i=0;i<count;i++){tnode *child=tree_new(group,distros[i].root,distros[i].name,NODE_DISTRO);if(!child){tree_free_node(group);return;}child->ti=TI_DRIVE;group->kids[group->nkids++]=child;}
 }
 if(old){
  /* Retire intake identities too: a late response must not attach to a rebuilt
   * distro branch, including off/on settings reloads. Engine callbacks own data. */
  for(uint32_t i=0;i<T.npend;){int belongs=0;for(uint32_t k=0;k<old->nkids;k++)if(has_prefix(T.pend[i].path,old->kids[k]->path))belongs=1;if(belongs){free(T.pend[i].path);T.pend[i]=T.pend[--T.npend];}else i++;}
  if(T.reveal&&fpe_wsl_path_kind(T.reveal)){free(T.reveal);T.reveal=NULL;}
  tree_free_node(old);if(group)T.roots[at]=group;else{memmove(T.roots+at,T.roots+at+1,(T.nroots-at-1)*sizeof *T.roots);T.nroots--;}
 }else{tnode **roots=realloc(T.roots,(T.nroots+1)*sizeof *roots);if(!roots){tree_free_node(group);return;}T.roots=roots;T.roots[T.nroots++]=group;}
 tree_rebuild();
}

/* ---- pins: folders the user keeps at the top of the tree ---- */

static int tree_pin_count(void)
{
    int n = 0;
    while (n < (int)T.nroots && T.roots[n]->kind == NODE_PIN) n++;
    return n;
}

static int tree_pinned_at(const wchar_t *path)
{
    for (int i = 0; path && i < tree_pin_count(); i++)
        if (same_path(T.roots[i]->path, path)) return i;
    return -1;
}

/* Not checked on disk: a pinned network folder must not stall startup. */
static void tree_pin(const wchar_t *path, int refresh)
{
    if (!path || tree_pinned_at(path) >= 0) return;
    tnode **r = realloc(T.roots, (T.nroots + 1) * sizeof *r);
    if (!r) return;
    T.roots = r;
    tnode *n = tree_new(NULL, path, path_label(path), NODE_PIN);
    if (!n) return;
    int at = tree_pin_count();
    memmove(&T.roots[at + 1], &T.roots[at], (T.nroots - (uint32_t)at) * sizeof *r);
    T.roots[at] = n;
    T.nroots++;
    if (refresh) {
        tree_rebuild();
        tree_reveal(active_pane()->path);
    }
}

static void tree_unpin(const wchar_t *path)
{
    int i = tree_pinned_at(path);
    if (i < 0) return;
    tnode *n = T.roots[i];
    memmove(&T.roots[i], &T.roots[i + 1], (T.nroots - (uint32_t)i - 1) * sizeof *T.roots);
    T.nroots--;
    tree_free_node(n);
    tree_rebuild();
    tree_reveal(active_pane()->path);
}

static void tree_add_known(REFKNOWNFOLDERID id, const wchar_t *label, ti_kind icon)
{
    PWSTR p = NULL;
    tnode *n = SUCCEEDED(SHGetKnownFolderPath(id, 0, NULL, &p)) && p ? tree_add_root(p, label, NODE_QUICK) : NULL;
    if (n) n->ti = (uint8_t)icon;
    CoTaskMemFree(p);
}

static void tree_init_roots(void)
{
    wchar_t home[MAX_PATH];
    if (GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH)) {
        const wchar_t *user = wcsrchr(home, L'\\');
        tnode *n = tree_add_root(home, user ? user + 1 : L"Home", NODE_QUICK);
        if (n) n->ti = TI_HOME;
    }
    tree_add_known(&FOLDERID_Desktop, L"Desktop", TI_DESKTOP);
    tree_add_known(&FOLDERID_Documents, L"Documents", TI_DOCUMENTS);
    tree_add_known(&FOLDERID_Downloads, L"Downloads", TI_DOWNLOADS);

    /* No volume labels: querying them can spin up disks or stall on
     * disconnected network drives. */
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
        wchar_t label[8] = { (wchar_t)(L'A' + i), L':', 0 };
        tree_add_root(root, label, NODE_DRIVE);
    }
    T.roots_ready=1;
    tree_sync_wsl();
    tree_rebuild();
}

/* Drives get stock icons by drive type: asking for a drive's own icon can
 * spin up a disk or wait on a disconnected network share. Quick-access
 * roots (home, Desktop, ...) get their real icons from the icon worker. */
static int tree_icon(tnode *n)
{
    if (n->kind == NODE_LINUX || n->kind == NODE_DISTRO) return -1;
    if (!shell_ready()) return -1;
    if (n->kind == NODE_FOLDER) return shell_icon_folder();
    if (n->icon >= 0) return n->icon;
    if (n->kind == NODE_DRIVE) {
        UINT type = GetDriveTypeW(n->path);
        SHSTOCKICONID id = type == DRIVE_REMOTE ? SIID_DRIVENET
                         : type == DRIVE_REMOVABLE ? SIID_DRIVEREMOVE
                         : type == DRIVE_CDROM ? SIID_DRIVECD
                         : SIID_DRIVEFIXED;
        n->icon = shell_icon_stock(id);
        return n->icon;
    }
    int own = shell_icon_path(n->path);
    if (own >= 0) n->icon = own;
    return own >= 0 ? own : shell_icon_folder();
}

static void tree_paint(HDC dc)
{
    SelectObject(dc, g.font);
    fill(dc, 0, 0, T.cw, T.ch, C_TREE_BG);
    int focused = GetFocus() == T.hwnd;
    int y0 = g.pad;
    for (uint32_t r = (uint32_t)T.top; r < T.nrows; r++) {
        int y = y0 + ((int)r - T.top) * g.row_h;
        if (y >= T.ch) break;
        tnode *n = T.rows[r];
        if (n == T.current) fill(dc, 0, y, T.cw, g.row_h, focused ? C_SEL : C_SEL_IDLE);
        if (n == T.cursor && focused) frame(dc, S(2), y, T.cw - S(4), g.row_h, C_CURSOR);
        if (n->depth == 0 && r > 0) { /* a line between pins, places and drives */
            uint32_t i = 0;
            while (i < T.nroots && T.roots[i] != n) i++;
            if (i > 0 && i < T.nroots && T.roots[i - 1]->kind != n->kind)
                fill(dc, g.pad, y, T.cw - 2 * g.pad, 1, C_HEADER);
        }

        if (n == T.drop) {
            fill(dc, 0, y, T.cw, g.row_h, C_SEL);
            frame(dc, S(1), y, T.cw - S(2), g.row_h, C_ACCENT);
        }

        int x = g.pad + n->depth * S(14);
        if (!n->loaded || n->nkids) draw_arrow(dc, x + S(7), y, n->expanded, C_DIM);
        x += S(16);
        int icon = g.icon_style == ICONS_WINDOWS ? tree_icon(n) : -1;
        if (icon >= 0) draw_shell_icon(dc, icon, x, y, 0, 0);
        else draw_type_icon(dc, (ti_kind)n->ti, x, y, g.row_h, 0, n->kind == NODE_PIN ? C_ACCENT : 0);
        x += S(20);
        text(dc, n->label, -1, x, y, T.cw - x - g.pad, g.row_h, n->failed ? C_DIM : C_TEXT, DT_LEFT);
    }
}

static void tree_move_cursor(int delta_rows)
{
    if (!T.nrows) return;
    int r = T.cursor ? tree_row_of(T.cursor) : -1;
    r = r < 0 ? 0 : r + delta_rows;
    if (r < 0) r = 0;
    if (r >= (int)T.nrows) r = (int)T.nrows - 1;
    T.cursor = T.rows[r];
    tree_ensure_visible(T.cursor);
    tree_redraw();
    tree_open(T.cursor);
}

static tnode *tree_node_at(int y)
{
    y -= g.pad;
    if (y < 0) return NULL;
    int r = T.top + y / g.row_h;
    return r < (int)T.nrows ? T.rows[r] : NULL;
}

/* Rename from the tree: show the folder in the active pane and edit it there. */
static void tree_rename(const wchar_t *path)
{
    const wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash || !slash[1]) {
        set_status(active_pane(), 1, L"Drives can't be renamed here");
        return;
    }
    size_t keep = (size_t)(slash - path);
    if (keep == 2 && path[1] == L':') keep = 3;
    wchar_t *parent = malloc((keep + 1) * sizeof(wchar_t));
    if (!parent) return;
    memcpy(parent, path, keep * sizeof(wchar_t));
    parent[keep] = 0;
    Pane *p = active_pane();
    p_navigate(p, parent, slash + 1, 1);
    wcsncpy(p->want_select, slash + 1, 259);
    p->want_select[259] = 0;
    p->want_rename = 1;
    if (same_path(p->path, parent)) p_apply_want(p);
    focus(p->hwnd);
    free(parent);
}

static void tree_context_menu(tnode *n, POINT screen)
{
    fn_ctx c;
    tree_ctx(&c, n, FN_FROM_MENU);
    if (!c.n) return;
    char verb[32];
    UINT id = show_context_menu(FN_MENU_TREE, &c, screen, verb, sizeof verb);
    run_menu_choice(&c, id, verb);
    ctx_free(&c);
}

static DWORD tree_drop_over(void *ctx, POINT pt, DWORD keys, DWORD allowed, const wchar_t *const *paths, uint32_t n)
{
    static uint64_t scroll_tick;
    uint64_t now = GetTickCount64();
    if (now - scroll_tick > 50) {
        int old = T.top;
        if (pt.y < g.row_h / 2) T.top--;
        else if (pt.y > T.ch - g.row_h / 2) T.top++;
        tree_clamp();
        if (old != T.top) tree_redraw();
        scroll_tick = now;
    }
    tnode *nd = tree_node_at(pt.y);
    DWORD e = nd && nd->kind != NODE_LINUX ? drop_hover_effect(paths, n, nd->path, keys, allowed) : DROPEFFECT_NONE;
    tnode *hl = e ? nd : NULL;
    if (hl != T.drop) {
        T.drop = hl;
        T.drop_since = now;
        tree_redraw();
    } else if (hl && !hl->expanded && now - T.drop_since > 800) { /* hover to open, as in Explorer */
        tree_toggle(hl);
        T.drop_since = now;
    }
    return e;
}

static void tree_drop_leave(void *ctx)
{
    if (T.drop) {
        T.drop = NULL;
        tree_redraw();
    }
}

static DWORD tree_drop(void *ctx, POINT pt, DWORD effect, DWORD keys, DWORD allowed,
                       const wchar_t *const *paths, uint32_t n)
{
    tnode *nd=tree_node_at(pt.y);if(nd&&nd->kind==NODE_LINUX)return DROPEFFECT_NONE;wchar_t *dest=nd?wdup(nd->path):NULL;
    POINT screen=pt;ClientToScreen(T.hwnd,&screen);
    DWORD chosen=drop_commit(active_pane(),screen,dest,effect,keys,allowed,paths,n);
    free(dest);return chosen;
}

static const shell_drop_handler tree_drop_handler = { tree_drop_over, tree_drop_leave, tree_drop };

/* ======================================================================== */
/* Debug mode                                                                 */
/* ======================================================================== */

typedef struct textbuf {
    char  *p;
    size_t cap, len;
} textbuf;

static void tb_put(textbuf *t, const char *fmt, ...)
{
    if (t->len + 1 >= t->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(t->p + t->len, t->cap - t->len, fmt, ap);
    va_end(ap);
    if (n > 0) t->len += (size_t)n < t->cap - t->len ? (size_t)n : t->cap - t->len - 1;
}

static void fmt_bytes_a(char *b, size_t cap, uint64_t v)
{
    static const char *u[] = { "B", "KB", "MB", "GB" };
    double d = (double)v;
    int i = 0;
    while (d >= 1024 && i < 3) { d /= 1024; i++; }
    if (i == 0) snprintf(b, cap, "%llu B", (unsigned long long)v);
    else snprintf(b, cap, "%.1f %s", d, u[i]);
}

static void tree_measure(const tnode *n, uint64_t *nodes, uint64_t *bytes)
{
    (*nodes)++;
    *bytes += sizeof *n + (wcslen(n->path) + 1) * sizeof(wchar_t) + n->nkids * sizeof(tnode *);
    if (n->kind != NODE_FOLDER) *bytes += (wcslen(n->label) + 1) * sizeof(wchar_t);
    for (uint32_t i = 0; i < n->nkids; i++) tree_measure(n->kids[i], nodes, bytes);
}

/* Windows has no per-process thread count API; both ways to get one walk
 * every process on the system. Toolhelp snapshots measured ~40 ms here, a
 * direct NtQuerySystemInformation ~4 ms, so use that, keep its buffer, and
 * refresh at most every 2 s. */
static uint32_t count_threads(void)
{
    typedef LONG(WINAPI * nt_query_fn)(ULONG, PVOID, ULONG, PULONG);
    static nt_query_fn query;
    static uint8_t *buf;
    static ULONG cap;
    static uint32_t cached;
    static uint64_t when;

    uint64_t now = GetTickCount64();
    if (when && now - when < 2000) return cached;
    when = now;
    if (!query) query = (nt_query_fn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
    if (!query) return 0;

    for (int attempt = 0; attempt < 4; attempt++) {
        ULONG need = 0;
        LONG st = buf ? query(5 /* SystemProcessInformation */, buf, cap, &need) : (LONG)0xC0000004;
        if (st == 0) break;
        if (st != (LONG)0xC0000004 /* STATUS_INFO_LENGTH_MISMATCH */) return cached;
        if (buf) VirtualFree(buf, 0, MEM_RELEASE);
        cap = (need ? need : 1u << 20) + (256u << 10);
        buf = VirtualAlloc(NULL, cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!buf) { cap = 0; return cached; }
    }

    DWORD pid = GetCurrentProcessId();
    for (uint8_t *p = buf;;) {
        const SYSTEM_PROCESS_INFORMATION *e = (const SYSTEM_PROCESS_INFORMATION *)p;
        if ((DWORD)(ULONG_PTR)e->UniqueProcessId == pid) {
            cached = e->NumberOfThreads;
            break;
        }
        if (!e->NextEntryOffset) break;
        p += e->NextEntryOffset;
    }
    return cached;
}

static const char *msg_name(UINT m)
{
    switch (m) {
    case WM_PAINT: return "WM_PAINT";
    case WM_KEYDOWN: return "WM_KEYDOWN";
    case WM_SYSKEYDOWN: return "WM_SYSKEYDOWN";
    case WM_CHAR: return "WM_CHAR";
    case WM_LBUTTONDOWN: return "WM_LBUTTONDOWN";
    case WM_LBUTTONDBLCLK: return "WM_LBUTTONDBLCLK";
    case WM_RBUTTONUP: return "WM_RBUTTONUP";
    case WM_MOUSEMOVE: return "WM_MOUSEMOVE";
    case WM_MOUSEWHEEL: return "WM_MOUSEWHEEL";
    case WM_TIMER: return "WM_TIMER";
    case WM_APP_LOADED: return "WM_APP_LOADED";
    case WM_APP_SETTINGS_SAVED: return "WM_APP_SETTINGS_SAVED";
    case WM_APP_JOB: return "WM_APP_JOB";
    case WM_APP_ICONS: return "WM_APP_ICONS";
    case WM_APP_FSOP: return "WM_APP_FSOP";
    case WM_APP_LATE: return "WM_APP_LATE";
    case WM_APP_DROP_WARM: return "WM_APP_DROP_WARM";
    case WM_APP_THUMB: return "WM_APP_THUMB";
    case WM_APP_RELAY: return "WM_APP_RELAY";
    case WM_CONTEXTMENU: return "WM_CONTEXTMENU";
    case WM_RBUTTONDOWN: return "WM_RBUTTONDOWN";
    default: return "other";
    }
}

/* Times every dispatched message while profiling. A message that ran a nested
 * loop (a dialog, a menu, a window drag) is skipped: that time is the user's. */
static void drop_warm_progress(void);

static void dispatch_timed(const MSG *msg)
{
    int64_t t0 = fpe_prof_begin();
    uint32_t seq = g.modal_seq;
    int started = g.startup_ms != 0; /* up to the first frame, time counts as startup */
    if(!msg->hwnd&&msg->message==WM_APP_DROP_WARM)drop_warm_progress();
    else DispatchMessageW(msg);
    if (!t0 || seq != g.modal_seq || !started) return;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    double ms = (double)(now.QuadPart - t0) * 1000.0 / (double)freq.QuadPart;
    FPE_PROF_END("ui.dispatch", t0);
    if (ms > 16.0) FPE_PROF_COUNT("ui.over_16ms");
    if (ms > g.worst_ms) {
        g.worst_ms = ms;
        g.worst_msg = msg->message;
        if (!msg->hwnd || !GetClassNameW(msg->hwnd, g.worst_cls, 32)) wcscpy(g.worst_cls, L"thread");
    }
}

static double filetime_s(FILETIME f) { return (double)(((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) / 1e7; }

/* What the OS and the app's own structures say: the part of the cost that
 * zones and engine gauges cannot see. */
static size_t app_report(char *buf, size_t cap)
{
    textbuf t = { buf, cap, 0 };
    buf[0] = 0;
    HANDLE self = GetCurrentProcess();
    char a[32], b[32];

    PROCESS_MEMORY_COUNTERS_EX pmc = { sizeof pmc };
    GetProcessMemoryInfo(self, (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc);
    FILETIME c, e, k, u;
    GetProcessTimes(self, &c, &e, &k, &u);
    DWORD handles = 0;
    GetProcessHandleCount(self, &handles);

    tb_put(&t, "process\n");
    fmt_bytes_a(a, sizeof a, pmc.WorkingSetSize);
    fmt_bytes_a(b, sizeof b, pmc.PeakWorkingSetSize);
    tb_put(&t, "  working set        %10s  (peak %s)\n", a, b);
    fmt_bytes_a(a, sizeof a, pmc.PrivateUsage);
    tb_put(&t, "  private bytes      %10s\n", a);
    tb_put(&t, "  page faults        %10lu\n", pmc.PageFaultCount);
    tb_put(&t, "  cpu user/kernel    %8.2f s / %.2f s\n", filetime_s(u), filetime_s(k));
    int64_t t_threads = fpe_prof_begin();
    uint32_t threads = count_threads();
    FPE_PROF_END("debug.panel_paint/app_report/threads", t_threads);
    tb_put(&t, "  threads            %10u\n", threads);
    tb_put(&t, "  handles            %10lu\n", handles);
    tb_put(&t, "  gdi / user objects %10lu / %lu\n", GetGuiResources(self, GR_GDIOBJECTS),
           GetGuiResources(self, GR_USEROBJECTS));

    tb_put(&t, "\napp\n");
    uint64_t nodes = 0, tbytes = T.rows_cap * sizeof(tnode *);
    for (uint32_t i = 0; i < T.nroots; i++) tree_measure(T.roots[i], &nodes, &tbytes);
    fmt_bytes_a(a, sizeof a, tbytes);
    tb_put(&t, "  tree               %10llu nodes, %u rows, ~%s\n", (unsigned long long)nodes, T.nrows, a);
    for (int i = 0; i < 2; i++) {
        Pane *p = &g.panes[i];
        fmt_bytes_a(a, sizeof a, p->sel_cap);
        tb_put(&t, "  pane %d             %10u rows, selection %s, history %d\n", i + 1, p_count(p), a,
               p->nback + p->nfwd);
    }
    /* Back buffers are GDI bitmaps: they do not show up in private bytes. */
    uint64_t bb = (uint64_t)g.bb.w * g.bb.h + (uint64_t)T.bb.w * T.bb.h + (uint64_t)g.dbg_bb.w * g.dbg_bb.h;
    for (int i = 0; i < 2; i++) bb += (uint64_t)g.panes[i].bb.w * g.panes[i].bb.h;
    fmt_bytes_a(a, sizeof a, bb * 4);
    tb_put(&t, "  back buffers       %10s  (GDI, outside private bytes)\n", a);

    tb_put(&t, "\nui thread (dialogs, menus and window drags excluded)\n");
    if (g.worst_msg)
        tb_put(&t, "  slowest message    %7.1f ms  %s -> %ls\n", g.worst_ms, msg_name(g.worst_msg), g.worst_cls);
    else
        tb_put(&t, "  slowest message          -\n");

    tb_put(&t, "\nstartup (ms since process created)\n");
    for (int i = 0; i < g_nmarks; i++) tb_put(&t, "  %-18s %10.1f\n", g_marks[i].label, g_marks[i].ms);

    static char fns[6 * 1024];
    fn_report(fns, sizeof fns);
    tb_put(&t, "\n%s", fns);
    return t.len;
}

/* kind,name,calls,total_ms,max_ms,value,peak - for scripts (tests/budget.ps1). */
static size_t csv_report(char *buf, size_t cap)
{
    textbuf t = { buf, cap, 0 };
    buf[0] = 0;
    tb_put(&t, "kind,name,calls,total_ms,max_ms,value,peak\n");
    static fpe_prof_row rows[512];
    uint32_t n = fpe_prof_rows(rows, 512);
    for (uint32_t i = 0; i < n; i++) {
        const fpe_prof_row *r = &rows[i];
        if (r->is_gauge) tb_put(&t, "gauge,%s,,,,%lld,%lld\n", r->name, (long long)r->value, (long long)r->peak);
        else tb_put(&t, "zone,%s,%lld,%.4f,%.4f,,\n", r->name, (long long)r->calls, r->total_ms, r->max_ms);
    }
    HANDLE self = GetCurrentProcess();
    PROCESS_MEMORY_COUNTERS_EX pmc = { sizeof pmc };
    GetProcessMemoryInfo(self, (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc);
    DWORD handles = 0;
    GetProcessHandleCount(self, &handles);
    tb_put(&t, "metric,private_bytes,,,,%llu,\n", (unsigned long long)pmc.PrivateUsage);
    tb_put(&t, "metric,working_set,,,,%llu,%llu\n", (unsigned long long)pmc.WorkingSetSize,
           (unsigned long long)pmc.PeakWorkingSetSize);
    tb_put(&t, "metric,handles,,,,%lu,\n", handles);
    tb_put(&t, "metric,gdi_objects,,,,%lu,\n", GetGuiResources(self, GR_GDIOBJECTS));
    tb_put(&t, "metric,user_objects,,,,%lu,\n", GetGuiResources(self, GR_USEROBJECTS));
    tb_put(&t, "metric,threads,,,,%u,\n", count_threads());
    tb_put(&t, "metric,startup_ms,,,,%.1f,\n", g.startup_ms);
    tb_put(&t, "metric,slowest_message_ms,,,,%.3f,\n", g.worst_ms);
    tb_put(&t, "metric,slowest_message,,,,%s -> %ls,\n", g.worst_msg ? msg_name(g.worst_msg) : "-", g.worst_cls);
    static char fns[8 * 1024];
    fn_report_csv(fns, sizeof fns);
    tb_put(&t, "%s", fns);
    return t.len;
}

static void write_report(const wchar_t *path)
{
    size_t pl = wcslen(path);
    if (pl > 4 && !_wcsicmp(path + pl - 4, L".csv")) {
        static char csv[64 * 1024];
        size_t n = csv_report(csv, sizeof csv);
        HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) return;
        DWORD w;
        WriteFile(f, csv, (DWORD)n, &w, NULL);
        CloseHandle(f);
        return;
    }
    static char zones[64 * 1024], app[16 * 1024];
    fpe_prof_format(zones, sizeof zones);
    app_report(app, sizeof app);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char head[160];
    int hn = snprintf(head, sizeof head, "Folderwing profile  %04u-%02u-%02u %02u:%02u:%02u\n\n", st.wYear, st.wMonth,
                      st.wDay, st.wHour, st.wMinute, st.wSecond);

    HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        set_status(active_pane(), 1, L"Cannot write %ls", path);
        return;
    }
    DWORD w;
    WriteFile(f, head, (DWORD)hn, &w, NULL);
    WriteFile(f, zones, (DWORD)strlen(zones), &w, NULL);
    WriteFile(f, "\n", 1, &w, NULL);
    WriteFile(f, app, (DWORD)strlen(app), &w, NULL);
    CloseHandle(f);
}

static void save_report(void)
{
    wchar_t path[MAX_PATH + 64], tmp[MAX_PATH];
    SYSTEMTIME st;
    GetLocalTime(&st);
    GetTempPathW(MAX_PATH, tmp);
    _snwprintf(path, MAX_PATH + 64, L"%lsopenpilot-profile-%04u%02u%02u-%02u%02u%02u.txt", tmp, st.wYear, st.wMonth,
               st.wDay, st.wHour, st.wMinute, st.wSecond);
    path[MAX_PATH + 63] = 0;
    write_report(path);
    set_status(active_pane(), 0, L"Profile saved: %ls", path);
}

/* Draws newline-separated ASCII text, skipping `skip` lines; returns the
 * number of lines in the text. */
static int draw_lines(HDC dc, const char *s, int x, int y, int w, int line_h, int skip, int bottom)
{
    int line = 0;
    while (*s) {
        const char *e = strchr(s, '\n');
        int len = e ? (int)(e - s) : (int)strlen(s);
        if (line >= skip) {
            int yy = y + (line - skip) * line_h;
            if (yy < bottom) {
                RECT r = { x, yy, x + w, yy + line_h };
                ExtTextOutA(dc, x, yy, ETO_CLIPPED, &r, s, (UINT)len, NULL);
            }
        }
        line++;
        if (!e) break;
        s = e + 1;
    }
    return line;
}

static void dbg_paint(HDC dc)
{
    int64_t t = fpe_prof_begin();
    static char zones[64 * 1024], app[16 * 1024];
    int64_t t_fmt = fpe_prof_begin();
    fpe_prof_format(zones, sizeof zones);
    FPE_PROF_END("debug.panel_paint/format", t_fmt);
    int64_t t_app = fpe_prof_begin();
    app_report(app, sizeof app);
    FPE_PROF_END("debug.panel_paint/app_report", t_app);

    fill(dc, 0, 0, g.dbg_cw, g.dbg_ch, C_TREE_BG);
    fill(dc, 0, 0, g.dbg_cw, g.row_h, C_HEADER);
    SelectObject(dc, g.font);
    wchar_t head[300], k_hide[32], k_reset[32], k_save[32];
    if (!fn_key_text(fn_find("debug.panel"), k_hide, 32)) wcscpy(k_hide, L"-");
    if (!fn_key_text(fn_find("debug.reset"), k_reset, 32)) wcscpy(k_reset, L"-");
    if (!fn_key_text(fn_find("debug.save"), k_save, 32)) wcscpy(k_save, L"-");
    _snwprintf(head, 300, L"DEBUG   profiling %ls%ls      %ls hide    %ls reset    %ls save report    wheel scrolls",
               fpe_prof_enabled() ? L"on" : L"off", g.debug_cli ? L" (since start)" : L"", k_hide, k_reset, k_save);
    head[299] = 0;
    text(dc, head, -1, g.pad * 2, 0, g.dbg_cw - g.pad * 4, g.row_h, C_ACCENT, DT_LEFT);

    SelectObject(dc, g.mono);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    int lh = tm.tmHeight, cw = tm.tmAveCharWidth;
    SetTextColor(dc, C_TEXT);
    int y = g.row_h + g.pad, left_w = cw * 84;
    int right_x = g.pad * 2 + left_w + S(12);
    int n1 = draw_lines(dc, zones, g.pad * 2, y, left_w, lh, g.dbg_top, g.dbg_ch);
    SetTextColor(dc, C_DIM);
    int n2 = draw_lines(dc, app, right_x, y, g.dbg_cw - right_x - g.pad, lh, g.dbg_top, g.dbg_ch);
    g.dbg_lines = n1 > n2 ? n1 : n2;
    FPE_PROF_END("debug.panel_paint", t);
}

/* The repaint timer belongs to debug.panel (functions.c): it ends when the
 * function stops. */
static void show_debug_panel(int on)
{
    g.debug_panel = on;
    if (on) fpe_prof_enable(1);
    else if (!g.debug_cli) fpe_prof_enable(0);
    layout();
}

/* ======================================================================== */
/* Window management                                                          */
/* ======================================================================== */

static void update_title(void)
{
    Pane *p = active_pane();
    if (!p->path) return;
    const wchar_t *name = wcsrchr(p->path, L'\\');
    name = name && name[1] ? name + 1 : p->path;
    wchar_t t[400],display[400]={0};
    if(p_linux(p)||fpe_wsl_native(p->path))name=shown_name(name,(int)wcslen(name),display,399,1);
    _snwprintf(t, 400, L"%ls - Folderwing", name);
    t[399] = 0;
    SetWindowTextW(g.hwnd, t);
    term_note_pane(0);
}

static void set_active(int idx)
{
    if (idx == g.active) return;
    g.active = idx;
    preview_note(active_pane());
    InvalidateRect(g.panes[0].hwnd, NULL, FALSE);
    InvalidateRect(g.panes[1].hwnd, NULL, FALSE);
    update_title();
    tree_reveal(active_pane()->path);
    actiondock_refresh();
}

static void focus_cycle(HWND from, int dir)
{
    HWND order[6];
    if(actiondock_contains(from))from=actiondock_window();
    int n = 0;
    if(actiondock_window()&&IsWindowVisible(actiondock_window()))order[n++]=actiondock_window();
    if (g.show_tree) order[n++] = T.hwnd;
    order[n++] = g.panes[g.pv_swapped].hwnd;
    if (g.pv_shown) {order[n++]=g.pv_shown==5?mediaview_window():g.pv_shown==4?archiveview_window():g.pv_shown==3?textview_window():g.pv_shown==2?imageview_window():pdfview_window();if(g.pv_shown==4&&g.ar_kind)order[n++]=g.ar_kind==5?mediaview_window():g.ar_kind==3?textview_window():g.ar_kind==2?imageview_window():pdfview_window();}
    else if (g.dual) order[n++] = g.panes[1].hwnd;
    int at = 0;
    for (int i = 0; i < n; i++)
        if (order[i] == from) at = i;
    focus(order[(at + dir + n) % n]);
}

static void toggle_tree(void)
{
    g.show_tree = !g.show_tree;
    if (!g.show_tree && GetFocus() == T.hwnd) focus(active_pane()->hwnd);
    layout();
}

static void preview_hide(void);

static void toggle_dual(void)
{
    preview_hide();
    g.dual = !g.dual;
    if (!g.dual && g.active == 1) {
        focus(g.panes[0].hwnd);
        set_active(0);
    }
    layout();
    InvalidateRect(g.panes[0].hwnd, NULL, FALSE);
}

static void toggle_hidden(void)
{
    g.show_hidden = !g.show_hidden;
    for (int i = 0; i < 2; i++) {
        Pane *p = &g.panes[i];
        p->opts.show_hidden = p->opts.show_system = (uint8_t)g.show_hidden;
        if (p->view) p_keep_cursor_after(p, apply_opts);
    }
    for (uint32_t i = 0; i < T.nroots; i++) tree_reapply_loaded(T.roots[i]);
    tree_rebuild();
    main_status_redraw();
}

/* ======================================================================== */
/* PDF preview                                                               */
/* ======================================================================== */

/* The reader takes the right side while the cursor rests on a PDF. When the
 * PDF is in the right pane, the panes swap first so the file list stays
 * next to the reader. It goes once the cursor has been off PDFs for a while,
 * so stepping over a few other files doesn't make the layout jump. */
#define PREVIEW_REST_MS   100
#define PREVIEW_LINGER_MS 5000

static const fn_def *preview_def(void) { return fn_find("preview.pdf"); }
static const fn_def *preview_kind_def(int kind) { return fn_find(kind==5?"preview.media":kind==4?"preview.archive":kind==3?"preview.text":kind==2?"preview.image":"preview.pdf"); }
static HWND archive_content_window(void){return g.ar_kind==5?mediaview_window():g.ar_kind==3?textview_window():g.ar_kind==2?imageview_window():g.ar_kind==1?pdfview_window():NULL;}
static void archive_content_clear(void){int kind=g.ar_kind;if(!kind)return;g.ar_kind=0;if(kind==5)mediaview_clear();else if(kind==3)textview_clear();else if(kind==2)imageview_clear();else pdfview_clear();free(g.ar_path);g.ar_path=NULL;fn_release(preview_kind_def(kind));layout();}
static HWND preview_window(void) { return g.pv_shown==5?mediaview_window():g.pv_shown==4?archiveview_window():g.pv_shown==3?textview_window():g.pv_shown==2?imageview_window():pdfview_window(); }

static int is_pdf(const fpe_entry *e, const wchar_t *name)
{
    return e && !fpe_entry_is_dir(e) && e->name_len - e->ext == 3 && e->ext > 0 &&
           !_wcsicmp(name + e->ext, L"pdf");
}

static int media_candidate(Pane *p,const fpe_entry *e,const wchar_t *name){
 if(!p||!e||fpe_entry_is_dir(e)||!media_extension(name))return 0;
 if(fpe_wsl_path_kind(p->path)&&(!fn_active(fn_find("wsl.helper"))||fpe_wsl_requires_native(p->path)))return 0;
 fpe_unix u;if(p->view&&fpe_dir_unix(fpe_view_dir(p->view),e,&u)&&(u.mode&0170000)!=0100000)return 0;
 return !name||!wcschr(name,FPE_LX_ESCAPE);
}

/* A replaced, bounded demand list: both panes' visible tiles precede a single
 * look-ahead row. Snapshot metadata keys cached pixels, including WSL names. */
static void thumbnails_collect(void)
{
    int any = 0;
    for (int i = 0; i < 2; ++i)
        if (g.panes[i].thumbnails && IsWindowVisible(g.panes[i].hwnd))
            any = 1;
    const fn_def *svc = fn_find("svc.thumbnails");
    if (!any || !g.thumb_on) {
        if (g.thumb_held) {
            thumbnails_submit(NULL, 0);
            fn_release(svc);
            g.thumb_held = 0;
        }
        return;
    }
    if (!g.thumb_held) {
        if (fn_hold(svc))
            return;
        g.thumb_held = 1;
    }
    thumb_want wants[THUMB_WANTS];
    unsigned count = 0;
    for (int pass = 0; pass < 2; ++pass)
        for (int lane = 0; lane < 2; ++lane) {
            Pane *p = &g.panes[g.active ^ lane];
            if (!p->thumbnails || !p->view || !p->path || !IsWindowVisible(p->hwnd))
                continue;
            thumb_grid grid = p_grid(p);
            int edge = S(p->thumb_size);
            if (edge > THUMB_EDGE_MAX)
                edge = THUMB_EDGE_MAX;
            int visible_rows = (p_list_bottom(p) - p_list_top(p) + grid.cell_h - 1) / grid.cell_h;
            if (visible_rows < 1)
                visible_rows = 1;
            uint32_t first = (uint32_t)p->top + (pass ? visible_rows * grid.columns : 0);
            uint32_t end = first + (pass ? grid.columns : visible_rows * grid.columns);
            if (end > p_count(p))
                end = p_count(p);
            for (uint32_t row = first; row < end && count < THUMB_WANTS; ++row) {
                const fpe_entry *e = fpe_view_at(p->view, row);
                const wchar_t *name = fpe_view_name(p->view, row);
                if (fpe_entry_is_dir(e))
                    continue;
                /* Pipes/sockets/devices must never be opened as regular image files. */
                fpe_unix u;
                if (fpe_dir_unix(fpe_view_dir(p->view), e, &u) && (u.mode & 0170000) != 0100000)
                    continue;
                int pdf = is_pdf(e, name);
                if(!pdf&&g.me_on&&media_candidate(p,e,name)&&!media_audio_extension(name))pdf=2;
                if (!pdf && !image_extension(name))
                    continue;
                wchar_t *path = join(p->path, name);
                if (!path)
                    continue;
                wants[count++] = (thumb_want){.path = path,
                                              .size = e->size,
                                              .mtime = e->mtime,
                                              .edge = (uint32_t)edge,
                                              .pdf = pdf,
                                              .visible = !pass};
            }
        }
    thumbnails_submit(wants, count);
    for (unsigned i = 0; i < count; ++i)
        free(wants[i].path);
}

static void preview_show(void);
static void image_browse_reset(void)
{
    imagebrowse_cancel(g.image_worker,++g.image_sequence);imagebrowse_free(g.image_list);g.image_list=NULL;
    g.image_source=NULL;g.image_view=NULL;g.image_revision=0;g.image_row=UINT32_MAX;g.image_walk=0;g.image_list_state=-1;
    imageview_browse(0,0,0,0,-1);
}
static int image_browse_context(Pane *p)
{
    const wchar_t *path=imageview_path(),*slash=path?wcsrchr(path,L'\\'):NULL;
    if(!p||p!=active_pane()||!p->view||!p->path||p->nav_ticket||p->loading||!slash||g.pv_shown!=2)return 0;
    wchar_t parent[IMGP_PATH_MAX];size_t n=slash-path;if(n==2&&path[1]==L':')n++;
    if(n>=IMGP_PATH_MAX)return 0;memcpy(parent,path,n*sizeof *parent);parent[n]=0;return fpe_path_equal(parent,p->path);
}
static void image_browse_note(Pane *p)
{
    if(!g.image_worker||!image_browse_context(p)){if(g.image_source)image_browse_reset();return;}
    uint64_t revision=fpe_view_revision(p->view);
    int fresh=p!=g.image_source||p->view!=g.image_view||revision!=g.image_revision;
    const wchar_t *path=imageview_path(),*name=wcsrchr(path,L'\\')+1;
    const wchar_t *cursor=fpe_view_name(p->view,p->cursor);
    if(fresh){
        imagebrowse_free(g.image_list);g.image_list=NULL;g.image_walk=0;g.image_source=p;g.image_view=p->view;
        g.image_revision=revision;g.image_row=UINT32_MAX;
        g.image_list_state=imagebrowse_submit(g.image_worker,p->view,++g.image_sequence,(uint64_t)g.im_cache_mb<<20)?-1:1;
    }
    if(cursor&&!wcscmp(cursor,name))g.image_row=p->cursor;
    else if(fresh||(g.image_row!=UINT32_MAX&&(!fpe_view_name(p->view,g.image_row)||wcscmp(fpe_view_name(p->view,g.image_row),name))))
        g.image_row=fpe_view_find(p->view,name);
    uint32_t i=imagebrowse_index(g.image_list,g.image_row);
    imageview_browse(i==UINT32_MAX?0:i+1,g.image_list?g.image_list->count:0,
        imagebrowse_neighbor(g.image_list,g.image_row,-1)!=UINT32_MAX&&fn_active(fn_find("image.previous_file")),
        imagebrowse_neighbor(g.image_list,g.image_row,1)!=UINT32_MAX&&fn_active(fn_find("image.next_file")),g.image_list_state);
}
static int image_browse_can(int direction)
{
    Pane *p=g.image_source;
    return g.image_worker&&g.image_list&&g.image_list_state==0&&image_browse_context(p)&&
        p->view==g.image_view&&fpe_view_revision(p->view)==g.image_revision&&
        imagebrowse_neighbor(g.image_list,g.image_row,direction)!=UINT32_MAX;
}
static void image_browse_to(uint32_t row)
{
    Pane *p=g.image_source;if(!p||row>=p_count(p))return;
    g.image_moving=1;p_move_cursor(p,row,0,0);g.image_moving=0;
    KillTimer(g.hwnd,TIMER_PREVIEW);g.image_preserve=1;preview_show();g.image_preserve=0;
}
void app_image_step(int direction)
{
    image_browse_note(g.pv_src);if(!image_browse_can(direction))return;
    g.image_origin=g.image_row;g.image_walk=direction>0?1:-1;
    image_browse_to(imagebrowse_neighbor(g.image_list,g.image_row,direction));
}
void app_image_button(int direction)
{
    fn_ctx context;app_pane_ctx(&context,app_active_pane(),FN_FROM_MOUSE);
    fn_run_id(direction>0?"image.next_file":"image.previous_file",&context);app_ctx_free(&context);
}
void app_image_result(int supported)
{
    image_browse_note(g.pv_src);Pane *p=g.image_source;
    if(!p||!g.image_list||g.image_list_state||g.image_row==UINT32_MAX)return;
    const fpe_entry *e=fpe_view_at(p->view,g.image_row);
    if(!e||!imageview_matches(e->size,e->mtime))return;
    if(supported){g.image_walk=0;return;}
    imagebrowse_remove(g.image_list,g.image_row);int direction=g.image_walk;
    if(direction&&!fn_active(fn_find(direction>0?"image.next_file":"image.previous_file"))){g.image_walk=0;direction=0;}
    uint32_t next=imagebrowse_neighbor(g.image_list,g.image_row,direction);
    image_browse_note(p);
    if(direction&&next!=UINT32_MAX)image_browse_to(next);
    else if(direction){
        g.image_walk=0;
        if(imagebrowse_index(g.image_list,g.image_origin)!=UINT32_MAX)image_browse_to(g.image_origin);
    }
}

/* Archive third-pane playback shares the same source-departure policy. */
static int preview_media_active(void){return g.pv_shown==5||(g.pv_shown==4&&g.ar_kind==5);}

/* Called whenever a pane redraws: cheap unless the cursor moved onto a
 * different PDF or off PDFs. */
static void preview_note(Pane *p)
{
    if ((!g.pv_on && !g.im_on && !g.tx_on && !g.ar_on && !g.me_on) || p != active_pane()) return;
    const wchar_t *name = NULL;
    const fpe_entry *e = NULL;
    if (p->path && p_count(p) && p->cursor < p_count(p)) {
        e = fpe_view_at(p->view, p->cursor);
        name = fpe_view_name(p->view, p->cursor);
    }
    int kind = g.me_on&&media_candidate(p,e,name)?5:g.ar_on&&e&&!fpe_entry_is_dir(e)&&arc_extension(name)?4:g.pv_on && is_pdf(e,name) ? 1 : g.im_on && imagebrowse_candidate(p->view?fpe_view_dir(p->view):NULL,e,name) ? 2 : g.tx_on && e && !fpe_entry_is_dir(e) && text_file_language(name)>=0 ? 3 : 0;
    if (!kind) {
        if(preview_media_active())mediaview_suspend();
        if(!g.image_moving)g.image_walk=0;
        if(g.pv_shown==2)image_browse_note(g.pv_src);
        if (g.pv_want) {
            free(g.pv_want);
            g.pv_want = NULL;
            KillTimer(g.hwnd, TIMER_PREVIEW);
        }
        if (g.pv_shown && !g.pv_lingering) {
            g.pv_lingering = 1;
            /* not from here: this runs inside a pane's redraw */
            SetTimer(g.hwnd, TIMER_LINGER, (g.pv_shown==5?g.me_linger_ms:g.pv_shown==4?5000:g.pv_shown==3?g.tx_linger_ms:g.pv_shown==2?g.im_linger_ms:g.pv_linger_ms) ? (g.pv_shown==5?g.me_linger_ms:g.pv_shown==4?5000:g.pv_shown==3?g.tx_linger_ms:g.pv_shown==2?g.im_linger_ms:g.pv_linger_ms) : 1, NULL);
        }
        return;
    }
    if (g.pv_lingering) {
        g.pv_lingering = 0;
        KillTimer(g.hwnd, TIMER_LINGER);
    }
    wchar_t *path=join(p->path,name);
    if (!path) return;
    if(g.svg_source) {
        if(g.tx_on && fpe_path_equal(g.svg_source,path)) kind=3;
        else {free(g.svg_source);g.svg_source=NULL;}
    }
    if (g.pv_want && g.pv_src==p && g.pv_want_kind==kind && fpe_path_equal(g.pv_want,path) &&
        g.pv_want_size==e->size && g.pv_want_mtime==e->mtime) { free(path);if(kind==2&&g.pv_shown==2)image_browse_note(p);return; }
    if(!g.image_moving)g.image_walk=0;
    if(preview_media_active())mediaview_suspend();
    free(g.pv_want);g.pv_want=path;g.pv_want_kind=kind;g.pv_want_size=e->size;g.pv_want_mtime=e->mtime;
    g.pv_src = p;
    if(g.pv_shown==2)image_browse_note(p);
    SetTimer(g.hwnd, TIMER_PREVIEW, PREVIEW_REST_MS, NULL);
}

static void preview_show(void)
{
    const fn_def *d=preview_kind_def(g.pv_want_kind);
    if (!g.pv_want || !d) return;
    if((g.pv_want_kind>=1&&g.pv_want_kind<=3)||g.pv_want_kind==5)g.ar_readers&=~(1<<(g.pv_want_kind-1));
    if (g.pv_shown && g.pv_shown!=g.pv_want_kind) preview_hide();
    if (!g.pv_shown) {
        if (fn_hold(d)) { if(g.pv_want_kind==5)g.me_on=0;else if(g.pv_want_kind==4)g.ar_on=0;else if(g.pv_want_kind==3)g.tx_on=0;else if(g.pv_want_kind==2)g.im_on=0;else g.pv_on=0;return; }
        g.pv_swapped=g.dual && g.pv_src->idx==1;g.pv_shown=g.pv_want_kind;
        if(g.pv_shown==5)mediaview_open(g.pv_want,g.pv_want_size,g.pv_want_mtime);else if(g.pv_shown==4)archiveview_open(g.pv_want,g.pv_want_size,g.pv_want_mtime);else if(g.pv_shown==3)textview_open(g.pv_want,g.pv_want_size,g.pv_want_mtime);else if(g.pv_shown==2){if(g.image_preserve||image_browse_context(g.pv_src))imageview_open_browse(g.pv_want,g.pv_want_size,g.pv_want_mtime);else imageview_open(g.pv_want,g.pv_want_size,g.pv_want_mtime);}else pdfview_open(g.pv_want,g.pv_want_size,g.pv_want_mtime);
        layout();
    } else if(g.pv_shown==5)mediaview_open(g.pv_want,g.pv_want_size,g.pv_want_mtime);else if(g.pv_shown==4)archiveview_open(g.pv_want,g.pv_want_size,g.pv_want_mtime);else if(g.pv_shown==3)textview_open(g.pv_want,g.pv_want_size,g.pv_want_mtime);
    else if(g.pv_shown==2){if(g.image_preserve||image_browse_context(g.pv_src))imageview_open_browse(g.pv_want,g.pv_want_size,g.pv_want_mtime);else imageview_open(g.pv_want,g.pv_want_size,g.pv_want_mtime);}
    else pdfview_open(g.pv_want,g.pv_want_size,g.pv_want_mtime);
    if(g.pv_shown==2)image_browse_note(g.pv_src);
}

static void preview_hide(void)
{
    KillTimer(g.hwnd, TIMER_PREVIEW);
    KillTimer(g.hwnd, TIMER_LINGER);
    g.pv_lingering = 0;
    image_browse_reset();
    if (!g.pv_shown) return;
    const fn_def *d=preview_kind_def(g.pv_shown);
    HWND f = GetFocus(), reader = preview_window();
    int had_focus = f && reader && (f == reader || IsChild(reader, f)); /* the reader or its find box */
    archive_content_clear();if(g.pv_shown==5)mediaview_clear();else if(g.pv_shown==4)archiveview_clear();else if(g.pv_shown==3)textview_clear();else if(g.pv_shown==2)imageview_clear();else pdfview_clear();
    g.pv_shown = g.pv_swapped = 0;
    layout();
    if (had_focus) focus(active_pane()->hwnd);
    fn_release(d);
}

void app_preview_close(void) { if(g.pv_shown==4&&g.ar_kind)archive_content_clear();else preview_hide(); }
void app_archive_close(void){preview_hide();}
int app_archive_preview_kind(const wchar_t *name){if(!name)return 0;const wchar_t *ext=name?wcsrchr(name,L'.'):NULL;return g.me_on&&media_extension(name)?5:g.pv_on&&ext&&!_wcsicmp(ext,L".pdf")?1:g.im_on&&image_extension(name)?2:g.tx_on&&text_file_language(name)>=0?3:0;}
static void archive_content_show(int kind,const wchar_t *path,uint64_t bytes){
 wchar_t *copy=path?_wcsdup(path):NULL;archive_content_clear();if(!copy||!kind||g.pv_shown!=4){free(copy);return;}const fn_def *d=preview_kind_def(kind);if(fn_hold(d)){free(copy);return;}g.ar_kind=kind;g.ar_readers|=1<<(kind-1);g.ar_path=copy;g.ar_size=bytes;
 if(kind==5)mediaview_open(copy,bytes,0);else if(kind==1)pdfview_open(copy,bytes,0);else if(kind==2)imageview_open(copy,bytes,0);else textview_open(copy,bytes,0);layout();
}
void app_archive_preview(const wchar_t *name,const wchar_t *path,uint64_t bytes){if(!path){archive_content_clear();return;}archive_content_show(app_archive_preview_kind(name),path,bytes);}
static int archive_start(void){return archiveview_start(g.hwnd);}
static void archive_stop(void){if(g.pv_shown==4){archive_content_clear();g.pv_shown=g.pv_swapped=0;layout();}archiveview_stop();}
static void configure_archive(void){g.ar_on=fn_active(fn_find("preview.archive"))&&archiveview_available();if(!g.ar_on&&g.pv_shown==4)preview_hide();if(g.ar_on)preview_note(active_pane());}

static void preview_memory(void);

static int preview_start(void)
{
    int rc = pdfview_start(g.hwnd);
    if (rc) return rc;
    pdfview_set_colors(g.pv_colors);
    preview_memory();
    return 0;
}

/* Colors for this session, in the order a reader most often wants next. */
static void run_preview_colors(fn_ctx *c)
{
    static const uint32_t next[] = { PDFP_COLORS_MIXED, PDFP_COLORS_DARK, PDFP_COLORS_ORIGINAL };
    static const wchar_t *const name[] = { L"original", L"mixed (dark pages, pictures and scans as they are)",
                                           L"dark (every page)" };
    g.pv_colors = next[g.pv_colors % 3];
    pdfview_set_colors(g.pv_colors);
    app_status(NULL, 0, L"PDF colors: %ls", name[g.pv_colors]);
}

/* Ctrl+F and F3 belong to the reader while it shows; otherwise they fall
 * through to the next function bound to them (search with Everything). */
static int preview_shown(const fn_ctx *c) { return g.pv_shown==1||g.ar_kind==1; }
static void run_preview_find(fn_ctx *c) { pdfview_find(); }
static void run_preview_next(fn_ctx *c) { pdfview_find_step(1); }
static void run_preview_prev(fn_ctx *c) { pdfview_find_step(-1); }

static const fn_opt preview_opts[] = {
    { "linger", "5", "seconds the preview stays after the cursor leaves PDFs (0: it goes at once)", "#0..3600" },
    { "colors", "mixed",
      "mixed: text pages dark, pictures and scanned pages as they are; dark: every page; original: as the PDF draws it",
      "mixed dark original" },
    { "remember", "200", "PDFs whose page and zoom are kept across runs, in pdf-positions.txt beside the session (0: none)",
      "#0..10000" },
    { NULL },
};

/* Where positions are kept: beside the session file; no session file, no positions. */
static void preview_memory(void)
{
    wchar_t file[MAX_PATH + 32] = L"";
    if (g.session[0] && g.pv_remember && wcsrchr(g.session, L'\\')) {
        wcscpy(file, g.session);
        _snwprintf(wcsrchr(file, L'\\') + 1, MAX_PATH, L"pdf-positions.txt");
        file[MAX_PATH + 31] = 0;
    }
    pdfview_set_memory(file[0] ? file : NULL, g.pv_remember);
}

static void configure_preview(void)
{
    const fn_def *d = preview_def();
    g.pv_on = fn_active(d) && pdfview_available();
    g.pv_linger_ms = (UINT)atoi(fn_option(d, "linger")) * 1000;
    g.pv_remember = (uint32_t)atoi(fn_option(d, "remember"));
    const char *c = fn_option(d, "colors");
    g.pv_colors = !_stricmp(c, "dark") ? PDFP_COLORS_DARK : !_stricmp(c, "original") ? PDFP_COLORS_ORIGINAL : PDFP_COLORS_MIXED;
    if (!g.hwnd) return; /* startup */
    if (!g.pv_on) {
        if(g.pv_shown==1)preview_hide();else if(g.ar_kind==1)archive_content_clear();
    } else if (fn_running(d)) {
        pdfview_set_colors(g.pv_colors);
        preview_memory();
    }
}

static int image_start(void) {
    int rc=imageview_start(g.hwnd);if(!rc){imageview_configure(g.im_cache_mb,g.im_background);
        g.image_worker=imagebrowse_start(g.hwnd,WM_APP_IMAGE_LIST);g.image_list_state=-1;}return rc;
}
static void image_stop(void) {
    image_browse_reset();imagebrowse_stop(g.image_worker);g.image_worker=NULL;
    MSG message;while(PeekMessageW(&message,g.hwnd,WM_APP_IMAGE_LIST,WM_APP_IMAGE_LIST,PM_REMOVE)){
        if(message.message==WM_QUIT){PostQuitMessage((int)message.wParam);break;}/* Notifications carry no allocated payload. */
    }
    if(g.pv_shown==2){g.pv_shown=g.pv_swapped=0;layout();}imageview_stop();
}
static int image_shown(const fn_ctx *c){return g.pv_shown==2||g.ar_kind==2;}
static void run_image_fit(fn_ctx *c){imageview_action(IMAGE_FIT);}
static void run_image_width(fn_ctx *c){imageview_action(IMAGE_WIDTH);}
static void run_image_actual(fn_ctx *c){imageview_action(IMAGE_ACTUAL);}
static void run_image_zoom_in(fn_ctx *c){imageview_action(IMAGE_ZOOM_IN);}
static void run_image_zoom_out(fn_ctx *c){imageview_action(IMAGE_ZOOM_OUT);}
static void run_image_background(fn_ctx *c){imageview_action(IMAGE_BACKGROUND);}
static void run_image_rotate(fn_ctx *c){imageview_action(IMAGE_ROTATE);}
static void run_image_flip(fn_ctx *c){imageview_action(IMAGE_FLIP);}
static void run_image_metadata(fn_ctx *c){imageview_action(IMAGE_METADATA);}
static void run_image_copy(fn_ctx *c){imageview_action(IMAGE_COPY);}
static void run_image_open(fn_ctx *c){imageview_action(IMAGE_OPEN);}
static int image_animated(const fn_ctx *c){return image_shown(c)&&imageview_animated();}
static void run_image_play(fn_ctx *c){imageview_action(IMAGE_PLAY);}
static void run_image_previous(fn_ctx *c){imageview_action(IMAGE_PREVIOUS);}
static void run_image_next(fn_ctx *c){imageview_action(IMAGE_NEXT);}
static int image_previous_page(const fn_ctx *c){return image_shown(c)&&imageview_page_can(-1);}
static int image_next_page(const fn_ctx *c){return image_shown(c)&&imageview_page_can(1);}
static void run_image_previous_page(fn_ctx *c){imageview_action(IMAGE_PREVIOUS_PAGE);}
static void run_image_next_page(fn_ctx *c){imageview_action(IMAGE_NEXT_PAGE);}
void app_image_page_button(int direction) {
    fn_ctx context;app_pane_ctx(&context,app_active_pane(),FN_FROM_MOUSE);
    fn_run_id(direction>0?"image.next_page":"image.previous_page",&context);app_ctx_free(&context);
}
static int image_previous_file(const fn_ctx *c){return image_browse_can(-1);}
static int image_next_file(const fn_ctx *c){return image_browse_can(1);}
static void run_image_previous_file(fn_ctx *c){app_image_step(-1);}
static void run_image_next_file(fn_ctx *c){app_image_step(1);}
static int svg_path(void){const wchar_t *path=g.ar_path?g.ar_path:g.pv_want;const wchar_t *ext=path?wcsrchr(path,L'.'):NULL;return ext&&!_wcsicmp(ext,L".svg");}
static int image_source_scope(const fn_ctx *c){return (g.pv_shown==2||g.ar_kind==2)&&g.tx_on&&svg_path();}
static int text_image_scope(const fn_ctx *c){return (g.pv_shown==3||g.ar_kind==3)&&g.im_on&&svg_path();}
static void run_image_source(fn_ctx *c){
    if(g.ar_kind==2){archive_content_show(3,g.ar_path,g.ar_size);return;}wchar_t *path=_wcsdup(g.pv_want);if(!path)return;
    free(g.svg_source);g.svg_source=path;
    preview_hide();g.pv_want_kind=3;preview_show();
    if(g.pv_shown==3)focus(textview_window());
}
static void run_text_image(fn_ctx *c){
    if(g.ar_kind==3){archive_content_show(2,g.ar_path,g.ar_size);return;}
    free(g.svg_source);g.svg_source=NULL;
    preview_hide();g.pv_want_kind=2;preview_show();
    if(g.pv_shown==2)focus(imageview_window());
}
static const fn_opt image_opts[]={
    {"linger","5","seconds the preview stays after the cursor leaves images (0: close at once)","#0..3600"},
    {"background","checkerboard","background behind transparent pixels","checkerboard white dark"},
    {"cache_mb","64","encoded image and render cache byte limit in MB; oversized files are refused","#16..128"},
    {NULL}
};
static void configure_image(void) {
    uint32_t old_cap=g.im_cache_mb;
    const fn_def *d=fn_find("preview.image");int was_on=g.im_on;g.im_on=fn_active(d)&&imageview_available();
    g.im_linger_ms=(UINT)atoi(fn_option(d,"linger"))*1000;g.im_cache_mb=(uint32_t)atoi(fn_option(d,"cache_mb"));
    const char *bg=fn_option(d,"background");g.im_background=!_stricmp(bg,"white")?1:!_stricmp(bg,"dark")?2:0;
    if(!g.hwnd)return;
    if(!g.im_on&&g.ar_kind==2)archive_content_clear();if(!g.im_on&&g.pv_shown==2)preview_hide();
    else if(fn_running(d)){if(old_cap!=g.im_cache_mb)image_browse_reset();imageview_configure(g.im_cache_mb,g.im_background);if(g.pv_shown==2)image_browse_note(g.pv_src);}
    if(g.im_on){if(!was_on&&g.pv_want_kind==2){free(g.pv_want);g.pv_want=NULL;}preview_note(active_pane());}
}

static int media_start(void){int result=mediaview_start(g.hwnd);if(!result)mediaview_configure((unsigned)g.me_volume,g.me_muted);return result;}
static void media_stop(void){if(g.ar_kind==5)archive_content_clear();if(g.pv_shown==5){g.pv_shown=g.pv_swapped=0;layout();}mediaview_stop();}
static const fn_opt media_opts[]={
 {"linger","5","seconds the paused preview remains after leaving media files","#0..3600"},
 {"volume","25","initial playback volume in percent","#0..100"},
 {"muted","off","start each media preview muted","on off"},{NULL}
};
static void configure_media(void){const fn_def *d=fn_find("preview.media");int was=g.me_on;g.me_on=fn_active(d)&&mediaview_available();g.me_volume=atoi(fn_option(d,"volume"));g.me_muted=!_stricmp(fn_option(d,"muted"),"on");g.me_linger_ms=(UINT)atoi(fn_option(d,"linger"))*1000;if(!g.hwnd)return;
 if(!g.me_on){if(g.ar_kind==5)archive_content_clear();if(g.pv_shown==5)preview_hide();}else if(fn_running(d))mediaview_configure((unsigned)g.me_volume,g.me_muted);
 if(g.me_on){if(!was&&g.pv_want_kind==5){free(g.pv_want);g.pv_want=NULL;}preview_note(active_pane());}
}

static int text_start(void) {
    int rc=textview_start(g.hwnd);if(!rc)textview_configure(g.tx_limit_mb,g.tx_wrap,g.tx_lines,g.tx_encoding);return rc;
}
static void text_stop(void){if(g.pv_shown==3){g.pv_shown=g.pv_swapped=0;layout();}textview_stop();}
static int text_shown(const fn_ctx *c){return g.pv_shown==3||g.ar_kind==3;}
static int text_key_scope(const fn_ctx *c){return (g.pv_shown==3||g.ar_kind==3)&&(c->from!=FN_FROM_KEY||textview_key_scope());}
static int text_copy_scope(const fn_ctx *c){return text_key_scope(c)&&textview_selected();}
static void run_text_wrap(fn_ctx *c){textview_action(TEXTVIEW_WRAP);}
static void run_text_lines(fn_ctx *c){textview_action(TEXTVIEW_LINES);}
static void run_text_find(fn_ctx *c){textview_action(TEXTVIEW_FIND);}
static void run_text_next(fn_ctx *c){textview_action(TEXTVIEW_NEXT);}
static void run_text_prev(fn_ctx *c){textview_action(TEXTVIEW_PREV);}
static void run_text_select_all(fn_ctx *c){textview_action(TEXTVIEW_SELECT_ALL);}
static void run_text_copy(fn_ctx *c){textview_action(TEXTVIEW_COPY);}
static void run_text_encoding(fn_ctx *c){textview_action(TEXTVIEW_ENCODING);}
static void run_text_open(fn_ctx *c){textview_action(TEXTVIEW_OPEN);}
static const fn_opt text_opts[]={
    {"linger","5","seconds the preview stays after leaving text files (0: close at once)","#0..3600"},
    {"limit_mb","4","maximum file prefix to read in MB (also bounded to 2 million decoded characters)","#1..8"},
    {"wrap","on","wrap long lines to the viewer width","on off"},
    {"lines","on","show physical source line numbers","on off"},
    {"encoding","auto","automatic encoding detection or a forced interpretation","auto utf8 utf16le utf16be utf32le utf32be windows1252"},
    {NULL}
};
static void configure_text(void){
    const fn_def *d=fn_find("preview.text");int was_on=g.tx_on;g.tx_on=fn_active(d)&&textview_available();
    g.tx_linger_ms=(UINT)atoi(fn_option(d,"linger"))*1000;g.tx_limit_mb=(uint32_t)atoi(fn_option(d,"limit_mb"));
    g.tx_wrap=_stricmp(fn_option(d,"wrap"),"off")!=0;g.tx_lines=_stricmp(fn_option(d,"lines"),"off")!=0;
    static const char *const encodings[]={"auto","utf8","utf16le","utf16be","utf32le","utf32be","windows1252"};g.tx_encoding=TEXT_AUTO;
    for(unsigned i=0;i<sizeof encodings/sizeof *encodings;++i)if(!_stricmp(fn_option(d,"encoding"),encodings[i]))g.tx_encoding=i;
    if(!g.hwnd)return;
    if(!g.tx_on&&g.ar_kind==3)archive_content_clear();if(!g.tx_on&&g.pv_shown==3)preview_hide();else if(fn_running(d))textview_configure(g.tx_limit_mb,g.tx_wrap,g.tx_lines,g.tx_encoding);
    if(g.tx_on){if(!was_on&&g.pv_want_kind==3){free(g.pv_want);g.pv_want=NULL;}preview_note(active_pane());}
}

static const fn_opt wsl_opts[] = {
    { "user", "default",
      "default: the distro's default user; root: as root (WSL asks no password for it, so everything is reachable). "
      "A distro already in use keeps its user until its helper stops (60 s after its last folder)",
      "default root" },
    { "columns", "on", "Permissions and Owner columns in WSL folders (when the pane is wide enough)", "on off" },
    { NULL },
};

static void optional_wsl_policy_changed(void);
static void configure_wsl(void)
{
    const fn_def *d = fn_find("wsl.helper");
    g.wsl_columns = _stricmp(fn_option(d, "columns"), "off") != 0;
    fpe_wsl_config cfg = { (uint8_t)fn_active(d), (uint8_t)!_stricmp(fn_option(d, "user"), "root"), 60000 };
    fpe_wsl_configure(&cfg);
    if(!g.destroyed)tree_sync_wsl();
    optional_wsl_policy_changed();
    for (int i = 0; i < 2 && g.hwnd; i++) InvalidateRect(g.panes[i].hwnd, NULL, FALSE);
}

/* Breadcrumbs or a plain text box; the editor stays while someone types in it. */
static void configure_crumbs(void)
{
    g.crumbs = fn_active(fn_find("view.breadcrumbs"));
    for (int i = 0; i < 2 && g.hwnd; i++) {
        Pane *p = &g.panes[i];
        if (!p->edit) continue;
        if (!g.crumbs) {
            if (p->path && !IsWindowVisible(p->edit)) p_address_text(p,1);
            ShowWindow(p->edit, SW_SHOW);
        } else if (GetFocus() != p->edit) {
            ShowWindow(p->edit, SW_HIDE);p_address_text(p,0);
        }
        p_redraw(p);
    }
}

/* ======================================================================== */
/* Side panel and settings                                                   */
/* ======================================================================== */

/* The settings file is watched without a thread: the main loop waits on a
 * change notification for its folder along with messages. After a change,
 * the file is read once nothing has changed for SETTINGS_QUIET_MS (editors
 * write in steps), parsed completely and applied in one go (fn_reload). */
#define SETTINGS_QUIET_MS 300

static RECT     g_operations_button;
static RECT     g_gear;      /* the side panel's button in the status bar */
static HANDLE   g_cfg_watch; /* FindFirstChangeNotification on the settings folder, or NULL */
static uint64_t g_cfg_hash;  /* of the settings text last applied */
/* UI owns this record until its thread exits. The worker owns only copied
 * paths/text; it never uses windows, the live registry or panel app records. */
typedef struct settings_save {
    HANDLE thread;
    wchar_t path[MAX_PATH + 32];
    char *text, *current;
    size_t len, current_len;
    uint64_t expect;
    uint32_t request, generation;
    int64_t started;
    int result, read_result;
    int dock;
} settings_save;
static settings_save *g_settings_save;
static void layout(void);

static uint64_t bytes_hash(const char *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)p[i]) * 1099511628211ull;
    return h;
}

/* 0 = read (*len 0 and *buf NULL for a missing file), -1 = not readable now (an editor has it locked) */
static int settings_read_path(const wchar_t *path, char **buf, size_t *len)
{
    *buf = NULL;
    *len = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? 0 : -1;
    }
    LARGE_INTEGER size;
    DWORD got = 0;
    int rc = -1;
    if (GetFileSizeEx(f, &size) && size.QuadPart < (1 << 20) && (*buf = malloc((size_t)size.QuadPart + 1)) &&
        ReadFile(f, *buf, (DWORD)size.QuadPart, &got, NULL) && got == size.QuadPart) {
        *len = got;
        rc = 0;
    }
    CloseHandle(f);
    if (rc) {
        free(*buf);
        *buf = NULL;
    }
    return rc;
}

static int settings_read(char **buf, size_t *len)
{
    return settings_read_path(g.settings, buf, len);
}

/* The parent directory's final path makes junction/relative aliases share a
 * mutex. A hash collision only serializes unrelated files. */
static HANDLE settings_file_mutex(const wchar_t *path)
{
    wchar_t full[MAX_PATH + 64], parent[MAX_PATH + 64], basename[MAX_PATH + 64], name[80];
    DWORD n = GetFullPathNameW(path, MAX_PATH + 64, full, NULL);
    if (!n || n >= MAX_PATH + 64) return NULL;
    for (wchar_t *p = full; *p; p++) if (*p == L'/') *p = L'\\';
    wchar_t *leaf = wcsrchr(full, L'\\');
    if (!leaf || !leaf[1]) return NULL;
    wcscpy(basename, leaf);
    size_t prefix = (size_t)(leaf - full);
    memcpy(parent, full, prefix * sizeof(wchar_t));
    parent[prefix] = 0;
    if (prefix && parent[prefix - 1] == L':') { parent[prefix++] = L'\\'; parent[prefix] = 0; }
    HANDLE dir = CreateFileW(parent, FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (dir != INVALID_HANDLE_VALUE) {
        wchar_t final[MAX_PATH + 64];
        DWORD got = GetFinalPathNameByHandleW(dir, final, MAX_PATH + 64, FILE_NAME_NORMALIZED);
        CloseHandle(dir);
        if (got && got < MAX_PATH + 64 && (size_t)got + wcslen(basename) < MAX_PATH + 64)
            swprintf(full, MAX_PATH + 64, L"%ls%ls", final, basename);
    }
    uint64_t hash = 14695981039346656037ull;
    for (const wchar_t *p = full; *p; p++) { hash ^= (uint16_t)towlower(*p); hash *= 1099511628211ull; }
    _snwprintf(name, 80, L"Global\\OpenPilotSettings-%016llx", (unsigned long long)hash);
    return CreateMutexW(NULL, FALSE, name);
}

/* Keep the old file open without sharing writes while checking its hash and
 * replacing it. An editor with a live write handle makes this fail closed. */
static int settings_guarded_hash(const wchar_t *path, HANDLE *guard, uint64_t *hash)
{
    *guard = INVALID_HANDLE_VALUE;
    *hash = bytes_hash("", 0);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? 0 : -1;
    }
    LARGE_INTEGER size;
    DWORD got = 0;
    char *buf = NULL;
    int rc = -1;
    if (GetFileSizeEx(f, &size) && size.QuadPart < (1 << 20) &&
        (buf = malloc((size_t)size.QuadPart + 1)) &&
        ReadFile(f, buf, (DWORD)size.QuadPart, &got, NULL) && got == size.QuadPart) {
        *hash = bytes_hash(buf, got);
        *guard = f;
        rc = 0;
    }
    free(buf);
    if (rc) CloseHandle(f);
    return rc;
}

/* Replaces the file atomically. A cooperating writer holds the same mutex
 * from final validation through publication. Return -2 on conflict, -3 if
 * an uncooperative replacement requires a retained recovery backup. */
static int settings_write(const wchar_t *path, const char *text, size_t len, const uint64_t *expect)
{
    HANDLE mutex = settings_file_mutex(path);
    if (!mutex) return -1;
    DWORD waited = WaitForSingleObject(mutex, 5000);
    if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED) { CloseHandle(mutex); return -1; }
    wchar_t tmp[MAX_PATH + 96], backup[MAX_PATH + 96];
    static volatile LONG sequence;
    unsigned serial = (unsigned)InterlockedIncrement(&sequence);
    unsigned long long tick = (unsigned long long)GetTickCount64();
    _snwprintf(tmp, MAX_PATH + 96, L"%ls.%lu.%016llx.%u.tmp", path, GetCurrentProcessId(), tick, serial);
    _snwprintf(backup, MAX_PATH + 96, L"%ls.%lu.%016llx.%u.recovery", path, GetCurrentProcessId(), tick, serial);
    tmp[MAX_PATH + 95] = backup[MAX_PATH + 95] = 0;
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { ReleaseMutex(mutex); CloseHandle(mutex); return -1; }
    DWORD w = 0;
    BOOL ok = WriteFile(f, text, (DWORD)len, &w, NULL) && w == len && FlushFileBuffers(f);
    CloseHandle(f);
    int rc = -1;
    HANDLE guard = INVALID_HANDLE_VALUE;
    uint64_t current = 0;
    if (ok && settings_guarded_hash(path, &guard, &current)) ok = FALSE;
    if (ok && ((expect && current != *expect) || (!expect && guard != INVALID_HANDLE_VALUE)))
        ok = FALSE, rc = -2;
    if (ok) {
        BOOL replaced = guard != INVALID_HANDLE_VALUE;
        if (replaced) ok = ReplaceFileW(path, tmp, backup, 0, NULL, NULL);
        else ok = MoveFileExW(tmp, path, MOVEFILE_WRITE_THROUGH);
        if (ok) {
            rc = 0;
            if (replaced) {
                char *before = NULL;
                size_t before_len = 0;
                if (settings_read_path(backup, &before, &before_len) ||
                    bytes_hash(before ? before : "", before_len) != current) {
                    /* An atomic editor slipped between our read and replace.
                     * Restore its version if ours is still current; otherwise
                     * leave the old version in the recovery file. */
                    char *now = NULL;
                    size_t now_len = 0;
                    if (!settings_read_path(path, &now, &now_len) && now_len == len &&
                        (!len || (now && !memcmp(now, text, len))) &&
                        MoveFileExW(backup, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) rc = -2;
                    else rc = -3;
                    free(now);
                } else DeleteFileW(backup);
                free(before);
            }
        } else if (replaced && GetFileAttributesW(backup) != INVALID_FILE_ATTRIBUTES) rc = -3;
        else if (!replaced && expect && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) rc = -2;
    }
    if (guard != INVALID_HANDLE_VALUE) CloseHandle(guard);
    DeleteFileW(tmp);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return rc;
}

static const wchar_t *settings_app(void)
{
    static wchar_t exe[MAX_PATH + 32];
    if (!exe[0]) {
        DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
        wchar_t *slash = n && n < MAX_PATH ? wcsrchr(exe, L'\\') : NULL;
        if (slash) wcscpy(slash + 1, L"folderwing-settings.exe");
    }
    return exe;
}

static int settings_shown(void)
{
    const wchar_t *a = sidepanel_app();
    return a && !_wcsicmp(a, settings_app());
}

/* Everything the settings app shows, sent whenever it changes. */
static void settings_send(void)
{
    if (!settings_shown()) return;
    int64_t t = fpe_prof_begin();
    char head[MAX_PATH * 3 + 64], path[MAX_PATH * 3] = "";
    if (g.settings[0]) WideCharToMultiByte(CP_UTF8, 0, g.settings, -1, path, sizeof path, NULL, NULL);
    int hn = snprintf(head, sizeof head, "rev\t%u\nfile\t%s\t%d\n", fn_revision(), path,
                      g.settings[0] && GetFileAttributesW(g.settings) != INVALID_FILE_ATTRIBUTES);
    size_t need = fn_describe(NULL, 0);
    char *buf = malloc((size_t)hn + need + 1);
    if (buf) {
        memcpy(buf, head, (size_t)hn);
        fn_describe(buf + hn, need + 1);
        sidepanel_send(settings_app(), PANELP_SETTINGS, buf, (uint32_t)((size_t)hn + need));
        free(buf);
    }
    FPE_PROF_END("settings.snapshot", t);
}

static void settings_status(int changed)
{
    const char *errs = fn_settings_errors();
    if (errs[0]) {
        const char *nl = strchr(errs, '\n');
        app_status(NULL, 1, L"Settings: %.*hs  (Ctrl+, shows them all)", nl ? (int)(nl - errs) : 200, errs);
    } else if (changed >= 0) {
        app_status(NULL, 0, changed == 1 ? L"Settings applied: 1 function changed" : L"Settings applied: %d functions changed",
                   changed);
    }
}

/* New settings text: applied in one go, shown, and sent to the settings app. */
static void settings_apply(const char *buf, size_t len)
{
    g_cfg_hash = bytes_hash(buf ? buf : "", len);
    int64_t t = fpe_prof_begin();
    int changed = fn_reload(buf, len);
    if(g.pv_shown==2){image_browse_note(g.pv_src);imageview_controls_refresh();}
    layout();
    RedrawWindow(g.hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);
    FPE_PROF_END("settings.reload", t);
    settings_status(changed);
    settings_send();
    /* Published after the configuration and its live consumers have changed. */
    SetPropW(g.hwnd, L"Settings.WslRoot", (HANDLE)(uintptr_t)(!_stricmp(fn_option(fn_find("wsl.helper"), "user"), "root")));
    SetPropW(g.hwnd, L"Settings.Revision", (HANDLE)(uintptr_t)fn_revision());
}

/* The file was quiet for SETTINGS_QUIET_MS: apply it if its text changed. */
static void settings_check(void)
{
    if (g_settings_save) {
        SetTimer(g.hwnd, TIMER_SETTINGS, SETTINGS_QUIET_MS, NULL);
        return; /* the worker checks for external edits before replacing the file */
    }
    char *buf;
    size_t len;
    if (settings_read(&buf, &len)) {
        SetTimer(g.hwnd, TIMER_SETTINGS, SETTINGS_QUIET_MS, NULL); /* locked by an editor: soon again */
        return;
    }
    if (bytes_hash(buf ? buf : "", len) != g_cfg_hash) settings_apply(buf, len);
    free(buf);
}

/* The settings folder changed (any file in it): wait for quiet. */
static void settings_poke(void)
{
    FindNextChangeNotification(g_cfg_watch);
    SetTimer(g.hwnd, TIMER_SETTINGS, SETTINGS_QUIET_MS, NULL);
}

static int settings_watch_start(void)
{
    if (!g.settings[0] || g_cfg_watch) return 0;
    wchar_t dir[MAX_PATH + 32];
    wcscpy(dir, g.settings);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (!slash) return 0;
    *slash = 0;
    /* no folder yet (%APPDATA%\Folderwing before settings.edit makes it): nothing to watch */
    g_cfg_watch = FindFirstChangeNotificationW(dir, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME |
                                                            FILE_NOTIFY_CHANGE_SIZE);
    if (g_cfg_watch == INVALID_HANDLE_VALUE) g_cfg_watch = NULL;
    if (g_cfg_watch && g.hwnd) SetPropW(g.hwnd, L"Settings.Watching", (HANDLE)(uintptr_t)1);
    return 0;
}

static void settings_watch_stop(void)
{
    if (g_cfg_watch) FindCloseChangeNotification(g_cfg_watch);
    g_cfg_watch = NULL;
    if (g.hwnd) { KillTimer(g.hwnd, TIMER_SETTINGS); RemovePropW(g.hwnd, L"Settings.Watching"); }
}

static DWORD WINAPI edit_thread(void *arg)
{
    wchar_t *path = arg;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SHELLEXECUTEINFOW ei = { sizeof ei };
    ei.fMask = SEE_MASK_FLAG_NO_UI;
    ei.lpVerb = L"edit";
    ei.lpFile = path;
    ei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&ei)) {
        wchar_t args[MAX_PATH + 40];
        _snwprintf(args, MAX_PATH + 40, L"\"%ls\"", path);
        args[MAX_PATH + 39] = 0;
        ei.lpVerb = NULL;
        ei.lpFile = L"notepad.exe";
        ei.lpParameters = args;
        ShellExecuteExW(&ei);
    }
    CoUninitialize();
    free(path);
    return 0;
}

/* %APPDATA%\Folderwing may not exist before the first settings are saved. */
static void settings_folder(void)
{
    wchar_t dir[MAX_PATH + 32];
    wcscpy(dir, g.settings);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) {
        *slash = 0;
        CreateDirectoryW(dir, NULL);
    }
}

/* Opens the settings file in an editor; a missing one is made first, listing everything commented out. */
static void run_settings_edit(fn_ctx *c)
{
    if (!g.settings[0]) {
        app_status(NULL, 1, L"Started with --settings= : these settings are the defaults, not a file");
        return;
    }
    if (GetFileAttributesW(g.settings) == INVALID_FILE_ATTRIBUTES) {
        if (g_settings_save) {
            app_status(NULL, 0, L"Settings are being saved; open the file once saving finishes");
            return;
        }
        settings_folder();
        size_t n = fn_template(NULL, 0);
        char *t = malloc(n + 1);
        int bad = !t;
        if (t) {
            fn_template(t, n + 1);
            bad = settings_write(g.settings, t, n, NULL);
            if (!bad) g_cfg_hash = bytes_hash(t, n); /* comments only: nothing to apply */
            free(t);
        }
        if (bad == -2) { /* another instance created it while we prepared the template */
            settings_check();
            bad = 0;
        }
        if (bad) {
            app_status(NULL, 1, L"Could not create %ls", g.settings);
            return;
        }
        if (fn_running(fn_find("settings.watch"))) settings_watch_start();
        settings_send();
    }
    wchar_t *path = wdup(g.settings);
    HANDLE t = path ? CreateThread(NULL, 64 * 1024, edit_thread, path, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL) : NULL;
    if (t) CloseHandle(t);
    else free(path);
    app_status(NULL, 0, L"Opening %ls: saved changes apply at once", wcsrchr(g.settings, L'\\') + 1);
}

static int g_settings_open_builder;
static void panel_closed(int had_focus)
{
    g_settings_open_builder=0;
    if (had_focus) focus(active_pane()->hwnd);
    InvalidateRect(g.hwnd, NULL, FALSE);
}

/* ---- the panel terminal (folderwing-terminal.exe) ---------------------------------------------------- */

static const wchar_t *term_app(void)
{
    static wchar_t exe[MAX_PATH + 32];
    if (!exe[0]) {
        DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
        wchar_t *slash = n && n < MAX_PATH ? wcsrchr(exe, L'\\') : NULL;
        if (slash) wcscpy(slash + 1, L"folderwing-terminal.exe");
    }
    return exe;
}

static int term_shown(void)
{
    const wchar_t *a = sidepanel_app();
    return a && !_wcsicmp(a, term_app());
}

static wchar_t g_term_pane[MAX_PATH]; /* the folder the terminal was last told */

static const char *term_follow(void) { return fn_option(fn_find("terminal.show"), "follow"); }

static void term_config(void)
{
    const fn_def *d = fn_find("terminal.show");
    const char *f = term_follow(), *sh = fn_option(fn_find("shell.terminal"), "shell");
    char buf[512];
    int n = snprintf(buf, sizeof buf, "font\t%s\nsize\t%s\nscrollback\t%s\nhistory\t%s\nfollow\t%s\nshell\t%s\n",
                     fn_option(d, "font"), fn_option(d, "size"), fn_option(d, "scrollback"), fn_option(d, "history"),
                     !_stricmp(f, "both") || !_stricmp(f, "shell") ? "on" : "off", !_stricmp(sh, "profile") ? "auto" : sh);
    if (n > 0 && n < (int)sizeof buf) sidepanel_send(term_app(), PANELP_TERM_CONFIG, buf, (uint32_t)n);
}

/* The active pane's folder, for new sessions and for an idle shell to follow. */
static void term_note_pane(int force)
{
    const wchar_t *p = active_pane()->path;
    if (!p || !sidepanel_running(term_app()) || (!force && same_path(p, g_term_pane))) return;
    char buf[PATHWIRE_PREFIX_LEN + MAX_PATH * 4 + 1];
    size_t n = pathwire_encode(p, buf, sizeof buf);
    if (n && sidepanel_send(term_app(), PANELP_TERM_PANE, buf, (uint32_t)n)) {
        _snwprintf(g_term_pane, MAX_PATH, L"%ls", p);
        g_term_pane[MAX_PATH - 1] = 0;
    }
}

/* Shows the terminal; with dir, a new session there (cmd: that program instead of a shell). */
static int term_open(const wchar_t *dir, const wchar_t *cmd, const wchar_t *title)
{
    if (GetFileAttributesW(term_app()) == INVALID_FILE_ATTRIBUTES) {
        app_status(NULL, 1, L"The terminal is missing (folderwing-terminal.exe; build it after ./tools/get-libvterm.ps1)");
        return -1;
    }
    int was = sidepanel_running(term_app());
    if (sidepanel_show(g.hwnd, term_app(), SIDEPANEL_KEEP, 1) < 0) {
        app_status(NULL, 1, L"The side panel could not open");
        return -1;
    }
    if (!was) term_config();
    term_note_pane(!was);
    if (dir) {
        size_t cap = 4 * (wcslen(dir) + (cmd ? wcslen(cmd) : 0) + (title ? wcslen(title) : 0)) + 64;
        char *buf = malloc(cap);
        int ok = 0;
        if (buf) {
            size_t n = 6;
            memcpy(buf, "dir16\t", n);
            size_t pn = pathwire_encode(dir, buf + n, cap - n);
            if (pn) {
                n += pn;
                memcpy(buf + n, "\ncmd\t", 5); n += 5;
                int cn = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, cmd ? cmd : L"", -1,
                                             buf + n, (int)(cap - n), NULL, NULL);
                if (cn > 0) {
                    n += (size_t)cn - 1;
                    memcpy(buf + n, "\ntitle\t", 7); n += 7;
                    int tn = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, title ? title : L"", -1,
                                                 buf + n, (int)(cap - n), NULL, NULL);
                    if (tn > 0) {
                        n += (size_t)tn - 1;
                        buf[n++] = '\n';
                        ok = sidepanel_send(term_app(), PANELP_TERM_OPEN, buf, (uint32_t)n);
                    }
                }
            }
        }
        free(buf);
        if (!ok) return -1;
    }
    InvalidateRect(g.hwnd, NULL, FALSE);
    return 0;
}

int app_terminal_run(const wchar_t *dir, const wchar_t *cmd, const wchar_t *title) { return term_open(dir, cmd, title); }

/* The shell changed folder: the pane follows, unless told not to. */
static void term_cwd(const char *wire, uint32_t size)
{
    const char *f = term_follow();
    if (!term_shown() || (_stricmp(f, "both") && _stricmp(f, "pane"))) return;
    wchar_t path[MAX_PATH];
    if (!pathwire_decode(wire, size, path, MAX_PATH)) return;
    Pane *p = active_pane();
    if (p->path && same_path(p->path, path)) return;
    _snwprintf(g_term_pane, MAX_PATH, L"%ls", path); /* the pane goes where the shell is: no need to tell it back */
    p_navigate(p, path, NULL, 1);
}

static void run_terminal_show(fn_ctx *c)
{
    if (term_shown()) sidepanel_close();
    else term_open(sidepanel_running(term_app()) ? NULL : active_pane()->path, NULL, NULL);
    InvalidateRect(g.hwnd, NULL, FALSE);
}

static void configure_terminal(void)
{
    if (sidepanel_running(term_app())) term_config();
}

static const fn_opt term_opts[] = {
    { "follow", "both", "both: the pane follows the shell's cd, and an idle shell goes where the pane goes; "
                        "pane or shell: only that one follows; off", "both pane shell off" },
    { "font", "Cascadia Mono", "a monospaced font (Consolas if it isn't installed)" },
    { "size", "10", "font size, points", "#6..32" },
    { "scrollback", "5000", "lines kept per session", "#100..100000" },
    { "history", "10", "finished sessions kept, with their output (0: none)", "#0..50" },
    { NULL },
};

/* Host-issued roots bind panel actions to the scan that produced them. */
static wchar_t *g_space_root;
static space_open g_space_request;
static uint32_t g_space_state=SP_STOPPED;
static const wchar_t *space_app(void){static wchar_t path[32768];if(!path[0]){GetModuleFileNameW(NULL,path,32768);wchar_t *slash=wcsrchr(path,L'\\');if(slash)wcscpy(slash+1,L"folderwing-space.exe");}return path;}
static void space_send_root(void){if(!g_space_root)return;sidepanel_send(space_app(),PANELP_SPACE_BROWSER,&g_space_request,sizeof g_space_request);size_t bytes=(wcslen(g_space_root)+1)*2;uint8_t *data=malloc(sizeof g_space_request+bytes);if(data){memcpy(data,&g_space_request,sizeof g_space_request);memcpy(data+sizeof g_space_request,g_space_root,bytes);sidepanel_send(space_app(),PANELP_SPACE_OPEN,data,(uint32_t)(sizeof g_space_request+bytes));free(data);}}
static void space_open_root(const wchar_t *root){if(!root||!*root)return;wchar_t *copy=wdup(root);if(!copy)return;
 free(g_space_root);g_space_root=copy;g_space_request.session++;if(!g_space_request.session)g_space_request.session++;
 g_space_request.wsl=fn_active(fn_find("wsl.helper"));g_space_request.as_root=!_stricmp(fn_option(fn_find("wsl.helper"),"user"),"root");
 g_space_state=SP_RUNNING;if(sidepanel_show(g.hwnd,space_app(),SIDEPANEL_KEEP,1)<0){g_space_state=SP_STOPPED;app_status(NULL,1,L"Space analyzer could not open (folderwing-space.exe is needed)");}else space_send_root();layout();main_status_redraw();}
static void run_analyze_space(fn_ctx *c){const wchar_t *root=c->dir;if(c->n){wchar_t **paths=fn_paths(c);if(!paths||c->n!=1||c->dirs!=1)return;root=paths[0];}space_open_root(root);}
static void run_space_show(fn_ctx *c){(void)c;if(!g_space_root){const wchar_t *path=active_pane()->path;wchar_t root[512],distro[128],linux[32768];if(path&&path[1]==L':'){root[0]=path[0];root[1]=L':';root[2]=L'\\';root[3]=0;space_open_root(root);}else if(path&&term_wsl_split(path,distro,128,linux,32768)){swprintf(root,512,L"\\\\wsl.localhost\\%ls\\",distro);space_open_root(root);}else space_open_root(L"C:\\");}else if(!sidepanel_running(space_app()))space_open_root(g_space_root);else sidepanel_toggle(g.hwnd,space_app(),SIDEPANEL_KEEP);}
static void configure_space(void){if(!fn_active(fn_find("file.analyze_space"))){sidepanel_stop_app(space_app());g_space_state=SP_STOPPED;return;}int wsl=fn_active(fn_find("wsl.helper"));if(g_space_request.wsl!=(uint32_t)wsl){g_space_request.wsl=wsl;if(!wsl&&g_space_root&&fpe_wsl_path_kind(g_space_root)){sidepanel_stop_app(space_app());free(g_space_root);g_space_root=NULL;g_space_request.session++;g_space_state=SP_STOPPED;}else if(sidepanel_running(space_app()))sidepanel_send(space_app(),PANELP_SPACE_BROWSER,&g_space_request,sizeof g_space_request);}}
static const wchar_t *g_space_target;
static uint32_t g_space_target_flags;
static void run_space_result(fn_ctx *c){(void)c;if(!g_space_target)return;const char *id=fn_self()->id;
 if(!strcmp(id,"space.copy_path")){uint64_t ticket=app_clipboard_begin(g.hwnd);size_t n=(wcslen(g_space_target)+1)*2;HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,n);void *p=memory?GlobalLock(memory):NULL;if(p){memcpy(p,g_space_target,n);GlobalUnlock(memory);}else{if(memory)GlobalFree(memory);memory=NULL;}app_clipboard_submit(ticket,CF_UNICODETEXT,memory,0,NULL,L"Space result path copied");}
 else if(!strcmp(id,"space.analyze_folder")){if(g_space_target_flags&SP_DIR)space_open_root(g_space_target);}
 else{sidepanel_close();int side=!strcmp(id,"space.reveal_left")?0:!strcmp(id,"space.reveal_right")?1:active_pane()->idx;preview_hide();if(!strcmp(id,"space.preview")){free(g.pv_want);g.pv_want=NULL;}if(side==1&&!g.dual)g.dual=1;set_active(side);if(!(g_space_target_flags&SP_DIR)||strcmp(id,"space.preview"))app_reveal(&g.panes[side],g_space_target);else p_navigate(&g.panes[side],g_space_target,NULL,1);layout();}}
static void space_panel_action(const void *data,uint32_t size){if(size<sizeof(space_action)+2||(size-sizeof(space_action))%2)return;space_action action;memcpy(&action,data,sizeof action);
 const wchar_t *shown=sidepanel_app();if(!shown||_wcsicmp(shown,space_app())||!sidepanel_running(space_app())||!fn_active(fn_find("file.analyze_space"))||action.session!=g_space_request.session||action.action>SPACE_MENU)return;
 const wchar_t *path=(const wchar_t*)((const uint8_t*)data+sizeof action);size_t chars=(size-sizeof action)/2;if(chars>32768||path[chars-1]||wcslen(path)!=chars-1||!space_target_valid(g_space_root,path))return;
 static const char *ids[]={"space.reveal_left","space.reveal_right","space.preview","space.copy_path","space.analyze_folder"};uint32_t choice=action.action;
 wchar_t *owned=wdup(path);if(!owned)return;uint64_t session=g_space_request.session;
 if(choice==SPACE_MENU){static const wchar_t *labels[]={L"Reveal in left pane",L"Reveal in right pane",L"Preview",L"Copy path",L"Analyze this folder"};HMENU menu=CreatePopupMenu();for(uint32_t i=0;i<5;i++)if(i!=4||action.flags&SP_DIR)AppendMenuW(menu,MF_STRING,i+1,labels[i]);POINT point;GetCursorPos(&point);UINT selected=app_menu_track(menu,g.hwnd,point);DestroyMenu(menu);choice=selected?selected-1:SPACE_MENU;}
 if(choice<5&&session==g_space_request.session&&fn_active(fn_find("file.analyze_space"))){g_space_target=owned;g_space_target_flags=action.flags;pane_run(active_pane(),ids[choice],FN_FROM_PANEL);g_space_target=NULL;}free(owned);}


/* Only host-frozen roots authorize result navigation. */
static wchar_t *g_duplicate_roots[DUPLICATE_ROOTS];
static duplicate_open g_duplicate_request;
static duplicate_state g_duplicate_state;
static const wchar_t *duplicate_app(void){static wchar_t path[32768];if(!path[0]){GetModuleFileNameW(NULL,path,32768);wchar_t *slash=wcsrchr(path,L'\\');if(slash)wcscpy(slash+1,L"folderwing-duplicates.exe");}return path;}
static void duplicate_send_roots(void){if(!g_duplicate_request.count)return;size_t n=sizeof g_duplicate_request;for(uint32_t i=0;i<g_duplicate_request.count;i++)n+=g_duplicate_request.chars[i]*2;uint8_t *data=malloc(n);if(!data)return;memcpy(data,&g_duplicate_request,sizeof g_duplicate_request);size_t at=sizeof g_duplicate_request;for(uint32_t i=0;i<g_duplicate_request.count;i++){size_t bytes=g_duplicate_request.chars[i]*2;memcpy(data+at,g_duplicate_roots[i],bytes);at+=bytes;}sidepanel_send(duplicate_app(),PANELP_DUPLICATE_OPEN,data,(uint32_t)n);free(data);}
static void duplicate_open_roots(const wchar_t *const *roots,uint32_t count){if(!count||count>DUPLICATE_ROOTS)return;wchar_t *copies[DUPLICATE_ROOTS]={0};for(uint32_t i=0;i<count;i++){if(!roots[i]||!*roots[i]||wcslen(roots[i])>=32768){for(uint32_t j=0;j<i;j++)free(copies[j]);return;}copies[i]=wdup(roots[i]);if(!copies[i]){for(uint32_t j=0;j<i;j++)free(copies[j]);return;}}
 for(uint32_t i=0;i<DUPLICATE_ROOTS;i++){free(g_duplicate_roots[i]);g_duplicate_roots[i]=copies[i];g_duplicate_request.chars[i]=copies[i]?(uint32_t)wcslen(copies[i])+1:0;}g_duplicate_request.count=count;g_duplicate_request.session++;if(!g_duplicate_request.session)g_duplicate_request.session++;g_duplicate_request.wsl=fn_active(fn_find("wsl.helper"));g_duplicate_request.as_root=!_stricmp(fn_option(fn_find("wsl.helper"),"user"),"root");memset(&g_duplicate_state,0,sizeof g_duplicate_state);g_duplicate_state.session=g_duplicate_request.session;
 if(sidepanel_show(g.hwnd,duplicate_app(),SIDEPANEL_KEEP,1)<0)app_status(NULL,1,L"Duplicate finder could not open (folderwing-duplicates.exe is needed)");else duplicate_send_roots();}
static int duplicate_applies(const fn_ctx *c){return c->dir&&(!c->n||(c->n==c->dirs&&c->n<=DUPLICATE_ROOTS));}
static void run_find_duplicates(fn_ctx *c){if(c->n){wchar_t **paths=fn_paths(c);if(paths&&c->n==c->dirs&&c->n<=DUPLICATE_ROOTS)duplicate_open_roots((const wchar_t *const*)paths,c->n);}else{const wchar_t *root=c->dir;duplicate_open_roots(&root,1);}}
static void run_duplicates_both(fn_ctx *c){(void)c;const wchar_t *roots[]={g.panes[0].path,g.panes[1].path};if(g.dual)duplicate_open_roots(roots,2);}
static void run_duplicates_show(fn_ctx *c){if(!g_duplicate_request.count)run_find_duplicates(c);else if(!sidepanel_running(duplicate_app()))duplicate_open_roots((const wchar_t *const*)g_duplicate_roots,g_duplicate_request.count);else sidepanel_toggle(g.hwnd,duplicate_app(),SIDEPANEL_KEEP);}
static void configure_duplicates(void){if(!fn_active(fn_find("file.find_duplicates"))){sidepanel_stop_app(duplicate_app());memset(&g_duplicate_state,0,sizeof g_duplicate_state);}}
static const wchar_t *g_duplicate_target;
static void run_duplicate_result(fn_ctx *c){(void)c;if(!g_duplicate_target)return;const char *id=fn_self()->id;if(!strcmp(id,"duplicates.copy_path")){uint64_t ticket=app_clipboard_begin(g.hwnd);size_t n=(wcslen(g_duplicate_target)+1)*2;HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,n);void *p=memory?GlobalLock(memory):NULL;if(p){memcpy(p,g_duplicate_target,n);GlobalUnlock(memory);}else{if(memory)GlobalFree(memory);memory=NULL;}app_clipboard_submit(ticket,CF_UNICODETEXT,memory,0,NULL,L"Duplicate path copied");return;}
 int side=!strcmp(id,"duplicates.reveal_left")?0:!strcmp(id,"duplicates.reveal_right")?1:g.active;preview_hide();if(!strcmp(id,"duplicates.preview")){free(g.pv_want);g.pv_want=NULL;}if(side==1&&!g.dual)g.dual=1;set_active(side);app_reveal(&g.panes[side],g_duplicate_target);layout();}
static void duplicate_panel_action(const void *data,uint32_t size){if(size<sizeof(duplicate_action)+4||(size-sizeof(duplicate_action))%2)return;duplicate_action a;memcpy(&a,data,sizeof a);const wchar_t *shown=sidepanel_app();if(!shown||_wcsicmp(shown,duplicate_app())||!sidepanel_running(duplicate_app())||!fn_active(fn_find("file.find_duplicates"))||a.session!=g_duplicate_request.session||a.generation!=g_duplicate_state.generation||!a.generation||g_duplicate_state.state<4||a.root>=g_duplicate_request.count||a.action>DUPLICATE_MENU)return;
 const wchar_t *rel=(const wchar_t*)((const uint8_t*)data+sizeof a);size_t chars=(size-sizeof a)/2;if(chars>32768||rel[chars-1]||wcslen(rel)!=chars-1||!duplicate_relative_safe(rel))return;wchar_t *owned=join(g_duplicate_roots[a.root],rel);if(!owned||!space_target_valid(g_duplicate_roots[a.root],owned)){free(owned);return;}uint32_t choice=a.action;uint64_t session=a.session;uint32_t generation=a.generation;
 if(choice==DUPLICATE_MENU){static const wchar_t *labels[]={L"Reveal in left pane",L"Reveal in right pane",L"Preview",L"Copy path"};HMENU menu=CreatePopupMenu();for(uint32_t i=0;i<4;i++)AppendMenuW(menu,MF_STRING,i+1,labels[i]);POINT point;GetCursorPos(&point);UINT selected=app_menu_track(menu,g.hwnd,point);DestroyMenu(menu);choice=selected?selected-1:DUPLICATE_MENU;}
 if(choice<4&&session==g_duplicate_request.session&&generation==g_duplicate_state.generation&&fn_active(fn_find("file.find_duplicates"))){static const char *ids[]={"duplicates.reveal_left","duplicates.reveal_right","duplicates.preview","duplicates.copy_path"};g_duplicate_target=owned;pane_run(active_pane(),ids[choice],FN_FROM_PANEL);g_duplicate_target=NULL;}free(owned);}

static wchar_t *g_diff_paths[2];
static diff_open g_diff_request;
static git_diff_open g_diff_git;
static diff_state g_diff_state;
static const wchar_t *diff_app(void){static wchar_t path[32768];if(!path[0]){GetModuleFileNameW(NULL,path,32768);wchar_t *slash=wcsrchr(path,L'\\');if(slash)wcscpy(slash+1,L"folderwing-diff.exe");}return path;}
static void diff_send_pair(void){if(g_diff_request.reserved){if(!g_diff_paths[0]||!g_diff_paths[1])return;g_diff_git.root_chars=(uint32_t)wcslen(g_diff_paths[0])+1;g_diff_git.path_chars=(uint32_t)wcslen(g_diff_paths[1])+1;size_t a=g_diff_git.root_chars*2,b=g_diff_git.path_chars*2,n=sizeof g_diff_git+a+b;uint8_t *p=malloc(n);if(p){memcpy(p,&g_diff_git,sizeof g_diff_git);memcpy(p+sizeof g_diff_git,g_diff_paths[0],a);memcpy(p+sizeof g_diff_git+a,g_diff_paths[1],b);sidepanel_send(diff_app(),PANELP_DIFF_GIT_OPEN,p,(uint32_t)n);free(p);}return;}if(!g_diff_paths[0]||!g_diff_paths[1])return;size_t a=(wcslen(g_diff_paths[0])+1)*2,b=(wcslen(g_diff_paths[1])+1)*2;uint8_t *p=malloc(sizeof g_diff_request+a+b);if(!p)return;memcpy(p,&g_diff_request,sizeof g_diff_request);memcpy(p+sizeof g_diff_request,g_diff_paths[0],a);memcpy(p+sizeof g_diff_request+a,g_diff_paths[1],b);sidepanel_send(diff_app(),PANELP_DIFF_OPEN,p,(uint32_t)(sizeof g_diff_request+a+b));free(p);}
static int diff_applies(const fn_ctx *c){if(!g.dual||!c->pane||c->n!=1||(c->state&FN_S_DIR))return 0;Pane *other=app_other_pane(c->pane);if(!other||!other->path||other->nsel>1||!p_count(other))return 0;const fpe_entry *e=fpe_view_at(other->view,other->cursor);return e&&!fpe_entry_is_dir(e);}
static void run_compare_files(fn_ctx *c){if(!diff_applies(c))return;wchar_t **paths=fn_paths(c);if(!paths||c->n!=1||c->dirs)return;fn_ctx other;app_pane_ctx(&other,app_other_pane(c->pane),FN_FROM_CODE);wchar_t **right=fn_paths(&other);if(!right||other.n!=1||other.dirs){app_ctx_free(&other);app_status(c->pane,1,L"Select one regular file in each pane");return;}
 int side=((Pane*)c->pane)->idx;wchar_t *a=wdup(side?right[0]:paths[0]),*b=wdup(side?paths[0]:right[0]);app_ctx_free(&other);if(!a||!b){free(a);free(b);return;}
 if(provider_path_check(a,fn_active(fn_find("wsl.helper")))||provider_path_check(b,fn_active(fn_find("wsl.helper")))){free(a);free(b);app_status(c->pane,1,L"These paths are unsupported by Windows file comparison");return;}
 free(g_diff_paths[0]);free(g_diff_paths[1]);g_diff_paths[0]=a;g_diff_paths[1]=b;g_diff_request.session++;if(!g_diff_request.session)g_diff_request.session++;g_diff_request.reserved=0;g_diff_request.wsl=fn_active(fn_find("wsl.helper"));g_diff_request.left_chars=(uint32_t)wcslen(a)+1;g_diff_request.right_chars=(uint32_t)wcslen(b)+1;memset(&g_diff_state,0,sizeof g_diff_state);preview_hide();
 if(sidepanel_show(g.hwnd,diff_app(),SIDEPANEL_KEEP,1)<0)app_status(c->pane,1,L"File differences could not open (folderwing-diff.exe is needed)");else diff_send_pair();}
static void run_diff_show(fn_ctx *c){if(g_diff_paths[0]&&sidepanel_running(diff_app()))sidepanel_toggle(g.hwnd,diff_app(),SIDEPANEL_KEEP);else if(g_diff_paths[0]){if(sidepanel_show(g.hwnd,diff_app(),SIDEPANEL_KEEP,1)>=0)diff_send_pair();}else run_compare_files(c);}
static void configure_diff(void){if(!fn_active(fn_find("file.compare_files"))){sidepanel_stop_app(diff_app());for(int i=0;i<2;i++){free(g_diff_paths[i]);g_diff_paths[i]=NULL;}memset(&g_diff_state,0,sizeof g_diff_state);}}


static wchar_t *g_git_folder;
static git_open g_git_request;
static git_state g_git_state;
static const wchar_t *git_app(void){static wchar_t path[32768];if(!path[0]){GetModuleFileNameW(NULL,path,32768);wchar_t *p=wcsrchr(path,L'\\');if(p)wcscpy(p+1,L"folderwing-git.exe");}return path;}
static void git_redraw(void){for(int i=0;i<2;i++)if(g.panes[i].hwnd)p_redraw(&g.panes[i]);}
static void git_send_root(void){if(!g_git_folder)return;size_t bytes=(wcslen(g_git_folder)+1)*2;uint8_t *p=malloc(sizeof g_git_request+bytes);if(!p)return;memcpy(p,&g_git_request,sizeof g_git_request);memcpy(p+sizeof g_git_request,g_git_folder,bytes);sidepanel_send(git_app(),PANELP_GIT_OPEN,p,(uint32_t)(sizeof g_git_request+bytes));free(p);}
static void git_open_folder(const wchar_t *folder){if(!folder||!*folder)return;wchar_t *copy=wdup(folder);if(!copy)return;free(g_git_folder);g_git_folder=copy;g_git_request.session++;if(!g_git_request.session)g_git_request.session++;g_git_request.wsl=fn_active(fn_find("wsl.helper"));g_git_request.as_root=!_stricmp(fn_option(fn_find("wsl.helper"),"user"),"root");git_model_free(&g_git_model);memset(&g_git_state,0,sizeof g_git_state);g_git_state.state=GIT_LOADING;git_redraw();if(sidepanel_show(g.hwnd,git_app(),SIDEPANEL_KEEP,1)<0)app_status(NULL,1,L"Git panel could not open (folderwing-git.exe is needed)");else git_send_root();}
static void run_git_status(fn_ctx *c){const wchar_t *folder=c->dir;if(c->n==1&&c->dirs==1){wchar_t **paths=fn_paths(c);if(paths)folder=paths[0];}git_open_folder(folder);}
static void run_git_show(fn_ctx *c){if(g_git_folder&&sidepanel_running(git_app()))sidepanel_toggle(g.hwnd,git_app(),SIDEPANEL_KEEP);else git_open_folder(g_git_folder?g_git_folder:c->dir);}
static void configure_git(void){g_git_badges=fn_active(fn_find("git.status"))&&!_stricmp(fn_option(fn_find("git.status"),"badges"),"on");if(!fn_active(fn_find("git.status"))){sidepanel_stop_app(git_app());git_model_free(&g_git_model);memset(&g_git_state,0,sizeof g_git_state);if(g_diff_request.reserved){sidepanel_stop_app(diff_app());for(int i=0;i<2;i++){free(g_diff_paths[i]);g_diff_paths[i]=NULL;}g_diff_request.reserved=0;} }else if(g_git_folder&&!fn_active(fn_find("wsl.helper"))){if(fpe_wsl_path_kind(g_git_folder)){sidepanel_stop_app(git_app());git_model_free(&g_git_model);g_git_request.session++;g_git_request.wsl=0;memset(&g_git_state,0,sizeof g_git_state);g_git_state.state=GIT_STOPPED;}if(g_diff_request.reserved&&g_diff_paths[0]&&fpe_wsl_path_kind(g_diff_paths[0])){sidepanel_stop_app(diff_app());for(int i=0;i<2;i++){free(g_diff_paths[i]);g_diff_paths[i]=NULL;}g_diff_request.reserved=0;}}git_redraw();}
static const fn_opt git_opts[]={{"badges","on","Show cached Git status beside names after an explicit status request","on off"},{NULL}};
static int git_scope(const wchar_t *root,const wchar_t *folder){wchar_t dr[128],df[128],pr[32768],pf[32768];int a=term_wsl_split(root,dr,128,pr,32768),b=term_wsl_split(folder,df,128,pf,32768);if(a!=b)return 0;if(a){if(_wcsicmp(dr,df))return 0;size_t n=wcslen(pr),m=wcslen(pf);return m>=n&&!wcsncmp(pr,pf,n)&&(m==n||pr[n-1]==L'/'||pf[n]==L'/');}size_t n=wcslen(root),m=wcslen(folder);return n&&m>=n&&!_wcsnicmp(root,folder,n)&&(m==n||root[n-1]==L'\\'||folder[n]==L'\\');}
static void git_panel_state(const void *data,uint32_t size){if(size<sizeof(git_state)||!fn_active(fn_find("git.status")))return;git_state state;memcpy(&state,data,sizeof state);if(state.session!=g_git_request.session||!state.generation||state.state<GIT_LOADING||state.state>GIT_STOPPED||state.native>1||state.root_chars>32768||state.bytes>2*1024*1024||(uint64_t)sizeof state+state.root_chars*2+state.bytes!=size)return;
 if(state.state!=GIT_READY){if(state.root_chars||state.bytes)return;git_model_free(&g_git_model);g_git_state=state;git_redraw();return;}if(state.root_chars<2||(state.native&&!g_git_request.wsl))return;const wchar_t *root=(const wchar_t*)((const uint8_t*)data+sizeof state);if(root[state.root_chars-1]||wcslen(root)!=state.root_chars-1||!git_scope(root,g_git_folder))return;git_model model={0};if(git_parse_status(&model,root,(const uint8_t*)data+sizeof state+state.root_chars*2,state.bytes,state.native))return;git_model_free(&g_git_model);g_git_model=model;g_git_state=state;git_redraw();}
static const wchar_t *g_git_target,*g_git_relative;
static git_action g_git_action;
static git_meta g_git_meta;
static void run_git_result(fn_ctx *c){(void)c;uint32_t action=g_git_action.action;if(!g_git_model.root)return;if(action==GIT_TERMINAL){if(fn_active(fn_find("terminal.show")))term_open(g_git_model.root,NULL,L"Repository terminal");return;}if(!g_git_target)return;
 if(action==GIT_DIFF){if(!fn_active(fn_find("file.compare_files"))){app_status(NULL,1,L"Enable Compare files for Git diffs");return;}if(g_git_meta.kind=='?'||g_git_meta.kind=='u'){app_status(NULL,1,L"Preview untracked files; inspect conflicts in the repository terminal");return;}wchar_t *root=wdup(g_git_model.root),*relative=wdup(g_git_relative);if(!root||!relative){free(root);free(relative);return;}free(g_diff_paths[0]);free(g_diff_paths[1]);g_diff_paths[0]=root;g_diff_paths[1]=relative;g_diff_request.session++;if(!g_diff_request.session)g_diff_request.session++;g_diff_request.reserved=g_git_action.staged?2:1;g_diff_request.wsl=g_git_request.wsl;g_diff_git=(git_diff_open){.session=g_diff_request.session,.wsl=g_git_request.wsl,.as_root=g_git_request.as_root,.staged=g_git_action.staged,.meta=g_git_meta};memset(&g_diff_state,0,sizeof g_diff_state);g_diff_state.state=DIFF_LOADING;if(sidepanel_show(g.hwnd,diff_app(),SIDEPANEL_KEEP,1)>=0)diff_send_pair();else app_status(NULL,1,L"Diff helper could not open");return;}
 if(action==GIT_COPY){uint64_t ticket=app_clipboard_begin(g.hwnd);size_t n=(wcslen(g_git_target)+1)*2;HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,n);void *p=memory?GlobalLock(memory):NULL;if(p){memcpy(p,g_git_target,n);GlobalUnlock(memory);}else{if(memory)GlobalFree(memory);memory=NULL;}app_clipboard_submit(ticket,CF_UNICODETEXT,memory,0,NULL,L"Git path copied");return;}
 int side=action==GIT_LEFT?0:action==GIT_RIGHT?1:g.active;preview_hide();if(action==GIT_PREVIEW){free(g.pv_want);g.pv_want=NULL;}if(side==1&&!g.dual)g.dual=1;set_active(side);app_reveal(&g.panes[side],g_git_target);layout();}
static void git_panel_action(const void *data,uint32_t size){if(size<sizeof(git_action)+2||(size-sizeof(git_action))%2)return;git_action a;memcpy(&a,data,sizeof a);const wchar_t *shown=sidepanel_app();if(!shown||_wcsicmp(shown,git_app())||!sidepanel_running(git_app())||!fn_active(fn_find("git.status"))||g_git_state.state!=GIT_READY||a.session!=g_git_request.session||a.generation!=g_git_state.generation||!a.generation||a.action>GIT_MENU||a.staged>1)return;const wchar_t *relative=(const wchar_t*)((const uint8_t*)data+sizeof a);size_t chars=(size-sizeof a)/2;if(chars>32768||relative[chars-1]||wcslen(relative)!=chars-1)return;const git_entry *entry=NULL;if(a.action!=GIT_TERMINAL){if(!git_path_safe(relative)||(entry=git_find(&g_git_model,relative))==NULL)return;if(a.action==GIT_DIFF&&(a.staged?entry->meta.x=='.':entry->meta.y=='.'))return;}
 wchar_t *owned=entry?join(g_git_model.root,relative):NULL,*rel=entry?wdup(relative):NULL;if(entry&&(!owned||!rel)){free(owned);free(rel);return;}git_meta meta=entry?entry->meta:(git_meta){0};uint32_t choice=a.action;uint64_t session=a.session;uint32_t generation=a.generation;if(choice==GIT_MENU){static const wchar_t *labels[]={L"Diff",L"Preview working file",L"Reveal in left pane",L"Reveal in right pane",L"Copy path",L"Repository terminal"};HMENU menu=CreatePopupMenu();for(uint32_t i=0;i<6;i++)AppendMenuW(menu,MF_STRING,i+1,labels[i]);POINT point;GetCursorPos(&point);UINT selected=app_menu_track(menu,g.hwnd,point);DestroyMenu(menu);choice=selected?selected-1:GIT_MENU;}
 if(choice<GIT_MENU&&session==g_git_request.session&&generation==g_git_state.generation&&fn_active(fn_find("git.status"))){g_git_target=owned;g_git_relative=rel;g_git_action=a;g_git_action.action=choice;g_git_meta=meta;pane_run(active_pane(),"git.result",FN_FROM_PANEL);g_git_target=g_git_relative=NULL;}free(owned);free(rel);}

static wchar_t *g_content_root;
static content_open g_content_request;
static uint32_t g_content_generation,g_content_state=CF_STOPPED;
static const wchar_t *content_app(void){static wchar_t path[32768];if(!path[0]){GetModuleFileNameW(NULL,path,32768);wchar_t *slash=wcsrchr(path,L'\\');if(slash)wcscpy(slash+1,L"folderwing-content.exe");}return path;}
static void content_send_root(void){if(!g_content_root)return;size_t n=(wcslen(g_content_root)+1)*2;uint8_t *p=malloc(sizeof g_content_request+n);if(p){memcpy(p,&g_content_request,sizeof g_content_request);memcpy(p+sizeof g_content_request,g_content_root,n);sidepanel_send(content_app(),PANELP_CONTENT_OPEN,p,(uint32_t)(sizeof g_content_request+n));free(p);}}
static void content_open_root(const wchar_t *path){if(!path||!*path)return;wchar_t *copy=wdup(path);if(!copy)return;free(g_content_root);g_content_root=copy;g_content_request.session++;if(!g_content_request.session)g_content_request.session++;g_content_request.wsl=fn_active(fn_find("wsl.helper"));g_content_request.as_root=!_stricmp(fn_option(fn_find("wsl.helper"),"user"),"root");g_content_generation=0;g_content_state=CF_STOPPED;if(sidepanel_show(g.hwnd,content_app(),SIDEPANEL_KEEP,1)<0)app_status(NULL,1,L"Find in files could not open (folderwing-content.exe is needed)");else content_send_root();}
static void run_find_content(fn_ctx *c){const wchar_t *path=c->dir;if(c->n&&c->dirs==1&&c->n==1){wchar_t **paths=fn_paths(c);if(paths)path=paths[0];}content_open_root(path);}
static void run_content_show(fn_ctx *c){(void)c;if(!g_content_root||!sidepanel_running(content_app()))content_open_root(g_content_root?g_content_root:active_pane()->path);else sidepanel_toggle(g.hwnd,content_app(),SIDEPANEL_KEEP);}
static void configure_content(void){if(!fn_active(fn_find("file.find_content"))){sidepanel_stop_app(content_app());g_content_state=CF_STOPPED;content_preview_forget();}}
static const wchar_t *g_content_target;
static content_action g_content_action;
static void content_preview_forget(void){free(g_content_preview.path);memset(&g_content_preview,0,sizeof g_content_preview);}
static void content_preview_try(Pane *p,uint64_t ticket){
 if(g_content_preview.pane!=p||!g_content_preview.path||(ticket&&ticket!=g_content_preview.ticket))return;
 const wchar_t *name=p->view&&p->cursor<p_count(p)?fpe_view_name(p->view,p->cursor):NULL;wchar_t *actual=name?join(p->path,name):NULL;
 int current=actual&&fpe_path_equal(actual,g_content_preview.path)&&active_pane()==p;free(actual);
 if(!current){content_preview_forget();return;}
 preview_hide();free(g.pv_want);g.pv_want=wdup(g_content_preview.path);g.pv_want_kind=3;g.pv_src=p;g.pv_want_size=g_content_preview.bytes;g.pv_want_mtime=g_content_preview.mtime;
 if(g.tx_on&&g.pv_want){preview_show();if(g.pv_shown==3)textview_goto_match(g_content_preview.path,g_content_preview.line,g_content_preview.column,g_content_preview.expected);}
 else app_status(NULL,1,L"Enable text preview to open search matches");content_preview_forget();
}
static void run_content_result(fn_ctx *c){(void)c;if(!g_content_target)return;const char *id=fn_self()->id;
 if(!strcmp(id,"content.copy_path")){uint64_t ticket=app_clipboard_begin(g.hwnd);size_t n=(wcslen(g_content_target)+1)*2;HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,n);void *p=memory?GlobalLock(memory):NULL;if(p){memcpy(p,g_content_target,n);GlobalUnlock(memory);}else{if(memory)GlobalFree(memory);memory=NULL;}app_clipboard_submit(ticket,CF_UNICODETEXT,memory,0,NULL,L"Search result path copied");return;}
 int side=!strcmp(id,"content.reveal_left")?0:!strcmp(id,"content.reveal_right")?1:g.active;preview_hide();if(side==1&&!g.dual)g.dual=1;set_active(side);Pane *p=&g.panes[side];content_preview_forget();app_reveal(p,g_content_target);layout();
 if(!strcmp(id,"content.preview")){g_content_preview.path=wdup(g_content_target);g_content_preview.pane=p;g_content_preview.ticket=p->nav_ticket;g_content_preview.line=g_content_action.hit.line;g_content_preview.column=g_content_action.hit.column;g_content_preview.bytes=g_content_action.bytes;g_content_preview.mtime=(int64_t)g_content_action.mtime;for(uint32_t i=0;i<CF_QUERY;i++)g_content_preview.expected[i]=(wchar_t)g_content_action.hit.matched[i];if(p->nav_shown||!p->nav_ticket)content_preview_try(p,0);}
}
static void content_panel_action(const void *data,uint32_t size){if(size<sizeof(content_action)+2||(size-sizeof(content_action))%2)return;content_action action;memcpy(&action,data,sizeof action);const wchar_t *shown=sidepanel_app();
 if(!shown||_wcsicmp(shown,content_app())||!sidepanel_running(content_app())||!fn_active(fn_find("file.find_content"))||action.session!=g_content_request.session||action.search_generation!=g_content_generation||!action.search_generation||action.action>CONTENT_MENU||!action.hit.line||!action.hit.column||!action.hit.length||action.hit.length>=CF_QUERY||action.hit.matched[action.hit.length])return;
 const wchar_t *relative=(const wchar_t*)((const uint8_t*)data+sizeof action);size_t chars=(size-sizeof action)/2;if(chars>32768||relative[chars-1]||wcslen(relative)!=chars-1||!content_relative_safe(relative)||!g_content_root)return;
 wchar_t *owned=join(g_content_root,relative);if(!owned||!space_target_valid(g_content_root,owned)){free(owned);return;}
 uint32_t choice=action.action;uint64_t session=g_content_request.session;uint32_t generation=g_content_generation;
 static const char *ids[]={"content.preview","content.reveal_left","content.reveal_right","content.copy_path"};
 if(choice==CONTENT_MENU){static const wchar_t *labels[]={L"Preview at match",L"Reveal in left pane",L"Reveal in right pane",L"Copy path"};HMENU menu=CreatePopupMenu();for(uint32_t i=0;i<4;i++)AppendMenuW(menu,MF_STRING,i+1,labels[i]);POINT point;GetCursorPos(&point);UINT item=app_menu_track(menu,g.hwnd,point);DestroyMenu(menu);choice=item?item-1:CONTENT_MENU;}
 if(choice<4&&session==g_content_request.session&&generation==g_content_generation){g_content_target=owned;g_content_action=action;pane_run(active_pane(),ids[choice],FN_FROM_PANEL);g_content_target=NULL;}free(owned);
}

static void panel_ready(const wchar_t *app)
{
    if (!_wcsicmp(app, git_app())) {
        git_send_root();
    } else if (!_wcsicmp(app, duplicate_app())) {
        duplicate_send_roots();
    } else if (!_wcsicmp(app, diff_app())) {
        diff_send_pair();
    } else if (!_wcsicmp(app, content_app())) {
        content_send_root();
    } else if (!_wcsicmp(app, space_app())) {
        space_send_root();
    } else if (!_wcsicmp(app, term_app())) {
        term_config();
        term_note_pane(1);
    } else if (!_wcsicmp(app, operations_app())) {
        operations_send();
    } else {
        settings_send();
        if(g_settings_open_builder){uint32_t open=1;sidepanel_send(settings_app(),PANELP_RELAY_OPEN,&open,4);}
    }
}

/* The panel's own toggles work with the panel focused too. */
static int panel_key(UINT vk)
{
    static const char *const ids[] = { "settings.show", "terminal.show", "operations.show", "space.show", "content.show", "duplicates.show", "git.show" };
    uint8_t mods = key_mods(0);
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        const fn_def *d = fn_find(ids[i]);
        wchar_t keys[128];
        char k8[128];
        if (!d || !fn_active(d) || !fn_key_text(d, keys, 128)) continue;
        WideCharToMultiByte(CP_UTF8, 0, keys, -1, k8, sizeof k8, NULL, NULL);
        fn_binding b;
        if (!fn_parse_key(k8, &b) && b.vk == vk && b.mods == mods) {
            eat_char(GetFocus(), 0);
            pane_run(active_pane(), ids[i], FN_FROM_KEY);
            return 1;
        }
    }
    return 0;
}

static void settings_saved(const char *req, const char *status, const char *msg)
{
    char buf[400];
    int n = snprintf(buf, sizeof buf, "%s\t%s\t%s", req, status, msg);
    if (n > 0) sidepanel_send(settings_app(), PANELP_SAVED, buf, (uint32_t)(n < (int)sizeof buf ? n : (int)sizeof buf - 1));
}

/* Splits a line at tabs, in place; missing fields are NULL. */
static int tab_fields(char *line, char **f, int max)
{
    int n = 0;
    for (int i = 0; i < max; i++) f[i] = NULL;
    for (char *p = line; p && n < max;) {
        f[n++] = p;
        p = strchr(p, '\t');
        if (p) *p++ = 0;
    }
    return n;
}

/* Settings changed together in the settings app. The file is only written
 * when it is still what the app saw: an edit made meanwhile in an editor
 * (saved, but not applied yet) is applied instead, and the app is told so.
 * The rest of the file stays as it was, and the new text applies at once. */
static void settings_set(char *text)
{
    char *body = strchr(text, '\n'), *f[2];
    if (body) *body++ = 0;
    if (tab_fields(text, f, 2) < 2 || !body || !*body) return;
    const char *req = f[0];
    if (g_settings_save) {
        settings_saved(req, "failed", "Another settings save is still in progress; try again when it finishes");
        return;
    }
    if (!g.settings[0]) {
        settings_saved(req, "failed", "Started with --settings= : there is no file to save to");
        return;
    }
    int64_t t = fpe_prof_begin();
    char *buf, *out = NULL, why[200];
    size_t len, out_len = 0;
    if (strtoul(f[1], NULL, 10) != fn_revision()) { /* a SETTINGS on its way already shows what applies */
        settings_saved(req, "conflict", "Not saved: the settings changed meanwhile. What applies now shows here; check it, then change it again");
        return;
    }
    if (settings_read(&buf, &len)) {
        settings_saved(req, "failed", "The settings file is locked by another program: try again");
        return;
    }
    if (bytes_hash(buf ? buf : "", len) != g_cfg_hash) {
        settings_apply(buf, len);
        free(buf);
        settings_saved(req, "conflict", "Not saved: the file was just changed elsewhere. What applies now shows here; check it, then change it again");
        return;
    }
    if (!buf) { /* no file yet: start from the template, so the rest is there to read */
        size_t n = fn_template(NULL, 0);
        if ((buf = malloc(n + 1))) len = fn_template(buf, n + 1);
    }
    int rc = 0;
    for (char *line = body; line && *line && !rc;) {
        char *nl = strchr(line, '\n'), *c[4];
        if (nl) *nl++ = 0;
        if (tab_fields(line, c, 4) < 3) {
            line = nl;
            continue;
        }
        const char *sec = c[0], *key = c[1], *val = !strcmp(c[2], "set") ? (c[3] ? c[3] : "") : NULL;
        if (val && !strcmp(sec, "functions") && !strcmp(key, "settings.show") && _stricmp(val, "on")) {
            snprintf(why, sizeof why, "Only in the file: without it, Ctrl+, and the gear couldn't bring the settings back");
            rc = -1;
            break;
        }
        rc = fn_settings_edit(buf, len, sec, key, val, &out, &out_len, why, sizeof why);
        if (!rc) { /* the next change works on this one's result */
            free(buf);
            buf = out, len = out_len, out = NULL;
        }
        line = nl;
    }
    out = buf, out_len = len;
    if (rc) {
        free(out);
        settings_saved(req, rc == -1 ? "invalid" : "failed", rc == -1 ? why : "Out of memory");
        return;
    }
    settings_folder();
    int wrote = settings_write(g.settings, out, out_len, &g_cfg_hash);
    if (wrote) {
        free(out);
        if (wrote == -2 || wrote == -3) {
            settings_check();
            if (wrote == -2)
                settings_saved(req, "conflict", "Not saved: the file was just changed elsewhere. What applies now shows here; check it, then change it again");
            else settings_saved(req, "failed", "Settings changed during save; a recovery copy was kept beside the file");
        } else settings_saved(req, "failed", "Could not write the settings file");
        return;
    }
    if (fn_running(fn_find("settings.watch"))) settings_watch_start(); /* a new folder or file */
    settings_apply(out, out_len);
    free(out);
    FPE_PROF_END("settings.save", t);
    settings_saved(req, "ok", "");
}

static DWORD WINAPI settings_commit(void *arg)
{
    settings_save *s = arg;
    wchar_t dir[MAX_PATH + 32];
    wcscpy(dir, s->path);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) { *slash = 0; CreateDirectoryW(dir, NULL); }
    s->result = settings_write(s->path, s->text, s->len, &s->expect);
    /* The disk may have changed during the commit. Bring back what is there,
     * including an external edit on conflict, rather than applying stale text. */
    s->read_result = settings_read_path(s->path, &s->current, &s->current_len);
    return 0;
}

static void settings_save_free(settings_save *s)
{
    CloseHandle(s->thread);
    free(s->text);
    free(s->current);
    free(s);
}

static void settings_save_complete(void)
{
    settings_save *s = g_settings_save;
    if (!s || WaitForSingleObject(s->thread, 0) != WAIT_OBJECT_0) return;
    KillTimer(g.hwnd, TIMER_SETTINGS_SAVE);
    g_settings_save = NULL;
    FPE_PROF_END("settings.command_commit", s->started); /* end-to-end, includes worker time */
    if (!wcscmp(s->path, g.settings)) {
        if (!s->read_result) {
            if (bytes_hash(s->current ? s->current : "", s->current_len) != g_cfg_hash)
                settings_apply(s->current, s->current_len);
        } else SetTimer(g.hwnd, TIMER_SETTINGS, SETTINGS_QUIET_MS, NULL);
        if (!s->result && fn_running(fn_find("settings.watch"))) settings_watch_start();
        /* A closed/restarted helper can reuse request numbers. Only the
         * instance that submitted this save may receive its acknowledgement. */
        if (s->generation && s->generation == sidepanel_generation(settings_app())) {
            char req[32];
            snprintf(req, sizeof req, "%u", s->request);
            settings_saved(req, !s->result ? "ok" : s->result == -2 ? "conflict" : "failed",
                           !s->result ? "" : s->result == -2
                               ? "Not saved: the file changed elsewhere. Your draft is kept"
                               : s->result == -3
                                   ? "Settings changed during save; recovery copy kept beside the file. Draft kept"
                                   : "Could not write settings; your draft is kept");
        }
    }
    if(s->dock&&s->result)app_status(NULL,1,s->result==-2?L"Dock setting was not saved: settings changed elsewhere":L"Dock setting could not be saved");
    settings_save_free(s);
}

/* Called after the message loop, never from WM_DESTROY. An accepted save
 * finishes durably even when its panel/window closes, without accessing UI. */
static void settings_save_join(void)
{
    settings_save *s = g_settings_save;
    if (!s) return;
    WaitForSingleObject(s->thread, INFINITE);
    g_settings_save = NULL;
    settings_save_free(s);
}

/* A draft is one atomic edit: options, keys and exposure are checked before
 * the settings file or live registry changes. External edits keep precedence. */
static void settings_relay_save(const relay_draft *d)
{
    char req[32], why[200];
    snprintf(req, sizeof req, "%u", d->request);
    if (g_settings_save) {
        settings_saved(req, "failed", "Another settings save is still in progress; try again when it finishes");
        return;
    }
    if (!fn_active(fn_find("settings.commands"))) {
        settings_saved(req, "failed", "Custom commands are disabled");
        return;
    }
    if (!g.settings[0]) {
        settings_saved(req, "failed", "Started with --settings= : there is no file to save to");
        return;
    }
    if (d->revision != fn_revision()) {
        settings_saved(req, "conflict",
                       "Not saved: settings changed meanwhile. Your draft is kept; review the current "
                       "settings and save again");
        return;
    }
    char *text = NULL, *out = NULL;
    size_t len = 0, size = 0;
    if (settings_read(&text, &len)) {
        settings_saved(req, "failed", "Settings are locked by another program; your draft is kept");
        return;
    }
    if (bytes_hash(text ? text : "", len) != g_cfg_hash) {
        settings_apply(text, len);
        free(text);
        settings_saved(req, "conflict",
                       "Not saved: the file changed elsewhere. Your draft is kept; review the current "
                       "settings and save again");
        return;
    }
    if (!text) {
        size_t n = fn_template(NULL, 0);
        text = malloc(n + 1);
        if (text)
            len = fn_template(text, n + 1);
    }
    if (!text) {
        settings_saved(req, "failed", "Out of memory");
        return;
    }
    int rc = relay_draft_apply(d, text, len, &out, &size, why, sizeof why);
    free(text);
    if (rc) {
        settings_saved(req, rc == -1 ? "invalid" : "failed", rc == -1 ? why : "Out of memory");
        return;
    }
    settings_save *s = calloc(1, sizeof *s);
    if (!s) {
        free(out);
        settings_saved(req, "failed", "Out of memory; your draft is kept");
        return;
    }
    wcscpy(s->path, g.settings);
    s->text = out;
    s->len = size;
    s->expect = g_cfg_hash;
    s->request = d->request;
    s->generation = sidepanel_generation(settings_app());
    s->started = fpe_prof_begin();
    /* The outer loop waits on the worker directly. A dialog/drag has its own
     * message loop, so provide a timer only while a save is outstanding. */
    if (!SetTimer(g.hwnd, TIMER_SETTINGS_SAVE, 25, NULL)) {
        free(out); free(s);
        settings_saved(req, "failed", "Could not arrange save completion; your draft is kept");
        return;
    }
    s->thread = CreateThread(NULL, 64 * 1024, settings_commit, s, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!s->thread) {
        KillTimer(g.hwnd, TIMER_SETTINGS_SAVE);
        free(out); free(s);
        settings_saved(req, "failed", "Could not start saving settings; your draft is kept");
        return;
    }
    g_settings_save = s;
}
/* Optional Direct helper: normal browsing never starts it or its runtime. */
static const wchar_t *direct_app(void)
{
    static wchar_t path[32768];
    if (!path[0]) {
        GetModuleFileNameW(NULL, path, 32768);
        wchar_t *slash = wcsrchr(path, L'\\');
        if (slash) wcscpy(slash + 1, L"direct-client.exe");
    }
    return path;
}
static void run_direct_show(fn_ctx *c)
{
    (void)c;
    if (sidepanel_toggle(g.hwnd, direct_app(), 0) < 0)
        app_status(NULL, 1, L"Direct client is missing (install direct-client.exe beside Folderwing)");
}
static void configure_direct(void)
{
    if (!fn_active(fn_find("direct.show"))) sidepanel_stop_app(direct_app());
}
/* Optional Windows tools cannot depend unconditionally on wsl.helper. Notify
 * their captured sessions explicitly when that policy alone is reloaded. */
static void optional_wsl_policy_changed(void)
{
    int enabled=fn_active(fn_find("wsl.helper"));
    configure_space(); /* Also removes disabled WSL volumes from a Windows view. */
    if(!enabled){
    if(g_content_root&&fpe_wsl_path_kind(g_content_root)){
        sidepanel_stop_app(content_app());free(g_content_root);g_content_root=NULL;
        g_content_request.session++;if(!g_content_request.session)g_content_request.session++;g_content_request.wsl=0;g_content_generation=0;
        g_content_state=CF_STOPPED;content_preview_forget();
    }
    int native_duplicate=0;
    for(uint32_t i=0;i<g_duplicate_request.count;i++)
        if(g_duplicate_roots[i]&&fpe_wsl_path_kind(g_duplicate_roots[i]))native_duplicate=1;
    if(native_duplicate){
        sidepanel_stop_app(duplicate_app());
        for(unsigned i=0;i<DUPLICATE_ROOTS;i++){free(g_duplicate_roots[i]);g_duplicate_roots[i]=NULL;g_duplicate_request.chars[i]=0;}
        g_duplicate_request.count=0;g_duplicate_request.session++;if(!g_duplicate_request.session)g_duplicate_request.session++;g_duplicate_request.wsl=0;
        memset(&g_duplicate_state,0,sizeof g_duplicate_state);
    }
    if((g_diff_paths[0]&&fpe_wsl_path_kind(g_diff_paths[0]))||
       (!g_diff_request.reserved&&g_diff_paths[1]&&fpe_wsl_path_kind(g_diff_paths[1]))){
        sidepanel_stop_app(diff_app());
        for(unsigned i=0;i<2;i++){free(g_diff_paths[i]);g_diff_paths[i]=NULL;}
        g_diff_request.session++;if(!g_diff_request.session)g_diff_request.session++;g_diff_request.wsl=0;g_diff_request.reserved=0;
        memset(&g_diff_git,0,sizeof g_diff_git);memset(&g_diff_state,0,sizeof g_diff_state);
    }
    }
    configure_git();
    uint32_t policy=(uint32_t)enabled;
    g_content_request.wsl=g_duplicate_request.wsl=g_diff_request.wsl=g_git_request.wsl=policy;
    g_diff_git.wsl=policy;
    /* Cold sends preserve Windows results and never start a helper. */
    sidepanel_send(content_app(),PANELP_CONTENT_WSL_POLICY,&policy,sizeof policy);
    sidepanel_send(duplicate_app(),PANELP_DUPLICATE_WSL_POLICY,&policy,sizeof policy);
    sidepanel_send(diff_app(),PANELP_DIFF_WSL_POLICY,&policy,sizeof policy);
    sidepanel_send(git_app(),PANELP_GIT_WSL_POLICY,&policy,sizeof policy);
}
static void panel_relay_context(uint32_t request, const wchar_t *target)
{
    Pane *p = active_pane();
    uint32_t count = 0, flags = 0;
    wchar_t *paths[RELAY_CONTEXT_MAX] = {0};
    const wchar_t *dir = p->path ? p->path : L"";
    size_t used = wcslen(dir) + 1;
    uint32_t targets = p->nsel ? p->nsel : p_count(p) ? 1 : 0;
    if (targets > RELAY_CONTEXT_MAX || used >= RELAY_CONTEXT_CHARS)
        flags = 1;
    else
        for (uint32_t row = 0; row < p_count(p) && count < targets; ++row) {
            if (p->nsel ? !p_is_sel(p, row) : row != p->cursor)
                continue;
            wchar_t *path = join(dir, fpe_view_name(p->view, row));
            if (!path || used + wcslen(path) + 1 > RELAY_CONTEXT_CHARS) {
                free(path);
                flags = 1;
                break;
            }
            used += wcslen(path) + 1;
            paths[count++] = path;
        }
    if (used > RELAY_CONTEXT_CHARS) {
        dir = L"";
        used = 1;
        flags = 1;
    }
    size_t size = 12 + used * 2;
    uint8_t *data = malloc(size);
    if (data) {
        uint32_t head[] = {request, count, flags};
        memcpy(data, head, 12);
        wchar_t *s = (wchar_t *)(data + 12);
        wcscpy(s, dir);
        s += wcslen(s) + 1;
        for (uint32_t i = 0; i < count; ++i) {
            wcscpy(s, paths[i]);
            s += wcslen(s) + 1;
        }
        sidepanel_send(target, PANELP_RELAY_CONTEXT, data, (uint32_t)size);
        free(data);
    }
    for (uint32_t i = 0; i < count; ++i)
        free(paths[i]);
}
static void settings_relay_browse(uint32_t request)
{
    /* Load common dialogs only for this user action; idle/startup stay small. */
    HMODULE library = LoadLibraryW(L"comdlg32.dll");
    typedef BOOL(WINAPI * pick_fn)(LPOPENFILENAMEW);
    pick_fn pick = library ? (pick_fn)(void *)GetProcAddress(library, "GetOpenFileNameW") : NULL;
    wchar_t path[32768] = L"";
    OPENFILENAMEW of = {sizeof of};
    of.hwndOwner = g.hwnd;
    of.lpstrFile = path;
    of.nMaxFile = 32768;
    of.lpstrFilter = L"Programs and scripts\0*.exe;*.com;*.cmd;*.bat;*.ps1\0All files\0*.*\0\0";
    of.lpstrTitle = L"Choose a program or script";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
    g.modal_seq++;
    BOOL ok = pick && pick(&of);
    g.modal_seq++;
    if (library)
        FreeLibrary(library);
    if (!ok)
        path[0] = 0;
    size_t bytes = (wcslen(path) + 1) * 2;
    uint8_t *data = malloc(4 + bytes);
    if (data) {
        memcpy(data, &request, 4);
        memcpy(data + 4, path, bytes);
        sidepanel_send(settings_app(), PANELP_RELAY_BROWSED, data, (uint32_t)(4 + bytes));
        free(data);
    }
}

/* What a panel app asks for. Only the settings app may run functions or change settings. */
static void panel_message(const wchar_t *app, uint32_t type, const void *data, uint32_t size)
{
    if(!_wcsicmp(app,git_app())){if(type==PANELP_GIT_STATE)git_panel_state(data,size);else if(type==PANELP_GIT_ACTION)git_panel_action(data,size);return;}

    if(!_wcsicmp(app,duplicate_app())){if(type==PANELP_DUPLICATE_ACTION)duplicate_panel_action(data,size);else if(type==PANELP_DUPLICATE_STATE&&size==sizeof(duplicate_state)){duplicate_state state;memcpy(&state,data,sizeof state);if(state.session==g_duplicate_request.session&&state.generation&&state.state>=1&&state.state<=6)g_duplicate_state=state;}return;}

    if(!_wcsicmp(app,diff_app())){if(type==PANELP_DIFF_STATE&&size==sizeof(diff_state)&&fn_active(fn_find("file.compare_files"))){diff_state s;memcpy(&s,data,sizeof s);if(s.session==g_diff_request.session&&s.state>=DIFF_LOADING&&s.state<=DIFF_FAILED)g_diff_state=s;}return;}
    if(!_wcsicmp(app,content_app())){
        if(type==PANELP_CONTENT_ACTION)content_panel_action(data,size);
        else if(type==PANELP_CONTENT_STATE&&size==sizeof(content_state)){content_state state;memcpy(&state,data,sizeof state);if(state.session==g_content_request.session&&state.state<=CF_STOPPED){int changed=g_content_state!=state.state;g_content_generation=state.generation;g_content_state=state.state;if(changed)main_status_redraw();}}
        return;
    }
    if (!_wcsicmp(app, space_app())) {
        if(type==PANELP_SPACE_VOLUME&&size>=sizeof(space_action)+2&&(size-sizeof(space_action))%2==0){space_action a;memcpy(&a,data,sizeof a);const wchar_t *root=(const wchar_t*)((const uint8_t*)data+sizeof a);size_t chars=(size-sizeof a)/2;const wchar_t *shown=sidepanel_app();if(shown&&!_wcsicmp(shown,space_app())&&fn_active(fn_find("file.analyze_space"))&&a.session==g_space_request.session&&!a.action&&!a.flags&&chars<=512&&!root[chars-1]&&wcslen(root)==chars-1&&space_volume_allowed(root,fn_active(fn_find("wsl.helper"))))space_open_root(root);}
        else if(type==PANELP_SPACE_ACTION)space_panel_action(data,size);
        else if(type==PANELP_SPACE_STATE&&size==sizeof(space_state)){space_state state;memcpy(&state,data,sizeof state);if(state.session==g_space_request.session&&state.state<=SP_STOPPED&&g_space_state!=state.state){g_space_state=state.state;main_status_redraw();}}
        return;
    }
    if (!_wcsicmp(app, operations_app())) {
        if (type == PANELP_OP_ACTION && size == sizeof(ops_action)) {
            ops_action action;
            memcpy(&action, data, sizeof action);
            operations_action(&action);
        }
        return;
    }
    if (!_wcsicmp(app, term_app())) {
        if (type == PANELP_TERM_CWD)
            term_cwd(data, size);
        return;
    }
    if (!_wcsicmp(app, direct_app())) {
        /* Selection only, requested by the visible helper. No RUN/SET authority. */
        const wchar_t *shown = sidepanel_app();
        if (type == PANELP_RELAY_GET_CONTEXT && size == 4 && shown && !_wcsicmp(shown, app)) {
            uint32_t request;
            memcpy(&request, data, 4);
            panel_relay_context(request, direct_app());
        }
        return;
    }
    if (_wcsicmp(app, settings_app()) || !settings_shown())
        return;
    if (type == PANELP_RUN && size < 64) {
        char id[64];
        memcpy(id, data, size);
        id[size] = 0;
        pane_run(active_pane(), id, FN_FROM_PANEL);
    } else if (type == PANELP_RELAY_SAVE && size == sizeof(relay_draft)) {
        relay_draft *draft = malloc(sizeof *draft);
        if (draft) {
            memcpy(draft, data, sizeof *draft);
            int64_t t = fpe_prof_begin();
            settings_relay_save(draft);
            FPE_PROF_END("settings.command_save", t);
            free(draft);
        }
    } else if (type == PANELP_RELAY_GET_CONTEXT && size == 4) {
        uint32_t request;
        memcpy(&request, data, 4);
        g_settings_open_builder = 0;
        panel_relay_context(request, settings_app());
    } else if (type == PANELP_RELAY_BROWSE && size == 4) {
        uint32_t request;
        memcpy(&request, data, 4);
        settings_relay_browse(request);
    } else if (type == PANELP_SET && size < 16384) {
        char *text = malloc((size_t)size + 1);
        if (!text)
            return;
        memcpy(text, data, size);
        text[size] = 0;
        settings_set(text);
        free(text);
    }
}

static const fn_opt operations_opts[] = {
    { "conflicts", "keep", "initial choice for each transfer; confirmed with Start", "keep skip replace" },
    { NULL },
};

static const fn_opt panel_opts[] = {
    { "slide", "on", "on: the panel slides in from the right (~100 ms); off: it appears at once", "on off" },
    { NULL },
};

static void configure_panel(void)
{
    sidepanel_config cfg = { _stricmp(fn_option(fn_find("panel.side"), "slide"), "off") != 0, layout, panel_closed,
                             panel_ready, panel_message, focus, panel_key };
    sidepanel_configure(&cfg);
}

static void run_settings_commands(fn_ctx *c)
{
    g_settings_open_builder=1;
    if(GetFileAttributesW(settings_app())==INVALID_FILE_ATTRIBUTES||sidepanel_show(g.hwnd,settings_app(),0,1)<0){
        g_settings_open_builder=0;app_status(NULL,1,L"Settings could not open (folderwing-settings.exe is needed)");return;}
    settings_send();uint32_t open=1;sidepanel_send(settings_app(),PANELP_RELAY_OPEN,&open,4);
}

static void run_settings_show(fn_ctx *c)
{
    if (GetFileAttributesW(settings_app()) == INVALID_FILE_ATTRIBUTES)
        app_status(NULL, 1, L"The settings app is missing (folderwing-settings.exe)");
    else if (sidepanel_toggle(g.hwnd, settings_app(), 0) < 0)
        app_status(NULL, 1, L"The side panel could not open");
    InvalidateRect(g.hwnd, NULL, FALSE);
}

static void preview_stop(void)
{
    if (g.pv_shown==1) { /* stopped on request while showing */
        g.pv_shown = g.pv_swapped = 0;
        layout();
    }
    pdfview_stop();
}

/* ======================================================================== */
/* App API for functions (app.h)                                             */
/* ======================================================================== */

HWND app_window(void) { return g.hwnd; }
Pane *app_left_pane(void) { return &g.panes[0]; }
Pane *app_other_pane(Pane *p) { return other_pane(p); }
const wchar_t *app_pane_path(Pane *p) { return p->path; }

void app_status(Pane *p, int err, const wchar_t *fmt, ...)
{
    wchar_t msg[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(msg, 512, fmt, ap);
    va_end(ap);
    msg[511] = 0;
    set_status(p ? p : active_pane(), err, L"%ls", msg);
}

void app_navigate(Pane *p, const wchar_t *path) { p_navigate(p, path, NULL, 1); }
void app_go_back(Pane *p) { p_go_back(p); }
void app_go_forward(Pane *p) { p_go_forward(p); }
void app_go_up(Pane *p) { p_go_up(p); }
void app_refresh(Pane *p) { p_refresh(p); }
void app_focus_address(Pane *p) { p_focus_address(p); }
void app_open_cursor(Pane *p) { p_open_row(p, p->cursor); }
void app_set_sort(Pane *p, fpe_sort_key k) { p_set_sort(p, k); }

void app_select_all(Pane *p)
{
    for (uint32_t r = 0; r < p_count(p); r++) p_set_sel(p, r, 1);
    p_redraw(p);
}

void app_rename_cursor(Pane *p)
{
    if (p_count(p)) rename_begin(p, p->cursor);
}

void app_rename_path(const wchar_t *path) { tree_rename(path); }
void app_new_item(Pane *p, int file) { p_new_item(p, file); }
void app_new_file(Pane *p, const wchar_t *name, const wchar_t *tmpl, const fn_def *owner)
{
    wchar_t encoded[260];
    if(p->path&&fpe_wsl_path_kind(p->path)){
        if(!linkdialog_name(name,encoded)){set_status(p,1,L"Use a Linux filename of at most 255 bytes");return;}name=encoded;
    }
    if(p->path)fsop_submit(p,FSOP_NEW_FILE,NULL,NULL,name,tmpl,owner);
}
void app_transfer(Pane *p, int move) { p_transfer(p, move); }
void app_clip_set(Pane *p, wchar_t *const *paths, uint32_t n, int cut) { clip_set(p, paths, n, cut); }
void app_paste_into(const wchar_t *dest) { clip_paste_into(dest); }
void app_delete(wchar_t *const *paths, uint32_t n, int permanent) { delete_paths(paths, n, permanent); }

int app_tab_count(Pane *p) { return p->ntabs; }
int app_tab_closed_count(void) { return g.nclosed; }
void app_tab_new(Pane *p, const wchar_t *path, int show) { tab_new(p, path, show); }
void app_tab_close(Pane *p) { tab_close(p, p->tab); }
void app_tab_step(Pane *p, int dir) { tab_step(p, dir); }
void app_tab_reopen(Pane *p) { tab_reopen(p); }

int app_is_pinned(const wchar_t *path) { return tree_pinned_at(path) >= 0; }
void app_pin(const wchar_t *path) { tree_pin(path, 1); }
void app_unpin(const wchar_t *path) { tree_unpin(path); }

void app_toggle_tree(void) { toggle_tree(); }
void app_toggle_dual(void) { toggle_dual(); }
void app_toggle_hidden(void) { toggle_hidden(); }
void app_focus_window(HWND window) { if(GetFocus()!=window)focus(window); }
void app_focus_cycle(int dir) { focus_cycle(GetFocus(), dir); }

Pane *app_active_pane(void) { return active_pane(); }
HFONT app_font(void) { return g.font; }
int app_px(int v) { return S(v); }
int app_row_h(void) { return g.row_h; }
void app_pane_ctx(fn_ctx *c, Pane *p, int from) { pane_ctx(c, p, from, 1); }
void app_preview_ctx(fn_ctx *c,HWND reader,int from){
 pane_ctx(c,active_pane(),from,1);
 if(reader&&(reader==archiveview_window()||(g.pv_shown==4&&(reader==mediaview_window()||reader==imageview_window()||reader==textview_window()||reader==pdfview_window()))||
   ((g.ar_readers&1)&&reader==pdfview_window())||((g.ar_readers&2)&&reader==imageview_window())||((g.ar_readers&4)&&reader==textview_window())||((g.ar_readers&16)&&reader==mediaview_window())))c->where=FN_ARCHIVE;
}
void app_ctx_free(fn_ctx *c) { ctx_free(c); }

const wchar_t *app_workspace_session(void){return g.session;}
DWORD app_workspace_capture(session *snapshot){
    session borrowed;session_defaults(&borrowed);session_defaults(snapshot);
    borrowed.tree=g.show_tree;borrowed.dual=g.dual;borrowed.hidden=g.show_hidden;borrowed.active=g.dual?g.active:0;borrowed.ratio=g.ratio;borrowed.tree_w=MulDiv(g.tree_w,96,(int)(g.dpi?g.dpi:96));
    if(borrowed.tree_w<120)borrowed.tree_w=120;if(borrowed.tree_w>1200)borrowed.tree_w=1200;
    for(int i=0;i<2;i++){Pane *p=&g.panes[i];if(p->loading||p->nav_ticket||p->address_resolving)return ERROR_BUSY;
        borrowed.ntabs[i]=p->ntabs;borrowed.cur[i]=p->tab;
        for(int k=0;k<p->ntabs;k++){const fpe_view_opts *o=k==p->tab?&p->opts:&p->tabs[k].opts;borrowed.tabs[i][k]=(ses_tab){p->tabs[k].path,(uint8_t)o->key,o->descending};}}
    return workspace_clone(&borrowed,snapshot);
}
DWORD app_workspace_apply(const session *snapshot){
    session fresh;DWORD error=workspace_clone(snapshot,&fresh);if(error)return error;
    /* Allocate both shown paths before the first pane mutation. */
    wchar_t *shown[2]={wdup(fresh.tabs[0][fresh.cur[0]].path),wdup(fresh.tabs[1][fresh.cur[1]].path)};
    if(!shown[0]||!shown[1]){free(shown[0]);free(shown[1]);session_free(&fresh);return ERROR_NOT_ENOUGH_MEMORY;}
    tab_drag_cancel();preview_hide();content_preview_forget();
    for(int i=0;i<2;i++){Pane *p=&g.panes[i];rename_end(p,0);p_address_cancel(p);p->nav_ticket=p->refresh_ticket=0;p->loading=0;KillTimer(p->hwnd,TIMER_LOADING);
        SetPropW(p->hwnd,L"Address.Loading",NULL);SetPropW(p->hwnd,L"Address.Error",NULL);free(p->nav_path);p->nav_path=NULL;
        for(int k=0;k<p->ntabs;k++)tab_free(&p->tabs[k]);
        while(p->nback)free(p->back[--p->nback]);while(p->nfwd)free(p->fwd[--p->nfwd]);
        if(p->view){fpe_view_destroy(p->view);p->view=NULL;}free(p->path);p->path=shown[i];p_sel_reset(p,0);p->cursor=p->anchor=0;p->top=0;p->filter_len=0;p->filter[0]=0;
        p->want_rename=0;p->want_select[0]=0;p->down_armed=p->down_deferred=0;
        p->ntabs=fresh.ntabs[i];p->tab=fresh.cur[i];p->tabs_epoch++;
        for(int k=0;k<p->ntabs;k++){Tab *t=&p->tabs[k];t->path=fresh.tabs[i][k].path;fresh.tabs[i][k].path=NULL;t->opts=p->opts;t->opts.key=(fpe_sort_key)fresh.tabs[i][k].sort;t->opts.descending=fresh.tabs[i][k].desc;t->opts.show_hidden=t->opts.show_system=(uint8_t)fresh.hidden;t->workspace_restore=1;}
        p->opts=p->tabs[p->tab].opts;folderwatch_clear((unsigned)i);
    }
    g.show_tree=fresh.tree;g.dual=fresh.dual;g.show_hidden=fresh.hidden;g.tree_w=S(fresh.tree_w);g.ratio=fresh.ratio;g.active=fresh.dual?fresh.active:0;
    for(int i=0;i<2;i++){Pane *p=&g.panes[i];workspace_address(p);p_navigate(p,p->tabs[p->tab].path,NULL,0);
        if(!p->nav_ticket&&!p->view&&!p->tabs[p->tab].load_error){p->tabs[p->tab].load_error=ERROR_NOT_ENOUGH_MEMORY;set_status(p,1,L"Workspace folder could not be opened; retry with Refresh");}tab_strip_changed(p);}
    session_free(&fresh);layout();set_active(g.active);update_title();return 0;
}

void app_draw_icon(HDC dc, const wchar_t *path, int folder, int place, int x, int y, int h)
{
    ti_kind k = ti_of_path(path, folder);
    COLORREF color = 0;
    const tnode *root = NULL;
    if (place == APP_PLACE_ROOT || place == APP_PLACE_PIN)
        for (uint32_t i = 0; i < T.nroots && !root; i++)
            if (same_path(T.roots[i]->path, path)) root = T.roots[i];
    if (root) {
        k = (ti_kind)root->ti;
        if (root->kind == NODE_PIN) color = C_ACCENT;
    }
    int idx = -1;
    if (root && g.icon_style == ICONS_WINDOWS) {
        idx = tree_icon((tnode *)root);
    } else if (shell_icon_for(k)) {
        const wchar_t *slash = wcsrchr(path, L'\\');
        const wchar_t *name = slash ? slash + 1 : path;
        wchar_t dir[MAX_PATH];
        size_t dl = slash ? (size_t)(slash - path) : 0;
        if (dl < MAX_PATH) {
            wmemcpy(dir, path, dl);
            dir[dl] = 0;
        }
        uint32_t len = (uint32_t)wcslen(name);
        const wchar_t *dot = wcsrchr(name, L'.');
        uint32_t ext = dot && dot != name ? (uint32_t)(dot - name) + 1 : len;
        idx = folder ? shell_icon_folder()
                     : shell_icon_entry(dl < MAX_PATH ? dir : NULL, name, len, ext, 0);
    }
    if (idx >= 0) {
        int sz = shell_icon_size();
        shell_icon_draw(dc, idx, x + (S(16) - sz) / 2, y + (h - sz) / 2, 0);
    } else {
        draw_type_icon(dc, k, x, y, h, 0, color);
    }
}

void app_reveal(Pane *p, const wchar_t *path)
{
    wchar_t *dir = wdup(path);
    wchar_t *slash = dir ? wcsrchr(dir, L'\\') : NULL;
    if (!slash || !slash[1]) {
        free(dir);
        return;
    }
    const wchar_t *name = path + (slash - dir) + 1;
    if (slash == dir + 2 && dir[1] == L':') slash[1] = 0; /* "C:\x" -> "C:\" */
    else *slash = 0;
    p_navigate(p, dir, name, 1);
    free(dir);
}

void app_places(app_place_fn fn, void *user)
{
    for (int i = 0; i < g.nrecent; i++) fn(user, g.recent[i], NULL, APP_PLACE_RECENT);
    for (int k = 0; k < 2; k++) {
        Pane *p = &g.panes[k];
        if (k == 1 && !g.dual) break;
        for (int i = 0; i < p->ntabs; i++) fn(user, i == p->tab ? p->path : p->tabs[i].path, NULL, APP_PLACE_TAB);
    }
    for (uint32_t i = 0; i < T.nroots; i++) {
        tnode *root=T.roots[i];
        if(root->kind==NODE_LINUX){for(uint32_t k=0;k<root->nkids;k++)fn(user,root->kids[k]->path,root->kids[k]->label,APP_PLACE_ROOT);}
        else fn(user,root->path,root->kind==NODE_PIN?NULL:root->label,root->kind==NODE_PIN?APP_PLACE_PIN:APP_PLACE_ROOT);
    }
}

int app_debug_shown(void) { return g.debug_panel; }
void app_debug_show(int on) { show_debug_panel(on); }
void app_debug_save(void) { save_report(); }

void app_debug_tick(void)
{
    if (g.dbg) InvalidateRect(g.dbg, NULL, FALSE);
    InvalidateRect(g.hwnd, NULL, FALSE);
}

void app_debug_reset(void)
{
    fpe_prof_reset();
    g.worst_ms = 0;
    g.worst_msg = 0;
    set_status(active_pane(), 0, L"Profiling counters reset");
    if (g.dbg) InvalidateRect(g.dbg, NULL, FALSE);
}

/* ---- the app's own functions: services with a running cost ------------------------- */

static int svc_shell_start(void)
{
    if (shell_init()) return -1;
    g.com_ready = 1;
    return 0;
}

static int svc_icons_start(void) { return shell_icons_start(g.hwnd, WM_APP_ICONS); }

static void svc_icons_stop(void)
{
    shell_icons_stop();
    if (g.hwnd) RedrawWindow(g.hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

static int svc_drop_warm_wanted(void) { return fn_active(fn_find("svc.drop")); }
static int svc_drop_warm_start(void) { return dropwarm_start()?-1:FN_PENDING; }
static int svc_drop_warm_stop(void) { return dropwarm_stop()?FN_PENDING:0; }

static int svc_drop_start(void)
{
    return shell_drop_register(g.panes[0].hwnd, &pane_drop, &g.panes[0]);
}

static void svc_drop_stop(void) { shell_drop_revoke(g.panes[0].hwnd); }
static int svc_drop_right_start(void) { return shell_drop_register(g.panes[1].hwnd, &pane_drop, &g.panes[1]); }
static void svc_drop_right_stop(void) { shell_drop_revoke(g.panes[1].hwnd); }
static int svc_drop_tree_start(void) { return shell_drop_register(T.hwnd, &tree_drop_handler, NULL); }
static void svc_drop_tree_stop(void) { shell_drop_revoke(T.hwnd); }

static int svc_icons_wanted(void) { return g.icon_style != ICONS_OURS; }

/* Each category takes extra extensions; the examples are ones the built-in table doesn't list. */
#define X(name, eg) { name, "", "extra extensions, e.g. " eg }
static const fn_opt icon_opts[] = {
    { "style", "ours", "ours: by type, everywhere; mixed: programs and shortcuts keep their own; windows: as Windows shows them",
      "ours mixed windows" },
    X("text", "adoc"),          X("document", "wri"),      X("pdf", "fdf"),          X("spreadsheet", "gsheet"),
    X("slides", "gslides"),     X("image", "jp2"),         X("video", "y4m"),        X("audio", "spx"),
    X("archive", "sit"),        X("code", "nix"),          X("script", "nu"),        X("data", "bak"),
    X("database", "realm"),     X("program", "jar"),       X("system", "mun"),       X("shortcut", "library-ms"),
    X("font", "sfd"),           X("disc", "ova"),          X("design", "obj"),
    { "unknown", "", "extensions to show as unknown" },
    { NULL },
};
#undef X
/* view.icons: at startup before anything is drawn, and when its settings change. */
static void configure_icons(void)
{
    const fn_def *d = fn_find("view.icons");
    ti_reset();
    if (!fn_active(d)) {
        g.icon_style = ICONS_WINDOWS;
    } else {
        const char *s = fn_option(d, "style");
        g.icon_style = !_stricmp(s, "mixed") ? ICONS_MIXED : !_stricmp(s, "windows") ? ICONS_WINDOWS : ICONS_OURS;
        for (int k = 1; k < TI_FILE_KINDS; k++) {
            const char *exts = fn_option(d, ti_name((ti_kind)k));
            if (exts[0] && ti_add((ti_kind)k, exts))
                fn_config_error(d, "[view.icons] %s = %s: some extensions are not usable", ti_name((ti_kind)k), exts);
        }
    }
    if (g.hwnd) RedrawWindow(g.hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

static const fn_opt menu_opts[] = {
    { "style", "ours",
      "ours: drawn by Folderwing, Explorer's items under \"More from Windows\"; native: Windows' menu with Explorer's items included",
      "ours native" },
    { NULL },
};

static const fn_opt thumbnail_opts[] = {
    {"mode", "details", "Initial layout in each pane", "details thumbnails"},
    {"size", "medium", "Thumbnail size", "small medium large"},
    {"cache_mb", "32", "Pixel cache in MB; decoded images stay in restricted helpers", "#8..128"},
    {NULL}};
static int thumbnails_service_start(void) { return thumbnails_start(g.hwnd, WM_APP_THUMB); }
static void thumbnails_service_stop(void) { thumbnails_stop(); }
static void configure_thumbnails(void)
{
    const fn_def *d = fn_find("view.thumbnails");
    int was_on = g.thumb_on;
    g.thumb_on = fn_active(d);
    int mode = !strcmp(fn_option(d, "mode"), "thumbnails");
    const char *size = fn_option(d, "size");
    int edge = !strcmp(size, "small") ? 96 : !strcmp(size, "large") ? 256 : 160;
    g.thumb_cache_mb = (uint32_t)strtoul(fn_option(d, "cache_mb"), NULL, 10);
    thumbnails_configure(g.thumb_cache_mb);
    int changed_mode = g.thumb_default != mode, changed_size = g.thumb_default_size != edge;
    g.thumb_default = mode;
    g.thumb_default_size = edge;
    for (int i = 0; i < 2; ++i) {
        Pane *p = &g.panes[i];
        if (!g.thumb_on)
            p->thumbnails = 0;
        else if (!g.hwnd || !was_on || changed_mode)
            p->thumbnails = mode;
        if (!g.hwnd || changed_size)
            p->thumb_size = edge;
        if (folder_view_enabled() && p->path) folder_view_values(p, p->path);
        if (p->hwnd) {
            rename_end(p, 1);
            p_clamp(p);
            p_ensure_visible(p, p->cursor);
            p_redraw(p);
        }
    }
    if (!g.thumb_on && g.hwnd) {
        thumbnails_collect();
        fn_stop(fn_find("svc.thumbnails"));
    }
    g.thumb_default = mode;
    g.thumb_default_size = edge;
    if (g.hwnd)
        SetTimer(g.hwnd, TIMER_THUMB, 25, NULL);
}
static void run_thumbnails(fn_ctx *c)
{
    Pane *p = c->pane;
    if (!p)
        return;
    rename_end(p, 1);
    p->thumbnails = !p->thumbnails;
    if (!p->thumb_size)
        p->thumb_size = g.thumb_default_size ? g.thumb_default_size : 160;
    folder_view_note(p, FV_MODE);
    p_clamp(p);
    p_ensure_visible(p, p->cursor);
    p_redraw(p);
}
static int tiles_apply(const fn_ctx *c) { return c->pane && ((Pane *)c->pane)->thumbnails; }
static void thumbnail_size(fn_ctx *c, int step)
{
    Pane *p = c->pane;
    if (!p)
        return;
    rename_end(p, 1);
    p->thumb_size = step > 0 ? (p->thumb_size < 160 ? 160 : 256) : (p->thumb_size > 160 ? 160 : 96);
    folder_view_note(p, FV_SIZE);
    p_clamp(p);
    p_ensure_visible(p, p->cursor);
    p_redraw(p);
}
static void run_thumbnails_larger(fn_ctx *c) { thumbnail_size(c, 1); }
static void run_thumbnails_smaller(fn_ctx *c) { thumbnail_size(c, -1); }


/* Dock UI is another view of the command registry, with frozen pane targets
 * across mouse-down and nested group/overflow menus. */
static fn_ctx g_dock_context,g_dock_other;
static int g_dock_captured;
static HWND g_dock_source;
static const char *const dock_primary[AD_MORE]={"palette.search","file.find_content","file.compare_files","space.show","file.find_duplicates","git.status","workspace.manage","terminal.show","operations.show","direct.show"};
static void dock_end(void){if(!g_dock_captured)return;ctx_free(&g_dock_context);ctx_free(&g_dock_other);g_dock_captured=0;}
static void dock_enter(HWND previous){if(previous&&IsChild(g.hwnd,previous)&&!actiondock_contains(previous))g_dock_source=previous;}
static void dock_source(void){HWND f=GetFocus();if(f&&IsChild(g.hwnd,f)&&!actiondock_contains(f))g_dock_source=f;if(!g_dock_source||!IsWindow(g_dock_source))g_dock_source=active_pane()->hwnd;}
static void dock_context(fn_ctx *c,int from,int item){dock_source();app_preview_ctx(c,g_dock_source,from);Pane *p=c->pane;if((item==AD_SIZE||item==AD_DUPLICATES)&&!p->nsel&&!(c->state&FN_S_DIR)&&c->where==FN_PANE){ctx_free(c);pane_ctx(c,p,from,0);}if(c->n==1&&(c->state&FN_S_DIR))c->dirs=1;
 /* MENU contexts already gathered exact paths and directory counts. Only
  * lazy applicability queries need to count the selected directory entries. */
 if(item==AD_DUPLICATES&&!c->paths&&c->n>1&&c->n<=DUPLICATE_ROOTS&&p->view){const fpe_dir *dir=fpe_view_dir(p->view);const fpe_entry *entries=fpe_dir_entries(dir);uint32_t count=fpe_dir_count(dir);for(uint32_t i=0;i<count&&i<p->sel_cap;i++)if(p->sel[i]&&fpe_entry_is_dir(&entries[i]))c->dirs++;}}
static void dock_begin(void){if(g_dock_captured)return;dock_context(&g_dock_context,FN_FROM_MENU,-1);pane_ctx(&g_dock_other,other_pane(active_pane()),FN_FROM_MENU,1);g_dock_captured=1;}
static int dock_shown(int item){const wchar_t *shown=sidepanel_app();if(!shown||!sidepanel_open())return 0;const wchar_t *app=item==AD_CONTENT?content_app():item==AD_COMPARE?diff_app():item==AD_SIZE?space_app():item==AD_DUPLICATES?duplicate_app():item==AD_GIT?git_app():item==AD_TERMINAL?term_app():item==AD_OPERATIONS?operations_app():item==AD_DIRECT?direct_app():NULL;return app&&!_wcsicmp(shown,app);}
static void dock_query(int item,ad_state *state){state->enabled=1;if(item==AD_MORE){wcscpy(state->tooltip,L"More tools and action dock options");return;}fn_ctx c;dock_context(&c,FN_FROM_CODE,item);const char *id=dock_primary[item];state->enabled=fn_applies(fn_find(id),&c);if(item==AD_COMPARE)state->enabled|=fn_applies(fn_find("file.compare_folders"),&c);if(item==AD_SIZE)state->enabled|=fn_applies(fn_find("file.folder_size"),&c);state->selected=dock_shown(item);wchar_t keys[128]=L"";fn_key_text(fn_find(id),keys,128);swprintf(state->tooltip,320,L"%ls%ls%ls%ls",ad_labels[item],keys[0]?L" · ":L"",keys,!state->enabled?item==AD_COMPARE?L" · Select one file in each pane, or target folders":L" · Unavailable for this selection or disabled in Settings":L"");ctx_free(&c);}
static int dock_run(int item,const char *id){if(!g_dock_captured)dock_begin();if(!app_menu_context_current(&g_dock_context)||(item==AD_COMPARE&&!app_menu_context_current(&g_dock_other))){app_status(NULL,1,L"Selection changed; choose the dock action again");return 0;}fn_ctx c;dock_context(&c,FN_FROM_MENU,item);uint32_t revision=g_dock_context.menu_revision;int result=fn_run_id_at_revision(id,revision,&c);ctx_free(&c);return result;}
static void dock_change(const char *key,const char *value){if(!g.settings[0]){if(!strcmp(key,"placement"))g_dock_position=!strcmp(value,"left")?AD_LEFT:!strcmp(value,"top")?AD_TOP:!strcmp(value,"hidden")?AD_HIDDEN:AD_TREE_BOTTOM;else if(!strcmp(key,"labels"))g_dock_labels=!strcmp(value,"on");else if(!strcmp(key,"items")){g_dock_mask=0;for(int i=0;i<AD_MORE;i++)if(strstr(value,ad_names[i]))g_dock_mask|=1u<<i;}layout();return;}
 if(g_settings_save){app_status(NULL,1,L"Another settings save is in progress; try again shortly");return;}char *text=NULL,*out=NULL,why[200];size_t n=0,size=0;if(settings_read(&text,&n)){app_status(NULL,1,L"Settings are locked; dock option was not changed");return;}if(bytes_hash(text?text:"",n)!=g_cfg_hash){settings_apply(text,n);free(text);app_status(NULL,1,L"Settings changed elsewhere; choose the dock option again");return;}if(fn_settings_edit(text,n,"view.action_dock",key,value,&out,&size,why,sizeof why)){free(text);app_status(NULL,1,L"Dock option could not be changed");return;}free(text);settings_save *save=calloc(1,sizeof *save);if(!save){free(out);return;}wcscpy(save->path,g.settings);save->text=out;save->len=size;save->expect=g_cfg_hash;save->dock=1;save->started=fpe_prof_begin();if(!SetTimer(g.hwnd,TIMER_SETTINGS_SAVE,25,NULL)){free(out);free(save);return;}save->thread=CreateThread(NULL,65536,settings_commit,save,STACK_SIZE_PARAM_IS_A_RESERVATION,NULL);if(!save->thread){KillTimer(g.hwnd,TIMER_SETTINGS_SAVE);free(out);free(save);return;}g_settings_save=save;}
static void dock_menu_item(HMENU menu,UINT number,const char *id,int item){fn_ctx c;dock_context(&c,FN_FROM_CODE,item);const fn_def *d=fn_find(id);int enabled=fn_applies(d,&c);wchar_t keys[128]=L"",label[320];if(d)fn_key_text(d,keys,128);swprintf(label,320,L"%ls%ls%ls",d&&d->title?d->title:L"Unavailable",keys[0]?L"\t":L"",keys);AppendMenuW(menu,MF_STRING|(enabled?0:MF_GRAYED),number,label);ctx_free(&c);}
static void dock_invoke(int item,POINT point){if(!g_dock_captured)dock_begin();if(item==AD_COMPARE||item==AD_SIZE){HMENU menu=CreatePopupMenu();const char *first=item==AD_COMPARE?"file.compare_files":"file.analyze_space",*second=item==AD_COMPARE?"file.compare_folders":"file.folder_size";dock_menu_item(menu,1,first,item);dock_menu_item(menu,2,second,item);UINT choice=app_menu_track(menu,g.hwnd,point);DestroyMenu(menu);if(choice==1||choice==2)dock_run(item,choice==1?first:second);return;}if(item!=AD_MORE){dock_run(item,dock_primary[item]);return;}
 HMENU menu=CreatePopupMenu(),where=CreatePopupMenu(),buttons=CreatePopupMenu();for(int i=0;i<AD_MORE;i++)if((g_dock_mask&(1u<<i))&&!(g_dock_geometry.visible&(1u<<i))){ad_state state={0};dock_query(i,&state);AppendMenuW(menu,MF_STRING|(state.enabled?0:MF_GRAYED),100+i,ad_labels[i]);}if(GetMenuItemCount(menu))AppendMenuW(menu,MF_SEPARATOR,0,NULL);const wchar_t *locations[]={L"Left edge",L"Tree bottom (two rows)",L"Top",L"Hidden"};for(int i=0;i<4;i++)AppendMenuW(where,MF_STRING|(g_dock_position==i?MF_CHECKED:0),200+i,locations[i]);AppendMenuW(menu,MF_POPUP,(UINT_PTR)where,L"Placement");AppendMenuW(menu,MF_STRING|(g_dock_labels?MF_CHECKED:0),210,L"Show labels");for(int i=0;i<AD_MORE;i++)AppendMenuW(buttons,MF_STRING|(g_dock_mask&(1u<<i)?MF_CHECKED:0),220+i,ad_labels[i]);AppendMenuW(menu,MF_POPUP,(UINT_PTR)buttons,L"Visible buttons");UINT choice=app_menu_track(menu,g.hwnd,point);DestroyMenu(menu);
 if(choice>=100&&choice<100+AD_MORE)dock_invoke((int)choice-100,point);else if(choice>=200&&choice<204){static const char *values[]={"left","tree-bottom","top","hidden"};dock_change("placement",values[choice-200]);}else if(choice==210)dock_change("labels",g_dock_labels?"off":"on");else if(choice>=220&&choice<220+AD_MORE){uint32_t mask=g_dock_mask^(1u<<(choice-220));char items[160]="";for(int i=0;i<AD_MORE;i++)if(mask&(1u<<i)){if(items[0])strcat(items," ");strcat(items,ad_names[i]);}dock_change("items",items);}}
static void dock_leave(int direction){dock_end();if(direction)focus_cycle(actiondock_window(),direction);else focus(active_pane()->hwnd);}
static void run_action_dock(fn_ctx *c){(void)c;POINT point;GetCursorPos(&point);dock_begin();dock_invoke(AD_MORE,point);dock_end();}
static void configure_action_dock(void){const fn_def *d=fn_find("view.action_dock");const char *p=fn_option(d,"placement");g_dock_position=!fn_active(d)||!strcmp(p,"hidden")?AD_HIDDEN:!strcmp(p,"left")?AD_LEFT:!strcmp(p,"top")?AD_TOP:AD_TREE_BOTTOM;g_dock_labels=!strcmp(fn_option(d,"labels"),"on");const char *items=fn_option(d,"items");g_dock_mask=0;for(int i=0;i<AD_MORE;i++)if(strstr(items,ad_names[i]))g_dock_mask|=1u<<i;if(g.hwnd)layout();}
static const fn_opt dock_opts[]={{"placement","tree-bottom","Dock placement; tree bottom temporarily uses the left edge when the tree is hidden","left tree-bottom top hidden"},{"labels","off","Show text beside the action icons","on off"},{"items","find content compare size duplicates git workspaces terminal operations direct","Tools visible on the dock; overflow remains in More","+find content compare size duplicates git workspaces terminal operations direct"},{NULL}};
static int dock_key(UINT vk,int sys,LPARAM l){fn_ctx c;dock_context(&c,FN_FROM_KEY,-1);int ran=fn_key((uint8_t)vk,key_mods(sys&&(l&(1<<29))),&c);ctx_free(&c);if(ran)eat_char(GetFocus(),sys);return ran;}
static const ad_host dock_host={dock_query,dock_begin,dock_end,dock_invoke,dock_leave,focus,dock_key,dock_enter};

static void run_about(fn_ctx *c){(void)c;fw_brand_about(g.hwnd);}

static const fn_def app_functions[] = {
    {.id="help.about",.title=L"About Folderwing",.about="Product version, identity and license information",.life=FN_COLD,.where=(FN_ANYWHERE|FN_ARCHIVE),.run=run_about},
    {.id="view.action_dock",.title=L"Action dock options",.about="Shared tool buttons at the left edge, tree bottom or top; labels, tooltips and overflow",
     .life=FN_COLD,.where=(FN_ANYWHERE|FN_ARCHIVE),.opts=dock_opts,.configure=configure_action_dock,.run=run_action_dock},
    {.id="git.status",.title=L"Git status",.about="Explicit read-only repository snapshot: staged, unstaged, untracked, conflicts, cached badges, diffs and terminal",
     .life=FN_COLD,.where=FN_ANYWHERE,.menus=FN_MENU_FOLDER|FN_MENU_ITEMS|FN_MENU_TREE,.group=7,.keys="Ctrl+Alt+G",.needs="panel.side",.opts=git_opts,.configure=configure_git,.run=run_git_status},
    {.id="git.show",.title=L"Git panel",.life=FN_COLD,.where=FN_ANYWHERE,.needs="git.status",.run=run_git_show},
    {.id="git.result",.title=L"Git result action",.life=FN_COLD,.where=FN_ANYWHERE,.flags=FN_HIDDEN,.needs="git.status",.run=run_git_result},
    {.id="file.find_duplicates",.title=L"Find duplicates",.about="Read-only duplicate groups: size, SHA-256, then verified byte comparison; selected folders or current folder",
     .life=FN_COLD,.where=FN_ANYWHERE,.menus=FN_MENU_ITEMS|FN_MENU_FOLDER|FN_MENU_TREE,.group=7,.needs="panel.side",.applies=duplicate_applies,.run=run_find_duplicates,.configure=configure_duplicates},
    {.id="duplicates.both",.title=L"Find duplicates in both panes",.life=FN_COLD,.where=FN_PANE,.requires=FN_S_DUAL,.menus=FN_MENU_FOLDER,.group=7,.needs="file.find_duplicates",.run=run_duplicates_both},
    {.id="duplicates.show",.title=L"Duplicate finder",.life=FN_COLD,.where=FN_ANYWHERE,.needs="file.find_duplicates",.run=run_duplicates_show},
    {.id="duplicates.reveal_left",.title=L"Reveal duplicate left",.life=FN_COLD,.where=FN_ANYWHERE,.flags=FN_HIDDEN,.needs="file.find_duplicates",.run=run_duplicate_result},
    {.id="duplicates.reveal_right",.title=L"Reveal duplicate right",.life=FN_COLD,.where=FN_ANYWHERE,.flags=FN_HIDDEN,.needs="file.find_duplicates",.run=run_duplicate_result},
    {.id="duplicates.preview",.title=L"Preview duplicate",.life=FN_COLD,.where=FN_ANYWHERE,.flags=FN_HIDDEN,.needs="file.find_duplicates",.run=run_duplicate_result},
    {.id="duplicates.copy_path",.title=L"Copy duplicate path",.life=FN_COLD,.where=FN_ANYWHERE,.flags=FN_HIDDEN,.needs="file.find_duplicates",.run=run_duplicate_result},

    {.id="workspace.manage",.title=L"Saved workspaces",.about="Explicitly save, switch, update, rename and delete named sets of folder tabs and pane layout",
     .life=FN_COLD,.where=(FN_ANYWHERE|FN_ARCHIVE),.keys="Ctrl+Alt+W",.run=workspaces_run,.configure=workspaces_configure},
    {.id="file.compare_files",.title=L"Compare files",.about="Read-only aligned text differences for one file in each pane; linked scrolling, change navigation and full-byte binary comparison",
     .life=FN_COLD,.where=FN_PANE,.requires=FN_S_DUAL|FN_S_ONE,.menus=FN_MENU_ITEMS,.group=7,.needs="panel.side",.keys="Ctrl+Alt+D",.applies=diff_applies,.run=run_compare_files,.configure=configure_diff},
    {.id="diff.show",.title=L"File differences",.life=FN_COLD,.where=FN_ANYWHERE,.needs="file.compare_files",.run=run_diff_show},
    {.id="file.find_content",.title=L"Search inside files",.about="Literal text/code search with line excerpts, file patterns, case/whole-word options and native WSL execution",
     .life=FN_COLD,.where=FN_ANYWHERE,.menus=FN_MENU_ITEMS|FN_MENU_FOLDER|FN_MENU_TREE,.group=7,.needs="panel.side",.keys="Ctrl+Alt+F",.run=run_find_content,.configure=configure_content},
    {.id="content.show",.title=L"Find in files",.life=FN_COLD,.where=(FN_ANYWHERE|FN_ARCHIVE),.needs="file.find_content",.run=run_content_show},
    {.id="content.preview",.title=L"Preview content search match",.life=FN_COLD,.where=FN_ANYWHERE,.flags=FN_HIDDEN,.needs="file.find_content",.run=run_content_result},
    {.id="content.reveal_left",.title=L"Reveal content result left",.life=FN_COLD,.where=FN_ANYWHERE,.flags=FN_HIDDEN,.needs="file.find_content",.run=run_content_result},
    {.id="content.reveal_right",.title=L"Reveal content result right",.life=FN_COLD,.where=FN_ANYWHERE,.flags=FN_HIDDEN,.needs="file.find_content",.run=run_content_result},
    {.id="content.copy_path",.title=L"Copy content result path",.life=FN_COLD,.where=FN_ANYWHERE,.flags=FN_HIDDEN,.needs="file.find_content",.run=run_content_result},
    {.id="file.analyze_space",.title=L"Analyze space",.about="Folder tree, largest files and file types; logical and allocated sizes, Windows and native WSL",
     .life=FN_COLD,.where=FN_ANYWHERE,.menus=FN_MENU_ITEMS|FN_MENU_FOLDER|FN_MENU_TREE,.group=7,.needs="panel.side",
     .applies=foldersize_applies,.run=run_analyze_space,.configure=configure_space},
    {.id="space.show",.title=L"Space analyzer",.life=FN_COLD,.where=(FN_ANYWHERE|FN_ARCHIVE),.needs="file.analyze_space",.run=run_space_show},
    {.id="space.reveal_left",.title=L"Reveal Space result in left pane",.life=FN_COLD,.where=FN_ANYWHERE,.flags=FN_HIDDEN,.needs="file.analyze_space",.run=run_space_result},
    {.id="space.reveal_right",.title=L"Reveal Space result in right pane",.life=FN_COLD,.where=FN_ANYWHERE,.flags=FN_HIDDEN,.needs="file.analyze_space",.run=run_space_result},
    {.id="space.preview",.title=L"Preview Space result",.life=FN_COLD,.where=FN_ANYWHERE,.flags=FN_HIDDEN,.needs="file.analyze_space",.run=run_space_result},
    {.id="space.copy_path",.title=L"Copy Space result path",.life=FN_COLD,.where=FN_ANYWHERE,.flags=FN_HIDDEN,.needs="file.analyze_space",.run=run_space_result},
    {.id="space.analyze_folder",.title=L"Analyze Space result folder",.life=FN_COLD,.where=FN_ANYWHERE,.flags=FN_HIDDEN,.needs="file.analyze_space",.run=run_space_result},
    {.id="file.compare_folders",.title=L"Compare folders...",.about="Compare both pane folders, optionally including subfolders and SHA-256; stage selected differing files in Operations",
     .life=FN_COLD,.where=FN_PANE,.requires=FN_S_DUAL|FN_S_FOLDER,.keys="Ctrl+Shift+F7",.menus=FN_MENU_ITEMS|FN_MENU_FOLDER,.group=7,.run=foldercompare_run},
    {.id="file.bulk_rename",.title=L"Bulk rename...",.about="Preview literal replacements, prefixes, suffixes and numbering; preserve extensions; explicit Apply without replacing targets",
     .life=FN_COLD,.where=FN_PANE,.requires=FN_S_ITEMS,.keys="Ctrl+Shift+F2",.menus=FN_MENU_ITEMS,.group=3,
     .applies=bulkrename_applies,.run=bulkrename_run},
    {.id="file.folder_size",.title=L"Calculate folder size",.about="On-demand background logical size and counts; cancel, partial results and recalculate",
     .life=FN_COLD,.where=FN_ANYWHERE,.menus=FN_MENU_ITEMS|FN_MENU_FOLDER|FN_MENU_TREE,.group=7,
     .applies=foldersize_applies,.run=foldersize_run},
    {.id="file.symlink",.title=L"Create symbolic links",.about="Right-button drop into Linux: relative or absolute links, without replacing items",
     .life=FN_COLD,.where=FN_ANYWHERE,.flags=FN_HIDDEN,.needs="wsl.helper"},
    {.id="file.permissions",.title=L"Permissions...",.about="Edit selected Linux file/folder permissions and ownership, mixed bits, per-item preview and verified results",
     .life=FN_COLD,.where=FN_PANE,.requires=FN_S_ITEMS,.needs="wsl.helper",
     .menus=FN_MENU_ITEMS|FN_MENU_OURS,.group=7,.applies=permissions_applies,.run=permissions_run},
    {.id="tab.drag",.title=L"Drag tabs",.about="Reorder visible tabs or move them to the other visible pane; Escape cancels",
     .life=FN_COLD,.where=FN_PANE,.flags=FN_HIDDEN},
    {.id="file.shortcut",.title=L"Create shortcuts",.about="Right-button drop: create Windows shortcuts without replacing existing files",
     .life=FN_COLD,.where=FN_ANYWHERE,.flags=FN_HIDDEN,.needs="svc.shell"},
    {.id="view.folder_memory",.title=L"Remember folder views",.life=FN_COLD,.configure=configure_folder_views,
     .about="Remember each folder's explicit sort, layout and tile size (up to 256); saved beside the session on exit. --session= disables it"},
    {.id="view.folder_reset",.title=L"Reset this folder's view",.life=FN_COLD,.where=FN_PANE,.menus=FN_MENU_FOLDER|FN_MENU_ITEMS,.group=5,
     .needs="view.folder_memory",.applies=folder_reset_applies,.run=run_folder_reset,
     .about="Forget this folder's choices and use name order and the current thumbnail defaults"},
    {.id="svc.thumbnails",.title=L"Thumbnail worker",.about="Bounded cache and a worker with dedicated restricted image/PDF helpers",
     .life=FN_WARM,.flags=FN_HIDDEN,.idle_ms=30000,.start=thumbnails_service_start,.stop=thumbnails_service_stop},
    {.id="view.thumbnails",.title=L"Thumbnail view",.about="Switch this pane between details and thumbnails; visible files load first",
     .life=FN_COLD,.where=FN_PANE,.keys="Ctrl+Alt+T",.menus=FN_MENU_FOLDER,.opts=thumbnail_opts,.needs="svc.thumbnails",
     .configure=configure_thumbnails,.run=run_thumbnails},
    {.id="view.thumbnails_larger",.title=L"Larger thumbnails",.about="Increase thumbnail tiles: small, medium, large",
     .life=FN_COLD,.where=FN_PANE,.needs="view.thumbnails",.keys="Ctrl+Shift+Up",.menus=FN_MENU_FOLDER,.applies=tiles_apply,.run=run_thumbnails_larger},
    {.id="view.thumbnails_smaller",.title=L"Smaller thumbnails",.about="Decrease thumbnail tiles: large, medium, small",
     .life=FN_COLD,.where=FN_PANE,.needs="view.thumbnails",.keys="Ctrl+Shift+Down",.menus=FN_MENU_FOLDER,.applies=tiles_apply,.run=run_thumbnails_smaller},
    { .id = "svc.shell", .title = L"Shell", .about = "OLE on the UI thread: Explorer menus, drag and drop, Properties",
      .life = FN_HOT, .flags = FN_DEFERRED, .start = svc_shell_start, .stop = shell_shutdown },
    { .id = "svc.icons", .title = L"Windows icons", .about = "The icons apps registered, for view.icons style = mixed or windows (a thread, ~2.3 MB)",
      .life = FN_HOT, .flags = FN_DEFERRED, .start = svc_icons_start, .stop = svc_icons_stop, .wanted = svc_icons_wanted },
    { .id = "view.icons", .title = L"Type icons", .life = FN_COLD, .opts = icon_opts, .configure = configure_icons,
      .about = "Icons that say what a file is, drawn by Folderwing (off: style = windows)" },
    { .id = "svc.drop.warmup", .title = L"Prepare drop support", .about = "Prepare OLE registration on a private pumping thread",
      .life = FN_HOT, .flags = FN_DEFERRED | FN_HIDDEN, .needs = "svc.shell",
      .wanted = svc_drop_warm_wanted, .start = svc_drop_warm_start, .stop_async = svc_drop_warm_stop },
    { .id = "svc.drop", .title = L"Drop target", .about = "Accept files dropped on the panes and the tree",
      .life = FN_HOT, .flags = FN_DEFERRED, .needs = "svc.drop.warmup", .start = svc_drop_start, .stop = svc_drop_stop },
    /* Dependencies keep the master switch authoritative. Each callback owns
     * one registration and its cleanup; the bounded startup pass gives each
     * native call its own message and records registration failures. */
    { .id = "svc.drop.right", .title = L"Right pane drop target", .life = FN_HOT,
      .flags = FN_DEFERRED | FN_HIDDEN, .needs = "svc.drop", .start = svc_drop_right_start, .stop = svc_drop_right_stop },
    { .id = "svc.drop.tree", .title = L"Tree drop target", .life = FN_HOT,
      .flags = FN_DEFERRED | FN_HIDDEN, .needs = "svc.drop.right", .start = svc_drop_tree_start, .stop = svc_drop_tree_stop },
    { .id = "menu.context", .title = L"Folderwing context menu", .life = FN_COLD, .opts = menu_opts,
      .about = "Right-click menus drawn by Folderwing: open in ~1 ms, Explorer's items one click away (off: style = native)" },
    { .id = "menu.explorer", .title = L"Explorer menu items", .where = FN_ANYWHERE,
      .about = "Explorer's items: \"More from Windows\" in menus (native style: in every menu). "
               "The first one loads shell extensions: ~13 MB, ~1.6 s",
      .life = FN_COLD, .needs = "svc.shell" },
    { .id = "item.drag_out", .title = L"Drag out", .about = "Drag items to other apps and folders",
      .life = FN_COLD, .flags = FN_HIDDEN, .where = FN_ANYWHERE, .requires = FN_S_ITEMS, .needs = "svc.shell",
      .run = run_drag_out },
    /* switches */
    { .id = "view.breadcrumbs", .title = L"Breadcrumbs", .life = FN_COLD, .configure = configure_crumbs,
      .about = "Address bar as clickable folders; click empty space or Ctrl+L to type (off: a plain text box)" },
    { .id = "session.restore", .title = L"Restore session", .life = FN_COLD, .flags = FN_RESTART,
      .about = "Reopen with the last size, layout and tabs (only each pane's shown tab is read at startup)" },
    { .id = "nav.recent", .title = L"Recent folders", .life = FN_COLD,
      .about = "Remember the last 50 folders shown, for Go to (saved with the session)" },
    { .id = "wsl.helper", .title = L"WSL folders through Linux", .life = FN_COLD, .opts = wsl_opts, .configure = configure_wsl,
      .about = "List and watch folders in WSL distros with folderwing-wsl, a Linux helper run by wsl.exe: one per distro, "
               "started on its first folder, stopped 60 s after the last one closes (off: read over \\\\wsl.localhost, no refresh)" },
    { .id="preview.media",.title=L"Audio and video preview",.life=FN_WARM,.idle_ms=30000,.opts=media_opts,
      .about="Paused media preview, playback, seeking and volume in an on-demand helper. Media Foundation first; libmpv only for unsupported formats.",
      .start=media_start,.stop=media_stop,.configure=configure_media },
    { .id="preview.archive",.title=L"Archive preview",.life=FN_WARM,.idle_ms=30000,
      .about="Browse archives beside the list; preview entries in a third pane. On-demand miniz and libarchive helpers.",
      .start=archive_start,.stop=archive_stop,.configure=configure_archive,.tick=archiveview_tick },
    { .id="archive.preview",.title=L"Open / preview",.life=FN_COLD,.where=FN_ARCHIVE,.needs="preview.archive",
      .menus=FN_MENU_ARCHIVE,.applies=archiveview_can_open,.run=archiveview_preview },
    { .id="archive.extract",.title=L"Extract file…",.life=FN_COLD,.where=FN_ARCHIVE,.needs="preview.archive",
      .menus=FN_MENU_ARCHIVE,.applies=archiveview_can_extract,.run=archiveview_extract },
    { .id="archive.copy_name",.title=L"Copy name",.life=FN_COLD,.where=FN_ARCHIVE,.needs="preview.archive",
      .menus=FN_MENU_ARCHIVE,.applies=archiveview_scope,.run=archiveview_copy_name },
    { .id="archive.copy_path",.title=L"Copy archive-relative path",.life=FN_COLD,.where=FN_ARCHIVE,.needs="preview.archive",
      .keys="Ctrl+C",.menus=FN_MENU_ARCHIVE,.applies=archiveview_scope,.run=archiveview_copy_path },
    { .id="archive.up",.title=L"Up in archive",.life=FN_COLD,.where=FN_ARCHIVE,.needs="preview.archive",
      .menus=FN_MENU_ARCHIVE,.applies=archiveview_can_up,.run=archiveview_up },
    { .id="archive.close",.title=L"Close archive preview",.life=FN_COLD,.where=FN_ARCHIVE,.needs="preview.archive",
      .menus=FN_MENU_ARCHIVE,.applies=archiveview_scope,.run=archiveview_close },
    { .id = "preview.pdf", .title = L"PDF preview", .life = FN_WARM, .idle_ms = 30000, .opts = preview_opts,
      .about = "Read the PDF under the cursor beside the list (folderwing-pdf.exe + PDFium: a sandboxed process, stopped 30 s after use)",
      .start = preview_start, .stop = preview_stop, .configure = configure_preview },
    { .id = "preview.find", .title = L"Find in PDF", .life = FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE), .needs = "preview.pdf",
      .keys = "Ctrl+F", .applies = preview_shown, .run = run_preview_find,
      .about = "Find words in the PDF the preview shows (the helper searches page by page; nothing waits for it)" },
    { .id = "preview.find_next", .title = L"Next match in PDF", .life = FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE),
      .needs = "preview.pdf", .keys = "F3", .applies = preview_shown, .run = run_preview_next,
      .about = "The next match of Find in PDF" },
    { .id = "preview.find_prev", .title = L"Previous match in PDF", .life = FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE),
      .needs = "preview.pdf", .keys = "Shift+F3", .applies = preview_shown, .run = run_preview_prev,
      .about = "The previous match of Find in PDF" },
    { .id = "preview.colors", .title = L"PDF colors: mixed, dark, original", .life = FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE),
      .needs = "preview.pdf", .applies = preview_shown, .run = run_preview_colors,
      .about = "Switch the preview between mixed, dark and original colors for this session ([preview.pdf] colors sets the start)" },
    { .id="preview.image", .title=L"Image preview", .life=FN_WARM, .idle_ms=30000, .opts=image_opts,
      .about="View images beside the list with WIC and bundled WebP in a restricted helper; idle resources released after 30 seconds",
      .start=image_start, .stop=image_stop, .configure=configure_image },
    { .id="image.fit", .title=L"Fit image", .life=FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE), .needs="preview.image",
      .keys="Ctrl+0", .menus=FN_MENU_IMAGE, .applies=image_shown, .run=run_image_fit, .about="Fit the whole image without enlarging small images" },
    { .id="image.width", .title=L"Fit image width", .life=FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE), .needs="preview.image",
      .keys="Ctrl+2", .menus=FN_MENU_IMAGE, .applies=image_shown, .run=run_image_width, .about="Fit the width of the image" },
    { .id="image.actual", .title=L"Image actual size (100%)", .life=FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE), .needs="preview.image",
      .keys="Ctrl+1", .menus=FN_MENU_IMAGE, .applies=image_shown, .run=run_image_actual, .about="One image pixel per screen pixel" },
    { .id="image.zoom_in", .title=L"Zoom image in", .life=FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE), .needs="preview.image",
      .keys="Ctrl+Plus, Ctrl+Shift+Plus", .menus=FN_MENU_IMAGE, .applies=image_shown, .run=run_image_zoom_in, .about="Zoom around the centre of the image view" },
    { .id="image.zoom_out", .title=L"Zoom image out", .life=FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE), .needs="preview.image",
      .keys="Ctrl+Minus", .menus=FN_MENU_IMAGE, .applies=image_shown, .run=run_image_zoom_out, .about="Zoom around the centre of the image view" },
    { .id="image.background", .title=L"Image transparency background", .life=FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE), .needs="preview.image",
      .menus=FN_MENU_IMAGE, .applies=image_shown, .run=run_image_background, .about="Switch checkerboard, white and dark backgrounds" },
    { .id="image.rotate", .title=L"Rotate image view", .life=FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE), .needs="preview.image",
      .keys="Ctrl+Shift+R", .menus=FN_MENU_IMAGE, .applies=image_shown, .run=run_image_rotate, .about="Rotate the view clockwise; the file is unchanged" },
    { .id="image.flip", .title=L"Flip image view", .life=FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE), .needs="preview.image",
      .keys="Ctrl+Shift+H", .menus=FN_MENU_IMAGE, .applies=image_shown, .run=run_image_flip, .about="Flip the view horizontally; the file is unchanged" },
    { .id="image.metadata", .title=L"Image metadata", .life=FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE), .needs="preview.image",
      .keys="Ctrl+I", .menus=FN_MENU_IMAGE, .applies=image_shown, .run=run_image_metadata, .about="Show camera, date, orientation and frame count when available" },
    { .id="image.copy", .title=L"Copy image pixels", .life=FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE), .needs="preview.image",
      .keys="Ctrl+Shift+C", .menus=FN_MENU_IMAGE, .applies=image_shown, .run=run_image_copy, .about="Copy the full oriented image with transparency; Ctrl+C still copies the file" },
    { .id="image.previous_file",.title=L"Previous image",.life=FN_COLD,.where=(FN_ANYWHERE|FN_ARCHIVE),.needs="preview.image",
      .keys="Ctrl+PageUp",.menus=FN_MENU_IMAGE,.applies=image_previous_file,.run=run_image_previous_file,.about="Browse the previous supported image in the source pane's visible sort/filter order" },
    { .id="image.next_file",.title=L"Next image",.life=FN_COLD,.where=(FN_ANYWHERE|FN_ARCHIVE),.needs="preview.image",
      .keys="Ctrl+PageDown",.menus=FN_MENU_IMAGE,.applies=image_next_file,.run=run_image_next_file,.about="Browse the next supported image in the source pane's visible sort/filter order" },
    { .id="image.previous_page",.title=L"Previous image page",.life=FN_COLD,.where=(FN_ANYWHERE|FN_ARCHIVE),.needs="preview.image",
      .keys="Ctrl+Shift+PageUp",.menus=FN_MENU_IMAGE,.applies=image_previous_page,.run=run_image_previous_page,.about="View the previous TIFF page without wrapping; retain zoom or fit" },
    { .id="image.next_page",.title=L"Next image page",.life=FN_COLD,.where=(FN_ANYWHERE|FN_ARCHIVE),.needs="preview.image",
      .keys="Ctrl+Shift+PageDown",.menus=FN_MENU_IMAGE,.applies=image_next_page,.run=run_image_next_page,.about="Decode the next TIFF page on demand without wrapping; retain zoom or fit" },
    { .id="image.play", .title=L"Play / pause animation", .life=FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE), .needs="preview.image",
      .keys="Ctrl+Shift+Space", .menus=FN_MENU_IMAGE, .applies=image_animated, .run=run_image_play, .about="Play or pause GIF/WebP; Space also works while the image reader has focus" },
    { .id="image.previous", .title=L"Previous animation frame", .life=FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE), .needs="preview.image",
      .keys="Ctrl+Shift+Left", .menus=FN_MENU_IMAGE, .applies=image_animated, .run=run_image_previous, .about="Pause and step backward one composited frame" },
    { .id="image.next", .title=L"Next animation frame", .life=FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE), .needs="preview.image",
      .keys="Ctrl+Shift+Right", .menus=FN_MENU_IMAGE, .applies=image_animated, .run=run_image_next, .about="Pause and step forward one composited frame" },
    { .id="image.source", .title=L"View SVG source", .life=FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE), .needs="preview.text",
      .keys="Ctrl+Shift+U", .menus=FN_MENU_IMAGE, .applies=image_source_scope, .run=run_image_source, .about="Read SVG markup in the text preview" },
    { .id="text.image", .title=L"Render SVG image", .life=FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE), .needs="preview.image",
      .keys="Ctrl+Shift+U", .menus=FN_MENU_TEXT, .applies=text_image_scope, .run=run_text_image, .about="Return from SVG source to its rendered preview" },
    { .id="image.open", .title=L"Open image in default app", .life=FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE), .needs="preview.image",
      .menus=FN_MENU_IMAGE, .applies=image_shown, .run=run_image_open, .about="Open the previewed image using its Windows association" },
    { .id="preview.text", .title=L"Text and code preview", .life=FN_WARM, .idle_ms=30000, .opts=text_opts,
      .about="Read a bounded file prefix beside the list; decoding, syntax, layout and search run in a restricted helper",
      .start=text_start, .stop=text_stop, .configure=configure_text },
    { .id="text.wrap", .title=L"Text word wrap", .life=FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE), .needs="preview.text",
      .keys="Alt+Z", .menus=FN_MENU_TEXT, .applies=text_shown, .run=run_text_wrap, .about="Toggle wrapping without changing the file" },
    { .id="text.lines", .title=L"Text line numbers", .life=FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE), .needs="preview.text",
      .menus=FN_MENU_TEXT, .applies=text_shown, .run=run_text_lines, .about="Toggle physical source line numbers" },
    { .id="text.find", .title=L"Find in text", .life=FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE), .needs="preview.text",
      .keys="Ctrl+F", .menus=FN_MENU_TEXT, .applies=text_shown, .run=run_text_find, .about="Find words in the loaded preview; search is case-insensitive" },
    { .id="text.next", .title=L"Next match in text", .life=FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE), .needs="preview.text",
      .keys="F3", .menus=FN_MENU_TEXT, .applies=text_shown, .run=run_text_next, .about="Go to the next match, wrapping at the last match" },
    { .id="text.prev", .title=L"Previous match in text", .life=FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE), .needs="preview.text",
      .keys="Shift+F3", .menus=FN_MENU_TEXT, .applies=text_shown, .run=run_text_prev, .about="Go to the previous match" },
    { .id="text.select_all", .title=L"Select preview text", .life=FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE), .needs="preview.text",
      .keys="Ctrl+A", .menus=FN_MENU_TEXT, .applies=text_key_scope, .run=run_text_select_all, .about="Select the entire loaded prefix in the viewer" },
    { .id="text.copy", .title=L"Copy selected preview text", .life=FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE), .needs="preview.text",
      .keys="Ctrl+C, Ctrl+Insert", .menus=FN_MENU_TEXT, .applies=text_copy_scope, .run=run_text_copy, .about="Copy the selected text; without a selection Ctrl+C copies the file" },
    { .id="text.encoding", .title=L"Text encoding", .life=FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE), .needs="preview.text",
      .menus=FN_MENU_TEXT, .applies=text_shown, .run=run_text_encoding, .about="Cycle Auto, UTF-8, UTF-16, UTF-32 and Windows-1252; settings can choose a default" },
    { .id="text.open", .title=L"Open text in default app", .life=FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE), .needs="preview.text",
      .menus=FN_MENU_TEXT, .applies=text_shown, .run=run_text_open, .about="Open the previewed file in its default Windows app" },
    /* settings */
    { .id = "panel.side", .title = L"Side panel", .life = FN_COLD, .opts = panel_opts, .configure = configure_panel,
      .about = "The area at the right where the settings and the terminal show; each app runs as a process of its own" },
    { .id="settings.commands",.title=L"Custom commands",.about="Create custom commands and import selected Explorer actions in Settings",
      .life=FN_COLD,.where=FN_ANYWHERE,.needs="settings.show",.run=run_settings_commands },
    { .id = "settings.show", .title = L"Settings", .life = FN_COLD, .where=(FN_ANYWHERE|FN_ARCHIVE), .keys = "Ctrl+Comma",
      .run = run_settings_show,
      .about = "Show or hide every function and its settings in the side panel (also the gear in the status bar)" },
    { .id = "terminal.show", .title = L"Terminal", .life = FN_COLD, .where = (FN_ANYWHERE|FN_ARCHIVE), .keys = "Ctrl+Backquote",
      .opts = term_opts, .configure = configure_terminal, .run = run_terminal_show,
      .about = "Show or hide the terminal in the side panel (folderwing-terminal.exe: shells on pseudo consoles, tabs, history). "
               "It keeps running while hidden; finished sessions stay until left, then go to its history" },
    { .id = "direct.show", .title = L"Direct agent", .life = FN_COLD, .where = FN_ANYWHERE,
      .needs = "panel.side", .run = run_direct_show, .configure = configure_direct,
      .about = "Open the optional native Direct client in the side panel; its runtime is started separately" },
    { .id = "settings.edit", .title = L"Edit settings file", .life = FN_COLD, .where = FN_ANYWHERE,
      .run = run_settings_edit,
      .about = "Open folderwing.ini in your editor; a missing one is made first, listing everything commented out" },
    { .id = "settings.watch", .title = L"Apply saved settings", .life = FN_HOT, .flags = FN_DEFERRED,
      .start = settings_watch_start, .stop = settings_watch_stop,
      .about = "Apply the settings file whenever it is saved (no thread: the window's message loop waits on its folder)" },
};

/* ---- session ------------------------------------------------------------------------- */

static session g_ses; /* as read at startup; tab paths move into the panes */

/* --session=FILE; "--session=" alone: nothing restored or saved. Otherwise
 * session.ini next to the settings file, or in %APPDATA%\Folderwing. */
static void session_locate(const wchar_t *given)
{
    if (given) {
        _snwprintf(g.session, MAX_PATH + 32, L"%ls", given);
    } else if (g.settings[0] && wcsrchr(g.settings, L'\\')) {
        wcscpy(g.session, g.settings);
        wcscpy(wcsrchr(g.session, L'\\') + 1, L"session.ini");
    } else {
        wchar_t base[MAX_PATH+32]={0},path[MAX_PATH+32]={0};
        DWORD n=GetEnvironmentVariableW(L"APPDATA",base,MAX_PATH+32);
        if(n&&n<MAX_PATH+32){fw_settings_path(path,MAX_PATH+32,NULL,base);wchar_t *slash=wcsrchr(path,L'\\');if(slash){wcscpy(slash+1,L"session.ini");wcscpy(g.session,path);}else g.session[0]=0;}
        else g.session[0]=0;
    }
    g.session[MAX_PATH + 31] = 0;
}

static void folder_views_load(void)
{
    if (!g.session[0]) return;
    DWORD n = GetFullPathNameW(g.session, MAX_PATH + 32, g_folder_file, NULL);
    if (!n || n >= MAX_PATH + 32) { g_folder_file[0] = 0; return; }
    wchar_t *slash = wcsrchr(g_folder_file, L'\\');
    if (!slash || slash - g_folder_file + 18 >= MAX_PATH + 32) { g_folder_file[0] = 0; return; }
    wcscpy(slash + 1, L"folder-views.txt");
    fv_load(&g_folder_views, g_folder_file);
}
static void folder_views_save(void)
{
    if (!g_folder_file[0]) return;
    DWORD err = fv_save(&g_folder_views, g_folder_file);
    if (err) {
        wchar_t msg[160];
        _snwprintf(msg, 160, L"Folderwing: folder views could not be saved (Windows error %lu)\n", err);
        OutputDebugStringW(msg);
    }
}

static void session_load(void)
{
    session_defaults(&g_ses);
    if (!g.session[0]) return;
    HANDLE f = CreateFileW(g.session, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size;
    char *buf = NULL;
    DWORD got = 0;
    if (GetFileSizeEx(f, &size) && size.QuadPart < (1 << 20) && (buf = malloc((size_t)size.QuadPart + 1)))
        ReadFile(f, buf, (DWORD)size.QuadPart, &got, NULL);
    CloseHandle(f);
    session_parse(buf, got, &g_ses);
    free(buf);
}

/* Written to a temporary file and moved over the old one, so a crash
 * mid-write never leaves half a session. */
static void session_save(void)
{
    if (!g.session[0] || !g.hwnd) return;
    session s;
    session_defaults(&s);
    WINDOWPLACEMENT wp = { sizeof wp };
    if (GetWindowPlacement(g.hwnd, &wp)) {
        s.has_rect = 1;
        s.rect = wp.rcNormalPosition;
        s.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
    }
    s.tree = g.show_tree;
    s.dual = g.dual;
    s.hidden = g.show_hidden;
    s.active = g.active;
    s.ratio = g.ratio;
    s.tree_w = MulDiv(g.tree_w, 96, (int)g.dpi);
    for (int i = 0; i < 2; i++) { /* borrowed pointers: s is not freed */
        Pane *p = &g.panes[i];
        for (int k = 0; k < p->ntabs; k++) {
            const fpe_view_opts *o = k == p->tab ? &p->opts : &p->tabs[k].opts;
            s.tabs[i][k] = (ses_tab){ p->tabs[k].path, (uint8_t)o->key, o->descending };
        }
        s.ntabs[i] = p->ntabs;
        s.cur[i] = p->tab;
    }
    for (int i = 0; i < tree_pin_count() && s.npins < SES_PINS_MAX; i++) s.pins[s.npins++] = T.roots[i]->path;
    for (int i = 0; i < g.nrecent; i++) s.recent[s.nrecent++] = g.recent[i];

    static char buf[512 * 1024];
    size_t n = session_format(&s, buf, sizeof buf);
    if (!n) return;
    wchar_t dir[MAX_PATH + 32], tmp[MAX_PATH + 40];
    wcscpy(dir, g.session);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) {
        *slash = 0;
        CreateDirectoryW(dir, NULL);
    }
    _snwprintf(tmp, MAX_PATH + 40, L"%ls.tmp", g.session);
    tmp[MAX_PATH + 39] = 0;
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w;
    BOOL ok = WriteFile(f, buf, (DWORD)n, &w, NULL) && w == n;
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp, g.session, MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp);
}

/* ---- registry host ------------------------------------------------------------------- */

static uint64_t host_now(void) { return GetTickCount64(); }

static void host_arm(uint32_t ms)
{
    if (!g.hwnd) return;
    if (ms) SetTimer(g.hwnd, TIMER_FN, ms, NULL);
    else KillTimer(g.hwnd, TIMER_FN);
}

static void host_status(int err, const wchar_t *msg) { if(!g.destroyed)set_status(active_pane(), err, L"%ls", msg); }

/* --settings=FILE bypasses migration, including the empty defaults-only path.
 * Default discovery copies legacy product data before loading the new name. */
static void load_settings(const wchar_t *given)
{
    if (given) {
        _snwprintf(g.settings, MAX_PATH + 32, L"%ls", given);
    } else {
        wchar_t portable[MAX_PATH+32]={0},appdata[MAX_PATH+32]={0};
        DWORD n=GetModuleFileNameW(NULL,portable,MAX_PATH);
        wchar_t *slash=n&&n<MAX_PATH?wcsrchr(portable,L'\\'):NULL;
        if(slash)*slash=0;else portable[0]=0;
        n=GetEnvironmentVariableW(L"APPDATA",appdata,MAX_PATH+32);
        if(!n||n>=MAX_PATH+32)appdata[0]=0;
        DWORD error=fw_settings_path(g.settings,MAX_PATH+32,portable,appdata);
        if(error){wchar_t message[160];swprintf(message,160,L"Folderwing: settings migration deferred (Windows error %lu); legacy data retained.\n",error);OutputDebugStringW(message);}
    }
    g.settings[MAX_PATH + 31] = 0;
    char *buf = NULL;
    size_t len = 0;
    if (g.settings[0]) settings_read(&buf, &len);
    g_cfg_hash = bytes_hash(buf ? buf : "", len);
    fn_settings(buf, len);
    free(buf);
}

/* --functions: the registry as text, to stdout (or the console that
 * started us; this is a GUI program). */
static void list_functions(void)
{
    static char buf[48 * 1024];
    int n = snprintf(buf, sizeof buf, "Folderwing functions   settings: %ls%ls\n\n", g.settings[0] ? g.settings : L"(defaults)",
                     g.settings[0] && GetFileAttributesW(g.settings) == INVALID_FILE_ATTRIBUTES ? L" (not found)" : L"");
    if (n < 0) n = 0;
    fn_list(buf + n, sizeof buf - (size_t)n);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    int console = 0;
    if (!out || out == INVALID_HANDLE_VALUE) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
        out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        console = 1;
    }
    DWORD w;
    WriteFile(out, buf, (DWORD)strlen(buf), &w, NULL);
    if (console) CloseHandle(out);
}

/* The children move in one batch (DeferWindowPos): one pass of size and
 * repaint messages instead of one per window, and nothing half moved shows.
 * A window already where it belongs is left alone. */
static HDWP g_dwp;

static void place(HWND h, int x, int y, int w, int hh)
{
    if (!h) return;
    RECT r;
    GetWindowRect(h, &r);
    MapWindowPoints(NULL, g.hwnd, (POINT *)&r, 2);
    if (IsWindowVisible(h) && r.left == x && r.top == y && r.right - r.left == w && r.bottom - r.top == hh) return;
    /* NOCOPYBITS: each child repaints all of itself anyway; Windows' copy of
     * the old pixels would show for a moment in the wrong place */
    /* DEFERERASE is a SetWindowPos flag; DeferWindowPos rejects it with
     * ERROR_INVALID_PARAMETER, discarding the batch. NOREDRAW queues paint. */
    UINT f = SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOCOPYBITS | SWP_NOREDRAW;
    if (g_dwp) {
        g_dwp = DeferWindowPos(g_dwp, h, NULL, x, y, w, hh, f);
        if (!g_dwp) FPE_PROF_COUNT("layout.batch_failed");
    }
    if (!g_dwp) SetWindowPos(h, NULL, x, y, w, hh, f); /* the batch failed: the rest one by one */
}

static void unplace(HWND h)
{
    if (h && IsWindowVisible(h)) ShowWindow(h, SW_HIDE);
}

static void layout(void)
{
    int64_t t = fpe_prof_begin();
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    g.cw = rc.right;
    g.ch = rc.bottom;
    int h = g.ch - g.status_h;
    if (h < 0) h = 0;
    g_dwp = BeginDeferWindowPos(8);
    if (!g_dwp) FPE_PROF_COUNT("layout.batch_failed");

    if (g.debug_panel && g.dbg) {
        int dh = h / 2 < S(340) ? h / 2 : S(340);
        if (dh < S(160)) dh = S(160);
        h -= dh + g.split;
        if (h < 0) h = 0;
        place(g.dbg, 0, h + g.split, g.cw, dh);
    } else {
        unplace(g.dbg);
    }

    const wchar_t *shown_space=sidepanel_app();int space_main=sidepanel_open()&&shown_space&&!_wcsicmp(shown_space,space_app());
    actiondock_geometry(g.cw,h,space_main?0:g.show_tree,g.tree_w,g.dpi,g_dock_position,g_dock_labels,g_dock_mask,&g_dock_geometry);
    g.body_x=g_dock_geometry.body.left;g.body_y=g_dock_geometry.body.top;
    g.tree_w=g_dock_geometry.tree_width;
    if(g_dock_position==AD_HIDDEN){if(actiondock_contains(GetFocus()))focus(active_pane()->hwnd);actiondock_destroy();}
    else{if(!actiondock_window())actiondock_create(g.hwnd,&dock_host);actiondock_place(&g_dock_geometry,g.dpi,g_dock_labels,g.font);}
    h-=g.body_y;int x=g.body_x;
    if(space_main){unplace(T.hwnd);unplace(g.panes[0].hwnd);unplace(g.panes[1].hwnd);unplace(pdfview_window());unplace(imageview_window());unplace(textview_window());unplace(archiveview_window());unplace(mediaview_window());g.gap_tree_x=g.gap_pane_x=g.ar_gap=-1000;sidepanel_place(x,g.body_y,g.cw-x,h);goto layout_finish;}
    if(g.show_tree){RECT r=g_dock_geometry.tree;place(T.hwnd,r.left,r.top,r.right-r.left,r.bottom-r.top);g.gap_tree_x=g.body_x+g.tree_w;x=g.gap_tree_x+g.split;}
    else{unplace(T.hwnd);g.gap_tree_x=-1000;}

    int avail = g.cw - x;
    int side = sidepanel_width(avail);
    if (side) {
        avail -= side;
        int64_t trace = fpe_prof_begin();
        sidepanel_place(x + avail, g.body_y, side, h);
        FPE_PROF_END("layout.panel", trace);
    }
    HWND reader = preview_window();g.ar_gap=-1000;
    if (g.dual || g.pv_shown) {
        /* the preview takes the right side; a PDF from the right pane swaps the panes first */
        Pane *left = &g.panes[g.pv_swapped], *right = &g.panes[!g.pv_swapped];
        int w0 = (int)((avail - g.split) * g.ratio);
        place(left->hwnd, x, g.body_y, w0, h);
        if (g.pv_shown && reader) {
            int rx=x+w0+g.split,rw=avail-w0-g.split;
            if(g.pv_shown==4&&g.ar_kind){if(g.ar_ratio<0.15||g.ar_ratio>0.85)g.ar_ratio=0.5;int aw=(int)((rw-g.split)*g.ar_ratio);if(aw<1)aw=1;place(reader,rx,g.body_y,aw,h);place(archive_content_window(),rx+aw+g.split,g.body_y,rw-aw-g.split>0?rw-aw-g.split:1,h);g.ar_gap=rx+aw;g.ar_left=rx;g.ar_width=rw;}
            else place(reader,rx,g.body_y,rw,h);
            unplace(right->hwnd);
        } else {
            place(right->hwnd, x + w0 + g.split, g.body_y, avail - w0 - g.split, h);
        }
        g.gap_pane_x = x + w0;
    } else {
        place(g.panes[0].hwnd, x, g.body_y, avail, h);
        unplace(g.panes[1].hwnd);
        g.gap_pane_x = -1000;
    }
    if(g.pv_shown!=5&&g.ar_kind!=5)unplace(mediaview_window());
    if(g.pv_shown!=1&&g.ar_kind!=1)unplace(pdfview_window());
    if(g.pv_shown!=2&&g.ar_kind!=2)unplace(imageview_window());
    if(g.pv_shown!=3&&g.ar_kind!=3)unplace(textview_window());if(g.pv_shown!=4)unplace(archiveview_window());
layout_finish:;
    int64_t batch = fpe_prof_begin();
    if (g_dwp) {
        if (EndDeferWindowPos(g_dwp)) FPE_PROF_COUNT("layout.batch_applied");
        else FPE_PROF_COUNT("layout.batch_failed");
    }
    FPE_PROF_END("layout.batch", batch);
    g_dwp = NULL;
    palette_layout();
    /* Queue one complete frame after geometry settles. Live resize/drag uses
     * layout_now to explicitly paint before the next pointer movement. */
    int64_t invalidate = fpe_prof_begin();
    RedrawWindow(g.hwnd, NULL, NULL, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
    FPE_PROF_END("layout.invalidate", invalidate);
    FPE_PROF_END("layout", t);
}

/* While something is dragged or the window is sized: laid out and painted
 * before the next mouse move is taken, or moves (input) would keep paints
 * waiting and the window would show stale pixels until the mouse stops. */
static void layout_now(void)
{
    layout();
    for(int i=0;i<2;i++)p_finish_geometry(&g.panes[i]);
    RedrawWindow(g.hwnd, NULL, NULL, RDW_UPDATENOW | RDW_ALLCHILDREN);
}

/* The last of the mouse moves already queued for h (a drag only needs where the mouse is now). */
static LPARAM latest_move(HWND h, LPARAM l)
{
    MSG m;
    while (PeekMessageW(&m, h, WM_MOUSEFIRST, WM_MOUSELAST, PM_NOREMOVE) && m.message == WM_MOUSEMOVE) {
        PeekMessageW(&m, h, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_REMOVE);
        l = m.lParam;
    }
    return l;
}

static int hit_splitter(int x, int y)
{
    if (y < g.body_y || y >= g.ch - g.status_h) return 0;
    if (g.debug_panel && g.dbg) {
        RECT r;
        GetWindowRect(g.dbg, &r);
        POINT pt = { 0, r.top };
        ScreenToClient(g.hwnd, &pt);
        if (y >= pt.y - g.split) return 0;
    }
    int slop = S(2);
    if(g.pv_shown==4&&g.ar_kind&&x>=g.ar_gap-slop&&x<g.ar_gap+g.split+slop)return 3;
    if (g.show_tree && x >= g.gap_tree_x - slop && x < g.gap_tree_x + g.split + slop) return 1;
    if ((g.dual || g.pv_shown) && x >= g.gap_pane_x - slop && x < g.gap_pane_x + g.split + slop) return 2;
    return 0;
}

static void main_paint(HDC dc)
{
    int64_t t = fpe_prof_begin();
    SelectObject(dc, g.font);
    fill(dc, 0, 0, g.cw, g.ch, C_SPLIT);

    int sy = g.ch - g.status_h;
    fill(dc, 0, sy, g.cw, g.status_h, C_STATUS);
    fill(dc, 0, sy, g.cw, 1, C_SPLIT);
    wchar_t status[400];swprintf(status,400,L"%ls%ls",g_content_state==CF_RUNNING&&sidepanel_running(content_app())?L"Content search · ":g_space_state==SP_RUNNING&&sidepanel_running(space_app())?L"Space scanning · ":L"",g.job_text);
    text(dc,status,-1,g.pad*2,sy,g.cw/2,g.status_h,C_TEXT,DT_LEFT);

    int gw = S(28);
    int ow = g_operations.n && fn_active(fn_find("operations.show")) ? S(100) : 0;
    g_operations_button = (RECT){ g.cw - gw - ow, sy, g.cw - gw, sy + g.status_h };
    if (ow) {
        fill(dc, g_operations_button.left, sy + 1, ow, g.status_h - 1, C_EDIT);
        text(dc, L"Operations", -1, g_operations_button.left, sy, ow, g.status_h, C_ACCENT, DT_CENTER);
    }
    g_gear = (RECT){ g.cw - gw, sy, g.cw, sy + g.status_h };
    if (sidepanel_open()) fill(dc, g_gear.left, sy + 1, gw, g.status_h - 1, C_EDIT);
    text(dc, L"\x2699", -1, g_gear.left, sy, gw, g.status_h, sidepanel_open() ? C_ACCENT : C_DIM, DT_CENTER);

    PROCESS_MEMORY_COUNTERS pmc = { sizeof pmc };
    wchar_t mem[32] = L"", right[300];
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) fmt_size(mem, 32, pmc.WorkingSetSize);
    _snwprintf(right, 300, L"%ls%lsstartup %.0f ms   RAM %ls", fpe_prof_enabled() ? L"PROFILING (F12)   " : L"",
               g.show_hidden ? L"hidden shown   " : L"", g.startup_ms, mem);
    right[299] = 0;
    text(dc, right, -1, g.cw / 2, sy, g.cw / 2 - g.pad - gw - ow, g.status_h, fpe_prof_enabled() ? C_ACCENT : C_DIM,
         DT_RIGHT);
    FPE_PROF_END("status.paint", t);
}

static void update_metrics(void)
{
    if (g.font) DeleteObject(g.font);
    g.font = CreateFontW(-MulDiv(9, (int)g.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g.row_h = S(24);
    g.edit_h = S(26);
    g.header_h = S(24);
    g.tab_h = S(30);
    g.status_h = S(24);
    g.pad = S(6);
    g.split = S(4);
    for (int i = 0; i < 2; i++)
        if (g.panes[i].edit) SendMessageW(g.panes[i].edit, WM_SETFONT, (WPARAM)g.font, TRUE);
    if (g.mono) DeleteObject(g.mono);
    g.mono = CreateFontW(-MulDiv(9, (int)g.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
}

/* Shared scroll bar handling for panes and the tree. */
static void scroll_msg(HWND h, WPARAM w, int *top, int page, int total)
{
    SCROLLINFO si = { sizeof si, SIF_ALL };
    GetScrollInfo(h, SB_VERT, &si);
    switch (LOWORD(w)) {
    case SB_LINEUP: (*top)--; break;
    case SB_LINEDOWN: (*top)++; break;
    case SB_PAGEUP: *top -= page; break;
    case SB_PAGEDOWN: *top += page; break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: *top = si.nTrackPos; break;
    case SB_TOP: *top = 0; break;
    case SB_BOTTOM: *top = total; break;
    default: break;
    }
}

static int wheel_rows(int *acc, WPARAM w)
{
    *acc += GET_WHEEL_DELTA_WPARAM(w);
    int rows = *acc / WHEEL_DELTA * 3;
    *acc %= WHEEL_DELTA;
    return rows;
}

static void note_first_frame(void)
{
    if (g.startup_ms != 0 || !IsWindowVisible(g.hwnd)) return;
    g.startup_ms = since_start_ms();
    startup_mark("first frame");
    InvalidateRect(g.hwnd, NULL, FALSE);
    PostMessageW(g.hwnd, WM_APP_LATE, 0, 0);

    /* FPDEMO_STARTUP_LOG=<file> appends the measurement, for benchmarking. */
    wchar_t log[MAX_PATH];
    if (GetEnvironmentVariableW(L"FPDEMO_STARTUP_LOG", log, MAX_PATH)) {
        HANDLE f = CreateFileW(log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            FILETIME created = { 0 }, exited = { 0 }, kernel = { 0 }, user = { 0 };
            if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
                CloseHandle(f);
                return;
            }
            ULARGE_INTEGER stamp = { .LowPart = created.dwLowDateTime, .HighPart = created.dwHighDateTime };
            char line[96];
            int n = snprintf(line, sizeof line, "%lu,%llu,%.1f\r\n", GetCurrentProcessId(),
                             (unsigned long long)stamp.QuadPart, g.startup_ms);
            DWORD wr;
            WriteFile(f, line, (DWORD)n, &wr, NULL);
            CloseHandle(f);
        }
    }
}

/* ---- window procedures ----------------------------------------------------- */

static LRESULT CALLBACK edit_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    Pane *p = (Pane *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_SETFOCUS) {p->address_editing=1;set_active(p->idx);}
    if (m == WM_KEYDOWN && w == VK_RETURN) {
        int len = GetWindowTextLengthW(h);
        wchar_t *buf = malloc((size_t)(len + 1) * sizeof(wchar_t));
        if (buf) {
            GetWindowTextW(h, buf, len + 1);
            focus(p->hwnd);
            p_end_address(p);
            p_navigate_typed(p, buf);
            free(buf);
        }
        return 0;
    }
    if (m == WM_KEYDOWN && (w == VK_ESCAPE || w == VK_TAB)) {
        p->address_editing=0;p_address_text(p,0);
        focus(p->hwnd);
        p_end_address(p);
        return 0;
    }
    /* clicking elsewhere in the window ends typing; switching apps doesn't */
    if (m == WM_KILLFOCUS && w && IsChild(g.hwnd, (HWND)w)) p_end_address(p);
    if (m == WM_CHAR && (w == L'\r' || w == 27 || w == L'\t')) return 0;
    return CallWindowProcW(g.edit_proc, h, m, w, l);
}

/* Core input (cursor, selection, filter) plus whatever function is bound. */
static void p_keydown(Pane *p, WPARAM vk)
{
    int ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    int64_t cur = p->cursor;
    /* while the filter has text, Backspace edits it rather than going up */
    if (vk == VK_BACK && p->filter_len) {
        p->filter[--p->filter_len] = 0;
        p_filter_changed(p);
        return;
    }
    if (pane_key(p, vk, 0, 0)) return;
    switch (vk) {
    case VK_UP: p_move_cursor(p, cur - p_columns_count(p), shift, ctrl); break;
    case VK_DOWN: p_move_cursor(p, cur + p_columns_count(p), shift, ctrl); break;
    case VK_LEFT: if(p->thumbnails)p_move_cursor(p,cur-1,shift,ctrl); break;
    case VK_RIGHT: if(p->thumbnails)p_move_cursor(p,cur+1,shift,ctrl); break;
    case VK_PRIOR: p_move_cursor(p, cur - p_page(p) + p_columns_count(p), shift, ctrl); break;
    case VK_NEXT: p_move_cursor(p, cur + p_page(p) - p_columns_count(p), shift, ctrl); break;
    case VK_HOME: p_move_cursor(p, 0, shift, ctrl); break;
    case VK_END: p_move_cursor(p, (int64_t)p_count(p) - 1, shift, ctrl); break;
    case VK_SPACE:
        if (ctrl && p_count(p)) { p_set_sel(p, p->cursor, !p_is_sel(p, p->cursor)); p_redraw(p); }
        break;
    case VK_ESCAPE:
        if(p->address_resolving) {p_address_cancel(p);p->loading=0;KillTimer(p->hwnd,TIMER_LOADING);set_status(p,0,L"Linux path resolution cancelled");p_address_text(p,0);p_redraw(p);break;}
        if (p->filter_len) { p->filter_len = 0; p->filter[0] = 0; p_filter_changed(p); }
        else { p_clear_sel(p); p_redraw(p); }
        break;
    default: break;
    }
}

static LRESULT CALLBACK pane_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    Pane *p = (Pane *)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_NCCREATE:
        p = (Pane *)((CREATESTRUCTW *)l)->lpCreateParams;
        p->hwnd = h;
        p->published_caption[0]=0;
        p->published_sort=p->published_descending=p->published_size=-1;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)p);
        break;

    case WM_CREATE:
        p->distro_label=CreateWindowExW(0,L"STATIC",L"WSL: ",WS_CHILD|SS_LEFT|SS_CENTERIMAGE|SS_ENDELLIPSIS,0,0,0,0,h,NULL,GetModuleHandleW(NULL),NULL);
        SendMessageW(p->distro_label,WM_SETFONT,(WPARAM)g.font,FALSE);
        p->edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | (g.crumbs ? 0 : WS_VISIBLE) | ES_AUTOHSCROLL, 0, 0, 0, 0, h, NULL,
                                  GetModuleHandleW(NULL), NULL);
        SetWindowLongPtrW(p->edit, GWLP_USERDATA, (LONG_PTR)p);
        g.edit_proc = (WNDPROC)SetWindowLongPtrW(p->edit, GWLP_WNDPROC, (LONG_PTR)edit_proc);
        SendMessageW(p->edit, WM_SETFONT, (WPARAM)g.font, FALSE);
        SetWindowTheme(h, L"DarkMode_Explorer", NULL);
        return 0;

    case PM_ADDRESS_DONE: {
        pathentry_result *result=(pathentry_result*)l;
        uint64_t sequence=result?result->sequence:(uint64_t)w;
        if(p->address_resolving && sequence==p->address_sequence) {
            p->address_resolving=p->loading=0;SetPropW(h,L"Address.Resolving",NULL);KillTimer(h,TIMER_LOADING);
            DWORD error=result?result->error:ERROR_NOT_ENOUGH_MEMORY;
            if(!fn_active(fn_find("wsl.helper")))error=ERROR_NOT_SUPPORTED;
            if(!error) p_navigate(p,result->path,NULL,1);
            else {wchar_t reason[256];win_err(reason,256,error);set_status(p,1,L"Cannot resolve Linux path: %ls",error==ERROR_BAD_ENVIRONMENT?L"WSL or the selected distro could not start":reason);
                SetPropW(h,L"Address.Error",(HANDLE)(uintptr_t)error);p_address_text(p,0);p_redraw(p);}
        }
        pathentry_result_free(result);return 0;
    }
    case WM_DESTROY: {
        p_address_cancel(p);pathentry_stop(p->address_worker);p->address_worker=NULL;
        MSG pending;
        while(PeekMessageW(&pending,h,PM_ADDRESS_DONE,PM_ADDRESS_DONE,PM_REMOVE)) {
            /* PeekMessage returns WM_QUIT even when a message filter is set. */
            if(pending.message==WM_QUIT){PostQuitMessage((int)pending.wParam);break;}
            pathentry_result_free((pathentry_result*)pending.lParam);
        }
        return 0;
    }
    case WM_SIZE: {
        int keep_cursor = p->thumbnails && (int)p->cursor >= p->top && (int)p->cursor < p->top + p_page(p);
        p->cw = LOWORD(l);
        p->ch = HIWORD(l);
        p_clamp(p);
        if (keep_cursor) p_ensure_visible(p, p->cursor);
        if(g_dwp){
            if(!p->geometry_pending){p->geometry_pending=1;if(!PostMessageW(h,PM_GEOMETRY,0,0))p_finish_geometry(p);}
        }else{p_place_edit(p);p_redraw(p);}
        return 0;
    }
    case PM_GEOMETRY:p_finish_geometry(p);return 0;

    case WM_APP_RELEASE_BUFFER:bbuf_discard(&p->bb);return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        int64_t phase=fpe_prof_begin();p_finish_geometry(p);FPE_PROF_END("paint.pane.geometry",phase);
        PAINTSTRUCT ps;
        phase=fpe_prof_begin();HDC dc=BeginPaint(h,&ps);FPE_PROF_END("paint.pane.begin",phase);
        int64_t t = fpe_prof_begin();
        phase=fpe_prof_begin();int fresh=bbuf_ensure(&p->bb,h,p->cw,p->ch);FPE_PROF_END("paint.pane.buffer",phase);
        if(fresh<0){fill(dc,ps.rcPaint.left,ps.rcPaint.top,ps.rcPaint.right-ps.rcPaint.left,ps.rcPaint.bottom-ps.rcPaint.top,C_BG);EndPaint(h,&ps);FPE_PROF_COUNT("paint.buffer.failed");return 0;}
        int saved=SaveDC(p->bb.dc);
        if(saved)IntersectClipRect(p->bb.dc,ps.rcPaint.left,ps.rcPaint.top,ps.rcPaint.right,ps.rcPaint.bottom);
        phase=fpe_prof_begin();p_paint(p,p->bb.dc);FPE_PROF_END("paint.pane.render",phase);
        if(saved)RestoreDC(p->bb.dc,saved);
        phase=fpe_prof_begin();BitBlt(dc,ps.rcPaint.left,ps.rcPaint.top,ps.rcPaint.right-ps.rcPaint.left,ps.rcPaint.bottom-ps.rcPaint.top,p->bb.dc,ps.rcPaint.left,ps.rcPaint.top,SRCCOPY);FPE_PROF_END("paint.pane.blit",phase);
        phase=fpe_prof_begin();EndPaint(h,&ps);FPE_PROF_END("paint.pane.end",phase);
        FPE_PROF_END("pane.paint", t);
        bbuf_presented(&p->bb,h);
        note_first_frame();
        return 0;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)w, p->idx == g.active ? C_TEXT : C_DIM);
        SetBkColor((HDC)w, C_EDIT);
        return (LRESULT)g.edit_brush;

    case WM_SETFOCUS:
        set_active(p->idx);
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_KILLFOCUS:
        if(g_tab_drag.source==p)tab_drag_cancel();
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_KEYDOWN: {
        if(g_tab_drag.source){if(w==VK_ESCAPE)tab_drag_cancel();return 0;}
        int64_t t = fpe_prof_begin();
        p_keydown(p, w);
        FPE_PROF_END("pane.key", t);
        return 0;
    }

    case WM_SYSKEYDOWN:
        if(g_tab_drag.source){tab_drag_cancel();return 0;}
        if (pane_key(p, w, 1, l)) return 0;
        break; /* Alt+F4, F10 */

    case WM_CHAR: {
        uint8_t vk = ctrl_char_key(w);
        if (vk) {
            fn_ctx c;
            pane_ctx(&c, p, FN_FROM_KEY, 1);
            fn_key(vk, FN_CTRL, &c);
            ctx_free(&c);
            return 0;
        }
        if (w >= 32 && w != 127 && GetKeyState(VK_CONTROL) >= 0 && p->filter_len < 255) {
            p->filter[p->filter_len++] = (wchar_t)w;
            p->filter[p->filter_len] = 0;
            p_filter_changed(p);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        rename_end(p, 1);
        int rows = wheel_rows(&p->wheel, w);
        if (rows) {
            p->top -= rows * p_columns_count(p);
            p_clamp(p);
            p_redraw(p);
        }
        return 0;
    }

    case WM_VSCROLL:
        rename_end(p, 1);
        {int top=p->top/p_columns_count(p);
        scroll_msg(h,w,&top,p_page(p)/p_columns_count(p),p_scroll_count(p));p->top=top*p_columns_count(p);}
        p_clamp(p);
        p_redraw(p);
        return 0;

    case PM_RENAME_DONE:
        if (p->rename_edit == (HWND)l) rename_end(p, 1);
        return 0;

    case WM_RBUTTONDOWN:
        p->down_armed=p->down_deferred=0;
        tab_drag_cancel();focus(h);
        p_right_click(p, GET_X_LPARAM(l), GET_Y_LPARAM(l));
        p->down_row=p_row_at(p,GET_X_LPARAM(l),GET_Y_LPARAM(l));
        p->down_x=GET_X_LPARAM(l);p->down_y=GET_Y_LPARAM(l);
        p->down_button=MK_RBUTTON;p->down_armed=p->down_row>=0;p->right_suppress=0;
        p->down_revision=fpe_view_revision(p->view);
        return 0;

    case WM_RBUTTONUP:
        p->down_armed=p->down_deferred=0;
        if(p->right_suppress)return 0;
        break;

    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
        if(g_tab_drag.source==p)tab_drag_cancel();
        p->down_armed=p->down_deferred=0;
        return 0;

    case WM_SETCURSOR:
        if(g_tab_drag.started){SetCursor(LoadCursorW(NULL,(LPCWSTR)(g_tab_drag.target?IDC_SIZEALL:IDC_NO)));return TRUE;}
        break;

    case WM_CONTEXTMENU:
        if(p->right_suppress && l!=-1){p->right_suppress=0;return 0;}
        p_context_key_or_mouse(p, l);
        return 0;

    case WM_MOUSEMOVE:
        if(g_tab_drag.source==p){
            if(!(w&MK_LBUTTON)){tab_drag_cancel();return 0;}
            int dx=abs(GET_X_LPARAM(l)-g_tab_drag.x),dy=abs(GET_Y_LPARAM(l)-g_tab_drag.y);
            if(g_tab_drag.started || dx>GetSystemMetrics(SM_CXDRAG) || dy>GetSystemMetrics(SM_CYDRAG)){
                g_tab_drag.started=1;POINT pt={GET_X_LPARAM(l),GET_Y_LPARAM(l)};ClientToScreen(h,&pt);tab_drag_over(pt);
            }
            return 0;
        }
        if (p->down_armed && p->down_revision != fpe_view_revision(p->view))
            p->down_armed = p->down_deferred = 0;
        if (p->down_armed) {
            int dx = abs(GET_X_LPARAM(l) - p->down_x), dy = abs(GET_Y_LPARAM(l) - p->down_y);
            if (!(w & p->down_button)) {
                p->down_armed = p->down_deferred = 0;
            } else if (dx > GetSystemMetrics(SM_CXDRAG) || dy > GetSystemMetrics(SM_CYDRAG)) {
                p->down_armed = p->down_deferred = 0;
                g_file_drag_right=p->down_button==MK_RBUTTON;
                p->right_suppress=g_file_drag_right;
                pane_run(p, "item.drag_out", FN_FROM_MOUSE);
                g_file_drag_right=0;
            }
        }
        return 0;

    case WM_LBUTTONUP:
        if(g_tab_drag.source==p){POINT pt={GET_X_LPARAM(l),GET_Y_LPARAM(l)};ClientToScreen(h,&pt);tab_drag_finish(pt);return 0;}
        if (p->down_armed && p->down_deferred && p->down_revision == fpe_view_revision(p->view) &&
            p->down_row >= 0 && p->down_row < (int)p_count(p)) {
            /* a plain click inside a multi-selection, not a drag: select just that row */
            p_clear_sel(p);
            p_set_sel(p, (uint32_t)p->down_row, 1);
            p->anchor = (uint32_t)p->down_row;
            p_redraw(p);
        }
        p->down_armed = p->down_deferred = 0;
        return 0;

    case WM_LBUTTONDOWN: {
        p->down_armed = p->down_deferred = 0;
        focus(h);
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        int hy = p_header_y(p), ay = p_addr_y(p);
        if (y < ay && p->ntabs > 1) {
            int close, i = tab_at(p, x, &close);
            if (i >= 0 && close) tab_close(p, i);
            else if (i >= 0) tab_drag_begin(p, i, x, y);
            return 0;
        }
        if (g.crumbs && y >= ay && y < ay + g.edit_h) {
            wchar_t child[260];
            wchar_t *to = crumb_path(p, crumb_at(p, x), child, 260);
            if (to) p_navigate(p, to, child, 1);
            else p_focus_address(p); /* "...", the current folder, or empty space: type a path */
            free(to);
            return 0;
        }
        if (y >= hy && y < hy + g.header_h) {
            cols c = p_columns(p);
            if (c.date_w && x >= c.date_x) p_set_sort(p, FPE_SORT_MTIME);
            else if (x >= c.size_x) p_set_sort(p, FPE_SORT_SIZE);
            else if (c.type_w && x >= c.type_x) p_set_sort(p, FPE_SORT_EXT);
            else if (c.perm_w && x >= c.perm_x) return 0; /* Permissions and Owner don't sort */
            else p_set_sort(p, FPE_SORT_NAME);
            return 0;
        }
        int r = p_row_at(p, x, y);
        if (r < 0) {
            if (y >= p_list_top(p) && y < p_list_bottom(p)) { p_clear_sel(p); p_redraw(p); }
            return 0;
        }
        if (w & MK_CONTROL) {
            p_set_sel(p, (uint32_t)r, !p_is_sel(p, (uint32_t)r));
            p->cursor = p->anchor = (uint32_t)r;
        } else if (w & MK_SHIFT) {
            p->cursor = (uint32_t)r;
            p_select_range(p, p->anchor, p->cursor);
        } else if (p_is_sel(p, (uint32_t)r) && p->nsel > 1) {
            /* keep the selection: this may be the start of dragging it */
            p->cursor = (uint32_t)r;
            p->down_deferred = 1;
        } else {
            p_clear_sel(p);
            p_set_sel(p, (uint32_t)r, 1);
            p->cursor = p->anchor = (uint32_t)r;
        }
        p->down_button=MK_LBUTTON;
        p->down_row = r;
        p->down_x = x;
        p->down_y = y;
        p->down_revision = fpe_view_revision(p->view);
        p->down_armed = p_is_sel(p, (uint32_t)r);
        p_redraw(p);
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l), close;
        if (y < p_addr_y(p) && p->ntabs > 1) { /* empty strip space: a new tab */
            if (tab_at(p, x, &close) < 0) pane_run(p, "tab.new", FN_FROM_MOUSE);
            return 0;
        }
        int r = p_row_at(p, x, y);
        if (r >= 0) p_open_row(p, (uint32_t)r);
        return 0;
    }

    /* Middle click: close a tab; open a crumb or a folder in a new tab, behind this one. */
    case WM_MBUTTONDOWN: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l), close;
        if (y < p_addr_y(p) && p->ntabs > 1) {
            int i = tab_at(p, x, &close);
            if (i >= 0 && fn_active(fn_find("tab.close"))) tab_close(p, i);
            return 0;
        }
        wchar_t *path = NULL;
        if (g.crumbs && y >= p_addr_y(p) && y < p_addr_y(p) + g.edit_h) {
            path = crumb_path(p, crumb_at(p, x), NULL, 0);
        } else {
            int r = p_row_at(p, x, y);
            const fpe_entry *e = r >= 0 ? fpe_view_at(p->view, (uint32_t)r) : NULL;
            if (e && fpe_entry_is_dir(e)) path = join(p->path, fpe_view_name(p->view, (uint32_t)r));
        }
        if (path) path_run(p, "tab.open", path, FN_FROM_MOUSE);
        return 0;
    }

    case WM_XBUTTONUP:
        focus(h);
        pane_run(p, GET_XBUTTON_WPARAM(w) == XBUTTON1 ? "nav.back" : "nav.forward", FN_FROM_MOUSE);
        return TRUE;

    case WM_TIMER:
        KillTimer(h, w);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static LRESULT CALLBACK tree_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE:
        T.hwnd = h;
        SetWindowTheme(h, L"DarkMode_Explorer", NULL);
        return 0;

    case WM_SIZE:
        T.cw = LOWORD(l);
        T.ch = HIWORD(l);
        tree_clamp();
        tree_redraw();
        return 0;

    case WM_APP_RELEASE_BUFFER:bbuf_discard(&T.bb);return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        int64_t t = fpe_prof_begin();
        bbuf_ensure(&T.bb, h, T.cw, T.ch);
        tree_paint(T.bb.dc);
        BitBlt(dc, 0, 0, T.cw, T.ch, T.bb.dc, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        FPE_PROF_END("tree.paint", t);
        bbuf_presented(&T.bb,h);
        return 0;
    }

    case WM_SETFOCUS:
        if (!T.cursor) T.cursor = T.current;
        /* fallthrough */
    case WM_KILLFOCUS:
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_KEYDOWN: {
        if (tree_key(w, 0, 0)) return 0;
        tnode *c = T.cursor;
        switch (w) {
        case VK_UP: tree_move_cursor(-1); break;
        case VK_DOWN: tree_move_cursor(1); break;
        case VK_PRIOR: tree_move_cursor(-tree_page() + 1); break;
        case VK_NEXT: tree_move_cursor(tree_page() - 1); break;
        case VK_HOME: tree_move_cursor(-(int)T.nrows); break;
        case VK_END: tree_move_cursor((int)T.nrows); break;
        case VK_RIGHT:
            if (c && !c->expanded) tree_toggle(c);
            else if (c && c->nkids) tree_move_cursor(1);
            break;
        case VK_LEFT:
            if (c && c->expanded) tree_toggle(c);
            else if (c && c->parent) {
                T.cursor = c->parent;
                tree_ensure_visible(T.cursor);
                tree_redraw();
                tree_open(T.cursor);
            }
            break;
        case VK_RETURN:
        case VK_SPACE:
            if(c&&c->kind==NODE_LINUX)tree_toggle(c);else tree_open(c);
            break;
        default: break;
        }
        return 0;
    }

    case WM_SYSKEYDOWN:
        if (tree_key(w, 1, l)) return 0;
        break;

    case WM_CHAR: {
        uint8_t vk = ctrl_char_key(w);
        if (vk) {
            fn_ctx c;
            tree_ctx(&c, T.cursor ? T.cursor : T.current, FN_FROM_KEY);
            fn_key(vk, FN_CTRL, &c);
            ctx_free(&c);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int rows = wheel_rows(&T.wheel, w);
        if (rows) {
            T.top -= rows;
            tree_clamp();
            tree_redraw();
        }
        return 0;
    }

    case WM_VSCROLL:
        scroll_msg(h, w, &T.top, tree_page(), (int)T.nrows);
        tree_clamp();
        tree_redraw();
        return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        focus(h);
        int y = GET_Y_LPARAM(l) - g.pad, x = GET_X_LPARAM(l);
        if (y < 0) return 0;
        int r = T.top + y / g.row_h;
        if (r >= (int)T.nrows) return 0;
        tnode *n = T.rows[r];
        int ax = g.pad + n->depth * S(14);
        T.cursor = n;
        if (n->kind == NODE_LINUX || m == WM_LBUTTONDBLCLK || (x >= ax && x < ax + S(16))) {
            tree_toggle(n);
        } else {
            T.down = n;T.down_button=MK_LBUTTON;
            T.down_x = x;
            T.down_y = GET_Y_LPARAM(l);
            tree_redraw();
            tree_open(n);
        }
        return 0;
    }

    case WM_MOUSEMOVE:
        if (T.down) {
            int dx = abs(GET_X_LPARAM(l) - T.down_x), dy = abs(GET_Y_LPARAM(l) - T.down_y);
            if (!(w & T.down_button)) {
                T.down = NULL;
            } else if (dx > GetSystemMetrics(SM_CXDRAG) || dy > GetSystemMetrics(SM_CYDRAG)) {
                fn_ctx c;
                tree_ctx(&c, T.down, FN_FROM_MOUSE);
                T.down = NULL;g_file_drag_right=T.down_button==MK_RBUTTON;T.right_suppress=g_file_drag_right;
                fn_run_id("item.drag_out", &c);g_file_drag_right=0;
                ctx_free(&c);
            }
        }
        return 0;

    case WM_LBUTTONUP:
        T.down = NULL;
        return 0;
    case WM_RBUTTONUP:
        T.down=NULL;if(T.right_suppress)return 0;break;

    case WM_MBUTTONDOWN: {
        tnode *n = tree_node_at(GET_Y_LPARAM(l));
        if (n) {
            fn_ctx c;
            tree_ctx(&c, n, FN_FROM_MOUSE);
            fn_run_id("tab.open", &c);
            ctx_free(&c);
        }
        return 0;
    }

    case WM_RBUTTONDOWN: {
        focus(h);
        tnode *n = tree_node_at(GET_Y_LPARAM(l));
        T.down=n;T.down_button=MK_RBUTTON;T.down_x=GET_X_LPARAM(l);T.down_y=GET_Y_LPARAM(l);T.right_suppress=0;
        if (n) {
            T.cursor = n;
            tree_redraw();
            UpdateWindow(h);
        }
        return 0;
    }

    case WM_CONTEXTMENU: {
        if(T.right_suppress && l!=-1){T.right_suppress=0;return 0;}
        POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        tnode *n;
        if (l == -1) {
            n = T.cursor ? T.cursor : T.current;
            int r = n ? tree_row_of(n) - T.top : 0;
            if (r < 0 || r >= tree_page()) r = 0;
            pt.x = g.pad + (n ? n->depth : 0) * S(14) + S(40);
            pt.y = g.pad + r * g.row_h + g.row_h;
            ClientToScreen(h, &pt);
        } else {
            POINT cl = pt;
            ScreenToClient(h, &cl);
            n = tree_node_at(cl.y);
        }
        tree_context_menu(n, pt);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

/* After the first frame, start one deferred service per message (dependencies
 * first). OLE,
 * the icon worker, drop targets) cost several milliseconds that startup
 * should not pay. Until then rows show plain glyph icons. */
static uint32_t late_cursor;
static int late_marked;
static void late_init(void)
{
    if(g.closing||g.destroyed)return;
    int64_t t=fpe_prof_begin();int more=fn_start_hot_step(1,&late_cursor);
    FPE_PROF_END("app.late_init",t);
    if(more==FN_PENDING)return;
    if(more){PostMessageW(g.hwnd,WM_APP_LATE,0,0);return;}
    if(fn_running(fn_find("svc.drop")))dropwarm_stop(); /* real targets retain the warmed OLE state */
    if(!late_marked){
        late_marked=1;startup_mark("shell ready");
        RedrawWindow(g.hwnd,NULL,NULL,RDW_INVALIDATE|RDW_ALLCHILDREN);
    }
    settings_status(-1);
}

static void drop_warm_progress(void)
{
    HRESULT error=S_OK;int result=dropwarm_collect(&error);
    const fn_def *owner=fn_find("svc.drop.warmup");
    if(result==DROPWARM_READY)fn_start_complete(owner,0);
    else if(result==DROPWARM_DONE){
        fn_stop_complete(owner);
        if(FAILED(error)){FPE_PROF_COUNT("drop.warm.cleanup_failed");if(!g.closing&&!g.destroyed)set_status(active_pane(),1,L"Drop preparation cleanup failed (0x%08lx)",(unsigned long)error);}
    }
    else if(result==DROPWARM_FAILED){
        if(!fn_start_complete(owner,-1))fn_fail(owner);
    }else return;
    if(g.destroyed){if(fn_shutdown())PostQuitMessage(0);return;}
    if(g.closing){if(!dropwarm_wait_handle())PostMessageW(g.hwnd,WM_CLOSE,0,0);return;}
    late_cursor=0;PostMessageW(g.hwnd,WM_APP_LATE,0,0);
}

static LRESULT CALLBACK main_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    LRESULT menu_result;
    if (shell_menu_msg(h, m, w, l, &menu_result)) return menu_result;
    switch (m) {
    case WM_CREATE: {
        g.hwnd = h;
        g.dpi = GetDpiForWindow(h);
        fw_brand_window(h,g.dpi);
        update_metrics();
        g.edit_brush = CreateSolidBrush(C_EDIT);
        g.tree_w = S(g_ses.tree_w);
        HINSTANCE inst = GetModuleHandleW(NULL);
        CreateWindowExW(0, L"OpenPilotTree", L"", WS_CHILD | WS_VSCROLL | WS_CLIPSIBLINGS, 0, 0, 0, 0, h, NULL, inst, NULL);
        for (int i = 0; i < 2; i++)
            CreateWindowExW(0, L"OpenPilotPane", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 0, 0,
                            h, NULL, inst, &g.panes[i]);
        g.dbg = CreateWindowExW(0, L"OpenPilotDebug", L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, h, NULL, inst, NULL);
        return 0;
    }

    case WM_SIZE:
        if (w != SIZE_MINIMIZED) layout_now(); /* an edge dragged: each step shows complete */
        return 0;

    case WM_DPICHANGED: {
        UINT old = g.dpi;
        g.dpi = HIWORD(w);
        fw_brand_window(h,g.dpi);
        g.tree_w = MulDiv(g.tree_w, (int)g.dpi, (int)old);
        palette_close();
        update_metrics();
        const RECT *r = (const RECT *)l;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        return 0;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)l;
        mm->ptMinTrackSize.x = S(560);
        mm->ptMinTrackSize.y = S(260);
        return 0;
    }

    case WM_APP_RELEASE_BUFFER:bbuf_discard(&g.bb);return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        int64_t phase=fpe_prof_begin();HDC dc=BeginPaint(h,&ps);FPE_PROF_END("paint.main.begin",phase);
        phase=fpe_prof_begin();int fresh=bbuf_ensure(&g.bb,h,g.cw,g.ch);FPE_PROF_END("paint.main.buffer",phase);
        if(fresh<0){fill(dc,ps.rcPaint.left,ps.rcPaint.top,ps.rcPaint.right-ps.rcPaint.left,ps.rcPaint.bottom-ps.rcPaint.top,C_BG);EndPaint(h,&ps);FPE_PROF_COUNT("paint.buffer.failed");return 0;}
        int saved=SaveDC(g.bb.dc);
        if(saved)IntersectClipRect(g.bb.dc,ps.rcPaint.left,ps.rcPaint.top,ps.rcPaint.right,ps.rcPaint.bottom);
        phase=fpe_prof_begin();main_paint(g.bb.dc);FPE_PROF_END("paint.main.render",phase);
        if(saved)RestoreDC(g.bb.dc,saved);
        phase=fpe_prof_begin();BitBlt(dc,ps.rcPaint.left,ps.rcPaint.top,ps.rcPaint.right-ps.rcPaint.left,ps.rcPaint.bottom-ps.rcPaint.top,g.bb.dc,ps.rcPaint.left,ps.rcPaint.top,SRCCOPY);FPE_PROF_END("paint.main.blit",phase);
        phase=fpe_prof_begin();EndPaint(h,&ps);FPE_PROF_END("paint.main.end",phase);
        bbuf_presented(&g.bb,h);
        return 0;
    }

    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) {
            HWND f = GetFocus();
            if (f && IsChild(h, f)) g.focus_save = f;
        }
        break;

    case WM_SETFOCUS: {
        /* back from another app: return to the control that had focus
         * (an open rename box first) */
        Pane *p = active_pane();
        HWND to = p->hwnd;
        if (p->rename_edit) to = p->rename_edit;
        else if (g.focus_save && IsWindow(g.focus_save) && IsChild(h, g.focus_save) && IsWindowVisible(g.focus_save))
            to = g.focus_save;
        g.focus_save = NULL;
        SetFocus(to);
        return 0;
    }

    case WM_ENDSESSION: /* logging off: WM_DESTROY may never come */
        if (w) { session_save(); folder_views_save(); }
        return 0;

    case WM_ENTERSIZEMOVE:
    case WM_ENTERMENULOOP:
        g.modal_seq++;
        break;

    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            if (g.drag || hit_splitter(pt.x, pt.y)) {
                SetCursor(LoadCursorW(NULL, IDC_SIZEWE));
                return TRUE;
            }
        }
        break;

    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        if (PtInRect(&g_operations_button, pt) && g_operations.n) {
            pane_run(active_pane(), "operations.show", FN_FROM_MOUSE);
            return 0;
        }
        if (PtInRect(&g_gear, pt)) {
            pane_run(active_pane(), "settings.show", FN_FROM_MOUSE);
            return 0;
        }
        g.drag = hit_splitter(pt.x, pt.y);
        if (g.drag) SetCapture(h);
        return 0;
    }

    case WM_MOUSEMOVE:
        if (g.drag) l = latest_move(h, l);
        if(g.drag==3){if(g.ar_width>g.split){g.ar_ratio=(double)(GET_X_LPARAM(l)-g.ar_left)/(g.ar_width-g.split);if(g.ar_ratio<0.15)g.ar_ratio=0.15;if(g.ar_ratio>0.85)g.ar_ratio=0.85;layout_now();}}else if (g.drag == 1) {
            g.tree_w = GET_X_LPARAM(l)-g.body_x;
            layout_now();
        } else if (g.drag == 2) {
            int x0 = g.body_x+(g.show_tree ? g.tree_w + g.split : 0);
            int avail = g.cw - x0 - sidepanel_width(g.cw - x0) - g.split;
            if (avail > 0) {
                g.ratio = (double)(GET_X_LPARAM(l) - x0) / avail;
                if (g.ratio < 0.15) g.ratio = 0.15;
                if (g.ratio > 0.85) g.ratio = 0.85;
                layout_now();
            }
        }
        return 0;

    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (g.drag) {
            g.drag = 0;
            if (m == WM_LBUTTONUP) ReleaseCapture();
        }
        return 0;

    case WM_TIMER:
        if (w == TIMER_SETTINGS_SAVE) {
            if (!g_settings_save) KillTimer(h, w);
            else {
                MSG completed = { .hwnd = h, .message = WM_APP_SETTINGS_SAVED };
                dispatch_timed(&completed); /* completion keeps its own timing in nested loops too */
            }
            return 0;
        }
        if (w == TIMER_CLIPBOARD) { clipboard_publish(); return 0; }
        if (w == TIMER_THUMB) {KillTimer(h,w);thumbnails_collect();return 0;}
        if (w == TIMER_FN) {
            fn_tick();
            return 0;
        }
        if (w == TIMER_PREVIEW) {
            KillTimer(h, w);
            preview_show();
            return 0;
        }
        if (w == TIMER_LINGER) {
            KillTimer(h, w);
            preview_hide();
            return 0;
        }
        if (w == TIMER_SETTINGS) {
            KillTimer(h, w);
            settings_check();
            return 0;
        }
        KillTimer(h, w);
        if(w==TIMER_JOBTEXT){g.job_text[0]=0;main_status_redraw();}
        else InvalidateRect(h,NULL,FALSE);
        return 0;

    case WM_APP_IMAGE_LIST:{
        imagebrowse_list *list=imagebrowse_take(g.image_worker);if(!list)return 0;
        if(g.image_worker&&list->sequence==g.image_sequence&&g.image_source&&image_browse_context(g.image_source)&&
           g.image_source->view==g.image_view&&fpe_view_revision(g.image_view)==list->revision){
            imagebrowse_free(g.image_list);g.image_list=list;g.image_list_state=0;image_browse_note(g.image_source);imageview_report_result();
        }else imagebrowse_free(list);return 0;
    }
    case WM_APP_LOADED: {
        int64_t t = fpe_prof_begin();
        load_msg *msg = (load_msg *)l;
        if (!p_handle_loaded(&g.panes[0], msg) && !p_handle_loaded(&g.panes[1], msg)) tree_handle_loaded(msg);
        fpe_dir_release(msg->dir);
        free(msg);
        InvalidateRect(h, NULL, FALSE);
        FPE_PROF_END("app.load_done", t);
        return 0;
    }


    case WM_APP_SETTINGS_SAVED:
        settings_save_complete();
        return 0;
    case WM_APP_JOB: {
        int64_t t = fpe_prof_begin();
        handle_job((job_msg *)l);
        FPE_PROF_END("app.job_progress", t);
        return 0;
    }

    case WM_APP_LATE:
        late_init();
        return 0;

    case WM_APP_ICONS:
        for (int i = 0; i < 2; i++) InvalidateRect(g.panes[i].hwnd, NULL, FALSE);
        InvalidateRect(T.hwnd, NULL, FALSE);
        palette_redraw();
        return 0;

    case WM_APP_FSOP:
        fsop_done((fsop *)l);
        return 0;

    case WM_APP_THUMB:
        for(int i=0;i<2;++i)if(g.panes[i].thumbnails)InvalidateRect(g.panes[i].hwnd,NULL,FALSE);
        return 0;

    case WM_APP_RELAY:
        relay_message(w, l);
        return 0;

    case WM_CLOSE:
        g.closing=1;
        dropwarm_stop(); /* retain runtime owners until the normal teardown snapshot */
        if(dropwarm_wait_handle())return 0; /* completion posts Close again */
        break;

    case WM_DESTROY:
        g.closing=g.destroyed=1;
        app_clipboard_cancel(NULL);
        folderwatch_shutdown();
        tab_drag_cancel();
        if (g.profile_out) write_report(g.profile_out);
        session_save();
        folder_views_save();
        fv_free(&g_folder_views);
        palette_close();
        preview_hide();
        free(g.svg_source);g.svg_source=NULL;
        dock_end();actiondock_destroy();
        sidepanel_shutdown();
        free(g_space_root);g_space_root=NULL;
        git_model_free(&g_git_model);free(g_git_folder);g_git_folder=NULL;
        for(uint32_t i=0;i<DUPLICATE_ROOTS;i++){free(g_duplicate_roots[i]);g_duplicate_roots[i]=NULL;}memset(&g_duplicate_request,0,sizeof g_duplicate_request);
        for(int i=0;i<2;i++){free(g_diff_paths[i]);g_diff_paths[i]=NULL;}
        content_preview_forget();free(g_content_root);g_content_root=NULL;
        while (g_op_requests) {
            operation_request *r = g_op_requests;
            g_op_requests = r->next; operation_request_free(r);
        }
        ops_free(&g_operations);
        if(g.thumb_held){fn_release(fn_find("svc.thumbnails"));g.thumb_held=0;}
        bulkrename_shutdown();
        foldersize_shutdown();
        foldercompare_shutdown();
        workspaces_shutdown();
        permissions_shutdown();
        if(fn_shutdown())PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static LRESULT CALLBACK dbg_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_SIZE:
        g.dbg_cw = LOWORD(l);
        g.dbg_ch = HIWORD(l);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_APP_RELEASE_BUFFER:bbuf_discard(&g.dbg_bb);return 0;

    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        bbuf_ensure(&g.dbg_bb, h, g.dbg_cw, g.dbg_ch);
        dbg_paint(g.dbg_bb.dc);
        BitBlt(dc, 0, 0, g.dbg_cw, g.dbg_ch, g.dbg_bb.dc, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        bbuf_presented(&g.dbg_bb,h);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int rows = wheel_rows(&g.dbg_wheel, w);
        g.dbg_top -= rows;
        if (g.dbg_top > g.dbg_lines - 3) g.dbg_top = g.dbg_lines - 3;
        if (g.dbg_top < 0) g.dbg_top = 0;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

static void register_class(const wchar_t *name, WNDPROC proc, UINT style, HINSTANCE inst)
{
    WNDCLASSEXW wc = { sizeof wc };
    wc.style = style;
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = fw_brand_icon(inst,GetSystemMetrics(SM_CXICON));
    wc.hIconSm = fw_brand_icon(inst,GetSystemMetrics(SM_CXSMICON));
    wc.lpszClassName = name;
    RegisterClassExW(&wc);
}

static fpe_dir *read_start(const wchar_t *path)
{
    fpe_dir *d = NULL;
    if (path && !fpe_dir_read(path, &d)) return d;
    return fpe_dir_read(L"C:\\", &d) ? NULL : d;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    /* Flags (--debug, --profile-out=FILE) are read first so profiling covers
     * everything after this point; other arguments are start folders. */
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *folders[2] = { NULL, NULL }, *settings = NULL, *ses = NULL;
    int nfolders = 0, list = 0;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--debug")) g.debug_cli = 1;
        else if (!wcsncmp(argv[i], L"--profile-out=", 14)) g.profile_out = wdup(argv[i] + 14);
        else if (!wcsncmp(argv[i], L"--settings=", 11)) settings = argv[i] + 11;
        else if (!wcsncmp(argv[i], L"--session=", 10)) ses = argv[i] + 10;
        else if (!wcscmp(argv[i], L"--functions")) list = 1;
        else if (nfolders < 2) folders[nfolders++] = argv[i];
    }

    /* Registering is a table copy and reading settings one small file:
     * nothing starts here (hot functions start below or after the first frame). */
    fn_host host = { host_now, host_arm, host_status };
    fn_init(&host);
    fn_register(app_functions, sizeof app_functions / sizeof app_functions[0]);
    static const fn_def operations_fn = {
        .id = "operations.show", .title = L"Operations", .about = "Transfers, cancellation and retained results with failed paths",
        .life = FN_COLD, .where = (FN_ANYWHERE|FN_ARCHIVE), .keys = "Ctrl+Shift+O", .opts = operations_opts, .run = run_operations_show,
    };
    fn_register(&operations_fn, 1);
    functions_register();
    relay_register();
    palette_register();
    load_settings(settings);
    fn_configure_all();
    if (list) {
        list_functions();
        return 0;
    }
    wchar_t env[8];
    if (GetEnvironmentVariableW(L"OPENPILOT_DEBUG", env, 8) && env[0] && env[0] != L'0') g.debug_cli = 1;
    int open_panel = g.debug_cli; /* --profile-out alone stays headless: the panel would be measured too */
    if (g.profile_out) g.debug_cli = 1;
    if (g.debug_cli) fpe_prof_enable(1);
    startup_mark("wWinMain");
    fw_brand_process();

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g.engine = fpe_engine_create(NULL);
    startup_mark("engine created");
    g.cf_effect = RegisterClipboardFormatW(L"Preferred DropEffect");

    /* The session is one small file; only each pane's shown tab is read below. */
    session_locate(ses);
    session_load();
    folder_views_load();
    int restore = fn_active(fn_find("session.restore")) && g.session[0];
    if (!restore) g_ses.tree_w = 230;
    g.dual = restore ? g_ses.dual : 1;
    g.show_tree = restore ? g_ses.tree : 1;
    g.show_hidden = restore && g_ses.hidden;
    g.ratio = restore ? g_ses.ratio : 0.5;
    g.crumbs = fn_active(fn_find("view.breadcrumbs"));
    int restore_tabs = restore && !nfolders;
    for (int i = 0; i < 2; i++) {
        Pane *p = &g.panes[i];
        p->idx = i;
        p->drop_row = -1;
        p->opts.key = FPE_SORT_NAME;
        p->opts.dirs_first = 1;
        p->opts.show_hidden = p->opts.show_system = (uint8_t)g.show_hidden;
        p->ntabs = 1;
        if (!restore_tabs || !g_ses.ntabs[i]) continue;
        for (int k = 0; k < g_ses.ntabs[i]; k++) {
            p->tabs[k].path = g_ses.tabs[i][k].path; /* moved */
            g_ses.tabs[i][k].path = NULL;
            p->tabs[k].opts = p->opts;
            p->tabs[k].opts.key = (fpe_sort_key)g_ses.tabs[i][k].sort;
            p->tabs[k].opts.descending = g_ses.tabs[i][k].desc;
        }
        p->ntabs = g_ses.ntabs[i];
        p->tab = g_ses.cur[i];
        p->opts = p->tabs[p->tab].opts;
    }

    register_class(L"OpenPilot", main_proc, 0, inst);
    register_class(L"OpenPilotPane", pane_proc, CS_DBLCLKS, inst);
    register_class(L"OpenPilotTree", tree_proc, CS_DBLCLKS, inst);
    register_class(L"OpenPilotDebug", dbg_proc, 0, inst);

    HWND h = CreateWindowExW(0, L"OpenPilot", L"Folderwing", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, inst, NULL);
    if (!h) return 1;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(h, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    /* A saved position is used only if it is still on a monitor. */
    if (restore && g_ses.has_rect && MonitorFromRect(&g_ses.rect, MONITOR_DEFAULTTONULL)) {
        WINDOWPLACEMENT wp = { sizeof wp };
        wp.showCmd = SW_HIDE;
        wp.rcNormalPosition = g_ses.rect;
        SetWindowPlacement(h, &wp);
        if (g_ses.maximized && (show == SW_SHOWNORMAL || show == SW_SHOWDEFAULT)) show = SW_SHOWMAXIMIZED;
    } else {
        SetWindowPos(h, NULL, 0, 0, S(1280), S(760), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    startup_mark("window created");

    for (int i = 0; i < g_ses.npins; i++) tree_pin(g_ses.pins[i], 0);
    tree_init_roots();
    startup_mark("tree roots");

    /* Start folders are read synchronously: typical folders take well under
     * a millisecond and this avoids an empty first frame. */
    wchar_t home[MAX_PATH] = L"C:\\";
    GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    const wchar_t *left = folders[0] ? folders[0] : home;
    const wchar_t *right = folders[1] ? folders[1] : left;
    for (int i = 0; i < 2; i++) {
        Pane *p = &g.panes[i];
        const wchar_t *start = p->tabs[p->tab].path ? p->tabs[p->tab].path : i == 0 ? left : right;
        fpe_dir *d = read_start(start);
        if (!d) {
            MessageBoxW(NULL, L"Cannot read the start folder.", L"Folderwing", MB_ICONERROR);
            return 1;
        }
        p_show_dir(p, d, NULL, 0);
        fpe_dir_release(d);
        if (p->ntabs > 1) tab_strip_changed(p);
    }
    if (restore && g.dual) set_active(g_ses.active);
    if (fn_active(fn_find("nav.recent")) && g.session[0]) {
        /* the start folders were noted above: the saved ones go after them */
        for (int i = 0; i < g_ses.nrecent && g.nrecent < SES_RECENT_MAX; i++) {
            int dup = 0;
            for (int k = 0; k < g.nrecent && !dup; k++) dup = same_path(g.recent[k], g_ses.recent[i]);
            if (dup) continue;
            g.recent[g.nrecent++] = g_ses.recent[i]; /* moved */
            g_ses.recent[i] = NULL;
        }
    }
    session_free(&g_ses);
    LocalFree(argv);
    startup_mark("panes loaded");

    fn_start_hot(0);
    if (open_panel) pane_run(&g.panes[0], "debug.panel", FN_FROM_CODE);
    ShowWindow(h, show);
    /* SetFocus would activate the window; a launch that asked not to
     * activate gets focus when the user brings the window up (WM_SETFOCUS) */
    if (show != SW_SHOWNOACTIVATE && show != SW_SHOWNA && show != SW_SHOWMINNOACTIVE) SetFocus(g.panes[0].hwnd);
    startup_mark("window shown");

    /* GetMessage, plus the settings folder's change notification while it is watched */
    MSG msg;
    for (;;) {
        HANDLE waits[3];
        DWORD n = 0, watch_index = MAXDWORD, save_index = MAXDWORD, warm_index=MAXDWORD;
        if (g_cfg_watch) { watch_index = n; waits[n++] = g_cfg_watch; }
        if (g_settings_save) { save_index = n; waits[n++] = g_settings_save->thread; }
        HANDLE warm_wait=dropwarm_wait_handle();
        if(warm_wait){warm_index=n;waits[n++]=warm_wait;}
        DWORD r = MsgWaitForMultipleObjectsEx(n, waits, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if(warm_index!=MAXDWORD&&r==WAIT_OBJECT_0+warm_index){
            MSG completed={.message=WM_APP_DROP_WARM};dispatch_timed(&completed);
        }
        if (watch_index != MAXDWORD && r == WAIT_OBJECT_0 + watch_index) settings_poke();
        if (save_index != MAXDWORD && r == WAIT_OBJECT_0 + save_index) {
            /* Keep completion work inside the normal UI timing guard. */
            MSG completed = { .hwnd = g.hwnd, .message = WM_APP_SETTINGS_SAVED };
            dispatch_timed(&completed);
        }
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            if(bulkrename_message(&msg))continue;
            if(foldersize_message(&msg))continue;
            if(foldercompare_message(&msg))continue;
            if(workspaces_message(&msg))continue;
            if(permissions_message(&msg))continue;
            TranslateMessage(&msg);
            dispatch_timed(&msg);
        }
    }
done:
    settings_save_join();
    folderwatch_join();
    fpe_search_index_shutdown();
    fpe_engine_destroy(g.engine);
    archive_sources_finish();
    return 0;
}
