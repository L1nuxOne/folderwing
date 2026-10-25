/* menu.c - popup menus drawn by OpenPilot (see menu.h).
 *
 * Each open level (the menu, then any submenu) is a no-activate popup
 * window. The modal loop below takes the mouse (capture) and the keyboard
 * (it reads key messages before they are dispatched to the focused window),
 * so the explorer keeps its focus, keeps painting and gets everything else.
 * Mouse messages are handled by their screen point, whichever level they
 * arrived at. */
#include "menu.h"

#include <dwmapi.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windowsx.h>

#include "app.h"
#include "theme.h"

#define MAX_LEVELS  6
#define TIMER_ID    1
#define MN_GETHMENU 0x01E1

typedef struct item {
    UINT     id, state, type;
    HMENU    sub;
    wchar_t *text; /* label, '&' marks the mnemonic */
    wchar_t *key;  /* after the tab, or NULL */
    int      y, h; /* in content coordinates */
} item;

typedef struct level {
    HWND  hwnd;
    item *it;
    int   n, hot;
    int   w, content_h, view_h, scroll;
    RECT  screen;
    int   from; /* the item of the level above it opened from */
} level;

static struct {
    level   lv[MAX_LEVELS];
    int     depth;
    HWND    owner, fg;
    HFONT   font, bold;
    int     done, cues;
    UINT    result;
    POINT   last_pt;
    int     open_lv, open_item; /* submenu waiting for the hover delay */
    ULONGLONG open_at;
    UINT    show_delay;
} M;

/* Level windows are made once and kept hidden between menus: creating one
 * costs about as much as showing it (~4 ms each). */
static struct {
    HWND hwnd[MAX_LEVELS];
    int  own_border;
} W;

static int px(int v) { return app_px(v); }
static int pad_y(void) { return px(4); }
static int row_h(void) { return app_row_h() + px(6); }
static int sep_h(void) { return px(9); }
static int gutter(void) { return px(30); }
static int right_pad(void) { return px(14); }
static int arrow_w(void) { return px(16); }

static int is_sep(const item *it) { return (it->type & MFT_SEPARATOR) != 0; }
static int is_off(const item *it) { return (it->state & (MFS_GRAYED | MFS_DISABLED)) != 0; }

/* ---- reading the HMENU ------------------------------------------------------------------------ */

static void free_items(level *l)
{
    for (int i = 0; i < l->n; i++) free(l->it[i].text);
    free(l->it);
    l->it = NULL;
    l->n = 0;
}

/* Owner-drawn and bitmap-only items are left out: only their owner could
 * draw them. Separators never lead, trail or repeat. */
static int read_items(level *l, HMENU m)
{
    int count = GetMenuItemCount(m);
    if (count <= 0) return 0;
    l->it = calloc((size_t)count, sizeof *l->it);
    if (!l->it) return 0;
    for (int i = 0; i < count; i++) {
        wchar_t buf[512];
        MENUITEMINFOW mi = { sizeof mi };
        mi.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
        mi.dwTypeData = buf;
        mi.cch = 512;
        if (!GetMenuItemInfoW(m, (UINT)i, TRUE, &mi)) continue;
        item *it = &l->it[l->n];
        if (mi.fType & MFT_SEPARATOR) {
            if (l->n == 0 || is_sep(&l->it[l->n - 1])) continue;
            it->type = MFT_SEPARATOR;
            l->n++;
            continue;
        }
        if ((mi.fType & MFT_OWNERDRAW) || !mi.dwTypeData || !buf[0]) continue;
        buf[511] = 0;
        it->text = _wcsdup(buf);
        if (!it->text) continue;
        wchar_t *tab = wcschr(it->text, L'\t');
        if (tab) {
            *tab = 0;
            if (tab[1]) it->key = tab + 1;
        }
        it->id = mi.wID;
        it->state = mi.fState;
        it->type = mi.fType;
        it->sub = mi.hSubMenu;
        l->n++;
    }
    while (l->n && is_sep(&l->it[l->n - 1])) l->n--;
    return l->n;
}

/* ---- layout and drawing ----------------------------------------------------------------------- */

static int text_w(HDC dc, const wchar_t *s)
{
    RECT r = { 0, 0, 0, 0 };
    DrawTextW(dc, s, -1, &r, DT_CALCRECT | DT_SINGLELINE);
    return r.right;
}

static void measure(level *l)
{
    HDC dc = GetDC(M.owner);
    HGDIOBJ old = SelectObject(dc, M.font);
    int label = px(140), key = 0, sub = 0, y = pad_y();
    for (int i = 0; i < l->n; i++) {
        item *it = &l->it[i];
        it->y = y;
        it->h = is_sep(it) ? sep_h() : row_h();
        y += it->h;
        if (is_sep(it)) continue;
        SelectObject(dc, (it->state & MFS_DEFAULT) && M.bold ? M.bold : M.font);
        int w = text_w(dc, it->text);
        if (w > label) label = w;
        if (it->key && (w = text_w(dc, it->key)) > key) key = w;
        if (it->sub) sub = 1;
    }
    SelectObject(dc, old);
    ReleaseDC(M.owner, dc);
    if (label > px(460)) label = px(460);
    l->content_h = y + pad_y();
    l->w = gutter() + label + (key ? px(40) + key : 0) + right_pad() + (sub ? arrow_w() : 0);
}

static void line(HDC dc, int x0, int y0, int x1, int y1)
{
    MoveToEx(dc, x0, y0, NULL);
    LineTo(dc, x1, y1);
}

static void paint(level *l, HDC out)
{
    RECT cr;
    GetClientRect(l->hwnd, &cr);
    HDC dc = CreateCompatibleDC(out);
    HBITMAP bmp = CreateCompatibleBitmap(out, cr.right, cr.bottom);
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    HBRUSH bg = CreateSolidBrush(C_POPUP);
    FillRect(dc, &cr, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old_font = SelectObject(dc, M.font);
    HPEN sep_pen = CreatePen(PS_SOLID, 1, C_BORDER);
    HPEN mark_pen = CreatePen(PS_SOLID, px(2) > 1 ? px(2) : 1, C_TEXT);
    HGDIOBJ old_pen = SelectObject(dc, sep_pen);
    int inset = px(4), radius = px(8);
    for (int i = 0; i < l->n; i++) {
        item *it = &l->it[i];
        int y = it->y - l->scroll;
        if (y + it->h <= 0 || y >= cr.bottom) continue;
        if (is_sep(it)) {
            SelectObject(dc, sep_pen);
            line(dc, px(12), y + it->h / 2, cr.right - px(12), y + it->h / 2);
            continue;
        }
        if (i == l->hot) {
            COLORREF c = is_off(it) ? C_SEL_IDLE : C_SEL;
            HBRUSH b = CreateSolidBrush(c);
            HPEN p = CreatePen(PS_SOLID, 1, c);
            HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
            RoundRect(dc, inset, y, cr.right - inset, y + it->h, radius, radius);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(b);
            DeleteObject(p);
        }
        COLORREF fg = is_off(it) ? C_DIM : C_TEXT;
        if (it->state & MFS_CHECKED) {
            int cx = gutter() / 2, cy = y + it->h / 2, s = px(4);
            SelectObject(dc, mark_pen);
            line(dc, cx - s, cy, cx - s / 2, cy + s);
            line(dc, cx - s / 2, cy + s, cx + s + 1, cy - s);
        }
        int right = cr.right - right_pad() - (it->sub ? arrow_w() : 0);
        SetTextColor(dc, fg);
        SelectObject(dc, (it->state & MFS_DEFAULT) && M.bold ? M.bold : M.font);
        int key_w = it->key ? text_w(dc, it->key) + px(24) : 0;
        RECT tr = { gutter(), y, right - key_w, y + it->h };
        DrawTextW(dc, it->text, -1, &tr,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOCLIP | (M.cues ? 0 : DT_HIDEPREFIX));
        SelectObject(dc, M.font);
        if (it->key) {
            RECT kr = { gutter(), y, right, y + it->h };
            SetTextColor(dc, C_DIM);
            DrawTextW(dc, it->key, -1, &kr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
        }
        if (it->sub) {
            int cx = cr.right - right_pad() - arrow_w() / 2 + px(4), cy = y + it->h / 2, s = px(4);
            HPEN ap = CreatePen(PS_SOLID, 1, fg);
            SelectObject(dc, ap);
            line(dc, cx - s / 2, cy - s, cx + s / 2, cy);
            line(dc, cx + s / 2, cy, cx - s / 2 - 1, cy + s + 1);
            SelectObject(dc, sep_pen);
            DeleteObject(ap);
        }
    }
    if (W.own_border) {
        HBRUSH b = CreateSolidBrush(C_BORDER);
        FrameRect(dc, &cr, b);
        DeleteObject(b);
    }
    BitBlt(out, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_font);
    DeleteObject(sep_pen);
    DeleteObject(mark_pen);
    SelectObject(dc, old_bmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

static level *level_of(HWND h)
{
    for (int i = 0; i < M.depth; i++)
        if (M.lv[i].hwnd == h) return &M.lv[i];
    return NULL;
}

static LRESULT CALLBACK menu_proc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    level *lv = level_of(h);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (lv) paint(lv, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case MN_GETHMENU: /* what tests and accessibility tools ask a menu window */
        return (LRESULT)GetPropW(h, L"menu");
    case WM_CANCELMODE:
        M.done = 1;
        return 0;
    case WM_CAPTURECHANGED:
        if (lv == &M.lv[0] && (HWND)l != h) M.done = 1;
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

static const wchar_t *menu_class(void)
{
    static ATOM atom;
    if (!atom) {
        WNDCLASSEXW wc = { sizeof wc };
        wc.lpfnWndProc = menu_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.lpszClassName = L"OpenPilotMenu";
        atom = RegisterClassExW(&wc);
    }
    return L"OpenPilotMenu";
}

/* ---- levels ----------------------------------------------------------------------------------- */

static HWND level_window(int k)
{
    if (W.hwnd[k] && GetWindow(W.hwnd[k], GW_OWNER) != M.owner) {
        DestroyWindow(W.hwnd[k]);
        W.hwnd[k] = NULL;
    }
    if (!W.hwnd[k]) {
        HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, menu_class(), L"", WS_POPUP, 0, 0, 1, 1, M.owner,
                                 NULL, GetModuleHandleW(NULL), NULL);
        if (!h) return NULL;
        /* Windows 11: rounded corners and a border line like its own menus;
         * older systems refuse the attribute and get a drawn border. */
        DWORD corner = 3; /* DWMWCP_ROUNDSMALL */
        W.own_border = FAILED(DwmSetWindowAttribute(h, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof corner));
        if (!W.own_border) {
            COLORREF border = C_BORDER;
            DwmSetWindowAttribute(h, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof border);
        }
        W.hwnd[k] = h;
    }
    return W.hwnd[k];
}

static void close_from(int k)
{
    while (M.depth > k) {
        level *l = &M.lv[--M.depth];
        if (l->hwnd) {
            SetWindowPos(l->hwnd, NULL, 0, 0, 0, 0,
                         SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            RemovePropW(l->hwnd, L"menu");
        }
        free_items(l);
        memset(l, 0, sizeof *l);
    }
    if (M.open_lv >= M.depth) M.open_lv = -1;
}

static RECT work_area(POINT pt)
{
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    return mi.rcWork;
}

/* Opens m as the next level. anchor: the screen rect it opens beside
 * (a submenu: its parent item), or a point (left == right) for the menu. */
static int open_level(HMENU m, RECT anchor, int from)
{
    if (M.depth >= MAX_LEVELS) return 0;
    level *l = &M.lv[M.depth];
    memset(l, 0, sizeof *l);
    l->hot = -1;
    l->from = from;
    if (!read_items(l, m) || !(l->hwnd = level_window(M.depth))) {
        free_items(l);
        l->hwnd = NULL;
        return 0;
    }
    measure(l);
    POINT at = { anchor.left, anchor.top };
    RECT wa = work_area(at);
    int w = l->w, h = l->content_h;
    if (h > wa.bottom - wa.top) h = wa.bottom - wa.top;
    l->view_h = h;
    int x, y;
    if (anchor.left == anchor.right) { /* the menu: below and right of the point, else flipped */
        x = anchor.left + w <= wa.right ? anchor.left : anchor.left - w;
        y = anchor.top + h <= wa.bottom ? anchor.top : anchor.top - h;
    } else { /* a submenu: beside its item, its first row level with it */
        x = anchor.right + w <= wa.right ? anchor.right : anchor.left - w;
        y = anchor.top - pad_y();
        if (y + h > wa.bottom) y = wa.bottom - h;
    }
    if (x + w > wa.right) x = wa.right - w;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;
    SetPropW(l->hwnd, L"menu", (HANDLE)m);
    SetRect(&l->screen, x, y, x + w, y + h);
    M.depth++;
    SetWindowPos(l->hwnd, HWND_TOP, x, y, w, h, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    UpdateWindow(l->hwnd);
    return 1;
}
static void redraw(level *l)
{
    if (l->hwnd) InvalidateRect(l->hwnd, NULL, FALSE);
}

static void set_hot(int k, int i)
{
    level *l = &M.lv[k];
    if (l->hot == i) return;
    l->hot = i;
    if (i >= 0) { /* keep it in view */
        item *it = &l->it[i];
        int top = it->y - (i == 0 ? pad_y() : 0), bottom = it->y + it->h + (i == l->n - 1 ? pad_y() : 0);
        if (top < l->scroll) l->scroll = top;
        if (bottom > l->scroll + l->view_h) l->scroll = bottom - l->view_h;
    }
    redraw(l);
}

static void open_sub(int k, int i)
{
    level *l = &M.lv[k];
    if (M.depth > k + 1 && M.lv[k + 1].from == i) return;
    close_from(k + 1);
    M.open_lv = -1;
    item *it = &l->it[i];
    if (!it->sub || is_off(it)) return;
    RECT r = { l->screen.left, l->screen.top + it->y - l->scroll, l->screen.right, 0 };
    r.bottom = r.top + it->h;
    open_level(it->sub, r, i);
}

static void scroll_by(level *l, int dy)
{
    int max = l->content_h - l->view_h;
    if (max <= 0) return;
    int s = l->scroll + dy;
    l->scroll = s < 0 ? 0 : s > max ? max : s;
    redraw(l);
}

/* ---- input ------------------------------------------------------------------------------------ */

static void pick(item *it)
{
    if (is_sep(it) || is_off(it)) return;
    M.result = it->id;
    M.done = 1;
}

static void activate(int k, int i)
{
    item *it = &M.lv[k].it[i];
    if (it->sub) {
        open_sub(k, i);
        if (M.depth > k + 1) set_hot(k + 1, 0);
    } else {
        pick(it);
    }
}

/* The level and item at a screen point (item -1: between items, level -1: outside). */
static int hit(POINT pt, int *item_out)
{
    *item_out = -1;
    for (int k = M.depth - 1; k >= 0; k--) {
        level *l = &M.lv[k];
        if (!PtInRect(&l->screen, pt)) continue;
        int y = pt.y - l->screen.top + l->scroll;
        for (int i = 0; i < l->n; i++)
            if (y >= l->it[i].y && y < l->it[i].y + l->it[i].h && !is_sep(&l->it[i])) *item_out = i;
        return k;
    }
    return -1;
}

/* Client-area messages carry the point relative to the window that got
 * them (the capturing level); the others carry screen points. */
static POINT mouse_point(const MSG *msg)
{
    POINT pt = { GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam) };
    if (msg->message >= WM_MOUSEFIRST && msg->message <= WM_MOUSELAST && msg->message != WM_MOUSEWHEEL &&
        msg->message != WM_MOUSEHWHEEL)
        ClientToScreen(msg->hwnd, &pt);
    return pt;
}

static void on_mouse(const MSG *msg)
{
    POINT pt = mouse_point(msg);
    int i, k = hit(pt, &i);
    switch (msg->message) {
    case WM_MOUSEMOVE:
    case WM_NCMOUSEMOVE:
        if (pt.x == M.last_pt.x && pt.y == M.last_pt.y) return;
        M.last_pt = pt;
        if (k < 0) {
            level *deepest = &M.lv[M.depth - 1];
            if (deepest->hot >= 0) set_hot(M.depth - 1, -1);
            return;
        }
        set_hot(k, i);
        if (i >= 0 && M.lv[k].it[i].sub) {
            if (!(M.depth > k + 1 && M.lv[k + 1].from == i)) {
                M.open_lv = k;
                M.open_item = i;
                M.open_at = GetTickCount64() + M.show_delay;
            }
        } else if (i >= 0 && M.depth > k + 1) {
            close_from(k + 1);
        }
        for (int j = 0; j < k; j++) /* the way down stays lit */
            set_hot(j, M.lv[j + 1].from);
        return;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_NCLBUTTONDOWN:
    case WM_NCRBUTTONDOWN:
        if (k < 0) M.done = 1;
        else if (i >= 0 && M.lv[k].it[i].sub) open_sub(k, i);
        return;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        if (k >= 0 && i >= 0 && !M.lv[k].it[i].sub) pick(&M.lv[k].it[i]);
        return;
    case WM_MOUSEWHEEL:
        if (k >= 0) scroll_by(&M.lv[k], -GET_WHEEL_DELTA_WPARAM(msg->wParam) * row_h() * 3 / WHEEL_DELTA);
        return;
    }
}

static int step(level *l, int from, int dir)
{
    if (!l->n) return -1;
    int i = from;
    for (int n = 0; n < l->n; n++) {
        i = i < 0 ? (dir > 0 ? 0 : l->n - 1) : (i + dir + l->n) % l->n;
        if (!is_sep(&l->it[i])) return i;
    }
    return -1;
}

/* 1: the key was the menu's (no character is made from it). */
static int on_key(UINT vk)
{
    int k = M.depth - 1;
    level *l = &M.lv[k];
    switch (vk) {
    case VK_DOWN: set_hot(k, step(l, l->hot, 1)); break;
    case VK_UP: set_hot(k, step(l, l->hot, -1)); break;
    case VK_HOME: set_hot(k, step(l, -1, 1)); break;
    case VK_END: set_hot(k, step(l, -1, -1)); break;
    case VK_RIGHT:
        if (l->hot >= 0 && l->it[l->hot].sub) activate(k, l->hot);
        break;
    case VK_LEFT:
        if (k > 0) close_from(k);
        break;
    case VK_ESCAPE:
        if (k > 0) close_from(k);
        else M.done = 1;
        break;
    case VK_RETURN:
        if (l->hot >= 0) activate(k, l->hot);
        break;
    case VK_MENU:
    case VK_F10:
        M.done = 1;
        break;
    default:
        return 0;
    }
    return 1;
}

static wchar_t mnemonic(const wchar_t *s)
{
    for (const wchar_t *p = s; *p; p++)
        if (p[0] == L'&' && p[1]) {
            if (p[1] != L'&') return p[1];
            p++;
        }
    return 0;
}

/* A letter picks the item it marks (&x), or else the item it starts;
 * when several share it, each press moves to the next one. */
static void on_char(wchar_t ch)
{
    int k = M.depth - 1;
    level *l = &M.lv[k];
    ch = (wchar_t)(uintptr_t)CharLowerW((LPWSTR)(uintptr_t)ch);
    int marked = 0;
    for (int i = 0; i < l->n; i++)
        if (!is_sep(&l->it[i]) && mnemonic(l->it[i].text)) marked = 1;
    int first = -1, next = -1, count = 0;
    for (int i = 0; i < l->n; i++) {
        item *it = &l->it[i];
        if (is_sep(it)) continue;
        wchar_t c = marked ? mnemonic(it->text) : it->text[0];
        if (!c || (wchar_t)(uintptr_t)CharLowerW((LPWSTR)(uintptr_t)c) != ch) continue;
        count++;
        if (first < 0) first = i;
        if (next < 0 && i > l->hot) next = i;
    }
    if (!count) return;
    int at = next >= 0 ? next : first;
    set_hot(k, at);
    if (count == 1) activate(k, at);
}

/* 1: the message was the menu's. */
static int filter(MSG *msg)
{
    UINT m = msg->message;
    if ((m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || (m >= WM_NCMOUSEMOVE && m <= WM_NCXBUTTONDBLCLK)) {
        on_mouse(msg);
        return 1;
    }
    switch (m) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (!M.cues) {
            M.cues = 1;
            for (int k = 0; k < M.depth; k++) redraw(&M.lv[k]);
        }
        if (!on_key((UINT)msg->wParam)) TranslateMessage(msg); /* letters pick by mnemonic */
        return 1;
    case WM_CHAR:
    case WM_SYSCHAR:
        if (msg->wParam > L' ') on_char((wchar_t)msg->wParam);
        return 1;
    case WM_KEYUP:
    case WM_SYSKEYUP:
    case WM_DEADCHAR:
    case WM_SYSDEADCHAR:
        return 1;
    case WM_TIMER:
        return msg->hwnd == M.lv[0].hwnd && msg->wParam == TIMER_ID;
    }
    return 0;
}

static void poll(void)
{
    if (!IsWindow(M.owner) || GetForegroundWindow() != M.fg || GetCapture() != M.lv[0].hwnd) M.done = 1;
    if (M.open_lv >= 0 && M.open_lv < M.depth && GetTickCount64() >= M.open_at) {
        int k = M.open_lv;
        M.open_lv = -1;
        if (M.lv[k].hot == M.open_item) open_sub(k, M.open_item);
    }
}

UINT menu_track(HMENU m, HWND owner, POINT pt)
{
    if (M.depth || !m) return 0; /* one menu at a time */
    int64_t t0 = fpe_prof_begin();
    memset(&M, 0, sizeof M);
    M.owner = owner;
    M.fg = GetForegroundWindow();
    M.open_lv = -1;
    M.font = app_font();
    M.last_pt = pt;
    UINT delay = 400;
    SystemParametersInfoW(SPI_GETMENUSHOWDELAY, 0, &delay, 0);
    M.show_delay = delay > 300 ? 300 : delay;
    for (int i = GetMenuItemCount(m) - 1; i >= 0; i--)
        if (GetMenuState(m, (UINT)i, MF_BYPOSITION) & MFS_DEFAULT) {
            LOGFONTW lf;
            if (GetObjectW(M.font, sizeof lf, &lf)) {
                lf.lfWeight = FW_BOLD;
                M.bold = CreateFontIndirectW(&lf);
            }
            break;
        }
    RECT at = { pt.x, pt.y, pt.x, pt.y };
    if (!open_level(m, at, -1)) {
        if (M.bold) DeleteObject(M.bold);
        return 0;
    }
    FPE_PROF_END("menu.open", t0);
    SetCapture(M.lv[0].hwnd);
    SetTimer(M.lv[0].hwnd, TIMER_ID, 50, NULL);

    while (!M.done) {
        poll();
        if (M.done) break;
        MSG msg;
        BOOL r = GetMessageW(&msg, NULL, 0, 0);
        if (r <= 0) {
            if (r == 0) PostQuitMessage((int)msg.wParam);
            break;
        }
        if (filter(&msg)) continue;
        DispatchMessageW(&msg);
    }

    UINT result = M.result;
    KillTimer(M.lv[0].hwnd, TIMER_ID);
    HWND first = M.lv[0].hwnd;
    M.done = 1;
    close_from(0);
    if (GetCapture() == first) ReleaseCapture();
    if (M.bold) DeleteObject(M.bold);
    M.bold = NULL;
    return result;
}
