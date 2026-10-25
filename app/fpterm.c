/*
 * fpterm.c - the terminal panel app (app/panelproto.h).
 *
 * Each session is a program (a shell, or a command) on a pseudo console
 * (ConPTY); what it prints goes through libvterm, and the screen is drawn
 * with GDI into the panel's shared pictures. Sessions are tabs. A session
 * whose program ended stays where it is, output and all, until it is left:
 * then it moves to the history (the last few), from where it can be read
 * or run again. Shells are started so they report their folder and prompts
 * (OSC 9;9 and 133, see app/termutil.h): the explorer's pane can follow the
 * shell, and an idle shell follows the pane.
 *
 * Threads: the UI thread owns everything; the host pipe and each session's
 * output have a reader thread that posts what arrives, and a wait posts
 * when a session's program ends. Writing to a program is synchronous: the
 * pseudo console reads its input promptly.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <vterm.h>

#include "panelproto.h"
#include "pathwire.h"
#include "termutil.h"
#include "theme.h"
#include "startuptrace.h"

#define WM_HOST      (WM_APP + 1) /* lParam: msg * */
#define WM_HOST_GONE (WM_APP + 2)
#define WM_T_OUT     (WM_APP + 3) /* wParam: session id, lParam: chunk * */
#define WM_T_EXIT    (WM_APP + 4) /* wParam: session id */
#define WM_T_EOF     (WM_APP + 5) /* wParam: session id */
#define TABS_MAX     16
#define HIST_MAX     50
#define HOTS_MAX     96

typedef struct msg {
    panelp_head head;
    uint8_t     data[];
} msg;

typedef struct chunk {
    size_t n;
    char   data[];
} chunk;

/* A cell as kept in the scrollback and drawn: colors resolved, attributes in ch's high bits */
typedef struct tcell {
    uint32_t ch, fg, bg;
} tcell;
#define CH_MASK   0x1FFFFFu
#define AT_BOLD   (1u << 21)
#define AT_ITALIC (1u << 22)
#define AT_UNDER  (1u << 23)
#define AT_STRIKE (1u << 24)
#define AT_WIDE   (1u << 25) /* the first half of a double-width character */
#define AT_SKIP   (1u << 26) /* the second half */

typedef struct sline {
    int   n;
    tcell c[];
} sline;

enum { S_RUN, S_DONE };

typedef struct sess {
    int      id, state;
    HPCON    pc;
    HANDLE   proc, in_w, wait;
    VTerm   *vt;
    VTermScreen *scr;
    int      rows, cols;
    sline  **sb; /* a ring of sb_cap lines */
    int      sb_cap, sb_n, sb_start;
    int64_t  dropped; /* lines that fell off the ring: line numbers stay valid */
    int      scroll;  /* lines scrolled back from the bottom */
    int      cursor_on, altscreen, mouse;
    int      eof;     /* its output ended */

    /* what was asked for: Run again does it again */
    wchar_t  shell[16], dir[MAX_PATH], title_req[64];
    wchar_t *cmd;      /* NULL: a shell */
    int      kind;     /* TERM_SH_* */
    wchar_t  name[64]; /* the program, for its tab */
    wchar_t  title[128];
    char     tbuf[256];
    size_t   tlen;

    /* the shell's folder and prompt (OSC 9;9 / 7 / 133) */
    wchar_t  cwd[MAX_PATH];
    wchar_t  cd_sent[MAX_PATH]; /* where we told it to go: its report of that isn't news */
    int      marks, at_prompt, typed, cwd_verified;
    uint32_t input_chars, input_enters, input_error;
    char    *osc;
    size_t   osc_n, osc_cap;

    DWORD    exit_code;
    uint64_t t_start, t_end;
    int      nlines; /* history: lines kept */
} sess;

enum { V_TAB, V_HISTLIST, V_HISTITEM };
enum { H_TAB = 1, H_TABX, H_NEW, H_HIST, H_RUN, H_CLOSE, H_BACK, H_HITEM, H_EMPTYNEW };

typedef struct hot {
    RECT r;
    int  kind, idx;
} hot;

static struct {
    HANDLE   in, out;
    HWND     hwnd;
    int      dpi, w, h;
    HANDLE   section;
    uint32_t seq;
    uint8_t *shared;
    HDC      mdc;
    HBITMAP  back;
    void    *back_bits;
    int      next, waiting, pending, dirty, visible, focused;
    uint64_t last_paint;
    uint32_t cursor;

    HFONT    mono[4], ui, ui_small;
    int      cw, chh, base;

    sess    *tabs[TABS_MAX];
    int      ntabs, cur;
    sess    *hist[HIST_MAX]; /* newest first */
    int      nhist;
    int      view, hsel, hscroll;
    int      ids;
    int      reported_ready, reported_prompt, first_prompt;

    hot      hots[HOTS_MAX];
    int      nhot, hover_kind, hover_idx;

    /* selection: absolute line numbers (sess.dropped + index) */
    sess    *sel_s;
    int64_t  sel_al, sel_bl;
    int      sel_ac, sel_bc, selecting, sel_on;
    int      eat_char; /* the key went somewhere else: its WM_CHAR doesn't type */
    wchar_t  hi_surrogate;

    /* settings (PANELP_TERM_CONFIG) */
    wchar_t  font[64], shell[16];
    int      pt, scrollback, history, follow;
    wchar_t  pane[MAX_PATH], want[MAX_PATH]; /* the pane's folder; where the idle shell is to go */
} A;

static int S(int v) { return MulDiv(v, A.dpi, 96); }

/* ---- host pipe ------------------------------------------------------------------------- */

static void send_msg(uint32_t type, const void *data, uint32_t size)
{
    panelp_head h = { type, size };
    DWORD w;
    WriteFile(A.out, &h, sizeof h, &w, NULL);
    if (size) WriteFile(A.out, data, size, &w, NULL);
}

static int read_all(HANDLE f, void *buf, DWORD n)
{
    uint8_t *p = buf;
    while (n) {
        DWORD got = 0;
        if (!ReadFile(f, p, n, &got, NULL) || !got) return 0;
        p += got;
        n -= got;
    }
    return 1;
}

static DWORD WINAPI host_reader(void *arg)
{
    HWND to = arg;
    for (;;) {
        panelp_head h;
        if (!read_all(A.in, &h, sizeof h) || h.size > PANELP_MAX) break;
        msg *m = malloc(sizeof *m + h.size);
        if (!m || !read_all(A.in, m->data, h.size)) {
            free(m);
            break;
        }
        m->head = h;
        if (!PostMessageW(to, WM_HOST, 0, (LPARAM)m)) free(m);
    }
    PostMessageW(to, WM_HOST_GONE, 0, 0);
    return 0;
}

static void send_text(uint32_t type, const wchar_t *s)
{
    char buf[PATHWIRE_PREFIX_LEN + MAX_PATH * 4 + 1];
    size_t n = pathwire_encode(s, buf, sizeof buf);
    if (n) send_msg(type, buf, (uint32_t)n);
}

/* ---- sessions: lookup, scrollback ------------------------------------------------------ */

static sess *by_id(int id)
{
    for (int i = 0; i < A.ntabs; i++)
        if (A.tabs[i]->id == id) return A.tabs[i];
    return NULL;
}

static sess *cur_sess(void)
{
    if (A.view == V_HISTITEM) return A.hsel >= 0 && A.hsel < A.nhist ? A.hist[A.hsel] : NULL;
    if (A.view != V_TAB || A.cur < 0 || A.cur >= A.ntabs) return NULL;
    return A.tabs[A.cur];
}

/* Process/window existence precedes the shell's first prompt. Publish the
 * current live session's state independently of frame acknowledgements, so
 * automation can wait for actual readiness without guessing a startup delay. */
static void publish_readiness(void)
{
    static uint32_t revision,chars,enters,error;
    static int session,kind;
    static wchar_t cwd[MAX_PATH];
    sess *s = cur_sess();
    int ready = s && A.view == V_TAB && s->state == S_RUN && s->in_w && !s->eof && s->cwd_verified;
    int prompt = ready && s->marks && s->at_prompt && !s->typed;
    int id=ready?s->id:0,sh=ready?s->kind:0;
    uint32_t nc=ready?s->input_chars:0,ne=ready?s->input_enters:0,err=s?s->input_error:0;
    const wchar_t *dir=ready?s->cwd:L"";
    if (ready != A.reported_ready || prompt != A.reported_prompt || session!=id || kind!=sh ||
        chars!=nc || enters!=ne || error!=err || wcscmp(cwd,dir)) {
        /* Odd/even revision brackets one coherent read-only snapshot. The
         * hidden window caption carries cwd without a cross-process pointer. */
        SetPropW(A.hwnd,L"Terminal.Revision",(HANDLE)(uintptr_t)++revision);
        SetPropW(A.hwnd, L"Terminal.Ready", (HANDLE)(uintptr_t)ready);
        SetPropW(A.hwnd, L"Terminal.AtPrompt", (HANDLE)(uintptr_t)prompt);
        SetPropW(A.hwnd,L"Terminal.Session",(HANDLE)(uintptr_t)id);
        SetPropW(A.hwnd,L"Terminal.Kind",(HANDLE)(uintptr_t)sh);
        SetPropW(A.hwnd,L"Terminal.Chars",(HANDLE)(uintptr_t)nc);
        SetPropW(A.hwnd,L"Terminal.Enters",(HANDLE)(uintptr_t)ne);
        SetPropW(A.hwnd,L"Terminal.InputError",(HANDLE)(uintptr_t)err);
        if(wcscmp(cwd,dir))SetWindowTextW(A.hwnd,dir);
        A.reported_ready=ready;A.reported_prompt=prompt;session=id;kind=sh;chars=nc;enters=ne;error=err;wcscpy(cwd,dir);
        SetPropW(A.hwnd,L"Terminal.Revision",(HANDLE)(uintptr_t)++revision);
    }
    if (prompt && !A.first_prompt) {
        A.first_prompt = 1;
        panel_startup_trace("terminal.first_prompt");
    }
}

static sline *sb_get(const sess *s, int i) { return s->sb[(s->sb_start + i) % s->sb_cap]; }

static void sb_push(sess *s, sline *l)
{
    if (!s->sb) {
        s->sb_cap = A.scrollback > 100 ? A.scrollback : 100;
        s->sb = calloc((size_t)s->sb_cap, sizeof *s->sb);
        if (!s->sb) {
            free(l);
            return;
        }
    }
    if (s->sb_n == s->sb_cap) {
        free(s->sb[s->sb_start]);
        s->sb[s->sb_start] = l;
        s->sb_start = (s->sb_start + 1) % s->sb_cap;
        s->dropped++;
    } else {
        s->sb[(s->sb_start + s->sb_n) % s->sb_cap] = l;
        s->sb_n++;
    }
}

static int is_blank(const tcell *c) { return ((c->ch & CH_MASK) == 0 || (c->ch & CH_MASK) == ' ') && c->bg == C_BG && !(c->ch & AT_UNDER); }

static void conv(sess *s, const VTermScreenCell *c, tcell *o)
{
    VTermColor fg = c->fg, bg = c->bg;
    vterm_screen_convert_color_to_rgb(s->scr, &fg);
    vterm_screen_convert_color_to_rgb(s->scr, &bg);
    uint32_t f = RGB(fg.rgb.red, fg.rgb.green, fg.rgb.blue), b = RGB(bg.rgb.red, bg.rgb.green, bg.rgb.blue);
    if (VTERM_COLOR_IS_DEFAULT_FG(&c->fg)) f = C_TEXT;
    if (VTERM_COLOR_IS_DEFAULT_BG(&c->bg)) b = C_BG;
    if (c->attrs.reverse) {
        uint32_t t = f;
        f = b, b = t;
    }
    if (c->attrs.conceal) f = b;
    uint32_t ch = c->chars[0];
    uint32_t at = (c->attrs.bold ? AT_BOLD : 0) | (c->attrs.italic ? AT_ITALIC : 0) | (c->attrs.underline ? AT_UNDER : 0) |
                  (c->attrs.strike ? AT_STRIKE : 0) | (c->width == 2 ? AT_WIDE : 0);
    if (ch == (uint32_t)-1) ch = 0, at = AT_SKIP;
    o->ch = (ch & CH_MASK) | at;
    o->fg = f;
    o->bg = b;
}

static sline *line_make(const tcell *c, int n)
{
    while (n > 0 && is_blank(&c[n - 1])) n--;
    sline *l = malloc(sizeof *l + (size_t)n * sizeof(tcell));
    if (!l) return NULL;
    l->n = n;
    memcpy(l->c, c, (size_t)n * sizeof(tcell));
    return l;
}

/* how many lines there are to show: scrollback, then the screen */
static int total_lines(const sess *s) { return s->sb_n + (s->vt ? s->rows : 0); }

/* line i's cells into buf (cols wide at most); returns how many */
static int line_cells(sess *s, int i, tcell *buf, int max)
{
    if (i < 0) return 0;
    if (i < s->sb_n) {
        sline *l = sb_get(s, i);
        int n = l->n < max ? l->n : max;
        memcpy(buf, l->c, (size_t)n * sizeof(tcell));
        return n;
    }
    i -= s->sb_n;
    if (!s->vt || i >= s->rows) return 0;
    int n = s->cols < max ? s->cols : max;
    for (int c = 0; c < n; c++) {
        VTermScreenCell vc;
        VTermPos p = { i, c };
        if (!vterm_screen_get_cell(s->scr, p, &vc)) {
            buf[c] = (tcell){ ' ', C_TEXT, C_BG };
            continue;
        }
        conv(s, &vc, &buf[c]);
    }
    return n;
}

/* ---- libvterm callbacks ---------------------------------------------------------------- */

static void write_pty(sess *s, const char *p, size_t n)
{
    if (!s->in_w || s->state != S_RUN) {s->input_error=ERROR_BROKEN_PIPE;return;}
    DWORD w;
    while (n) {
        if (!WriteFile(s->in_w, p, (DWORD)n, &w, NULL) || !w) {
            DWORD error=GetLastError();s->input_error=error?error:ERROR_WRITE_FAULT;return;
        }
        p += w;
        n -= w;
    }
}

static void vt_output(const char *s_, size_t len, void *user) { write_pty(user, s_, len); }

static int cb_pushline(int cols, const VTermScreenCell *cells, void *user)
{
    sess *s = user;
    tcell buf[512];
    int n = cols < 512 ? cols : 512;
    for (int i = 0; i < n; i++) conv(s, &cells[i], &buf[i]);
    sline *l = line_make(buf, n);
    if (l) sb_push(s, l);
    if (s->scroll && s->scroll < s->sb_n) s->scroll++; /* what is read stays put */
    return 1;
}

static int cb_popline(int cols, VTermScreenCell *cells, void *user)
{
    sess *s = user;
    if (!s->sb_n) return 0;
    int last = (s->sb_start + s->sb_n - 1) % s->sb_cap;
    sline *l = s->sb[last];
    for (int i = 0; i < cols; i++) {
        VTermScreenCell *c = &cells[i];
        memset(c, 0, sizeof *c);
        c->width = 1;
        tcell t = i < l->n ? l->c[i] : (tcell){ ' ', C_TEXT, C_BG };
        uint32_t ch = t.ch & CH_MASK;
        c->chars[0] = t.ch & AT_SKIP ? (uint32_t)-1 : ch ? ch : ' ';
        if (t.ch & AT_WIDE) c->width = 2;
        c->attrs.bold = !!(t.ch & AT_BOLD);
        c->attrs.italic = !!(t.ch & AT_ITALIC);
        c->attrs.underline = t.ch & AT_UNDER ? 1 : 0;
        c->attrs.strike = !!(t.ch & AT_STRIKE);
        vterm_color_rgb(&c->fg, GetRValue(t.fg), GetGValue(t.fg), GetBValue(t.fg));
        vterm_color_rgb(&c->bg, GetRValue(t.bg), GetGValue(t.bg), GetBValue(t.bg));
        if (t.fg == C_TEXT) c->fg.type |= VTERM_COLOR_DEFAULT_FG;
        if (t.bg == C_BG) c->bg.type |= VTERM_COLOR_DEFAULT_BG;
    }
    free(l);
    s->sb[last] = NULL;
    s->sb_n--;
    if (s->scroll > s->sb_n) s->scroll = s->sb_n;
    return 1;
}

static void utf8_to_w(const char *s, size_t n, wchar_t *out, int cap)
{
    int k = MultiByteToWideChar(CP_UTF8, 0, s, (int)n, out, cap - 1);
    out[k > 0 ? k : 0] = 0;
}

static int cb_prop(VTermProp prop, VTermValue *val, void *user)
{
    sess *s = user;
    switch (prop) {
    case VTERM_PROP_CURSORVISIBLE: s->cursor_on = val->boolean; break;
    case VTERM_PROP_ALTSCREEN: s->altscreen = val->boolean; break;
    case VTERM_PROP_MOUSE: s->mouse = val->number; break;
    case VTERM_PROP_TITLE:
        if (val->string.initial) s->tlen = 0;
        for (size_t i = 0; i < val->string.len && s->tlen + 1 < sizeof s->tbuf; i++) s->tbuf[s->tlen++] = val->string.str[i];
        if (val->string.final) utf8_to_w(s->tbuf, s->tlen, s->title, 128);
        break;
    default: break;
    }
    return 1;
}

static void cwd_changed(sess *s);
static void follow_pane(sess *s);

static void osc_done(sess *s, int cmd)
{
    const char *p = s->osc;
    size_t n = s->osc_n;
    wchar_t dir[MAX_PATH];
    if (term_osc_cwd(cmd, p, n, dir, MAX_PATH)) {
        if (s->kind == TERM_SH_WSL && cmd == 9) {
            wchar_t distro[128], linux[MAX_PATH];
            /* The helper must chdir successfully before Bash starts. Wait
             * for Bash's actual native cwd; a symlink may change its spelling. */
            if (term_wsl_split(dir, distro, 128, linux, MAX_PATH)) s->cwd_verified = 1;
        }
        if (wcscmp(dir, s->cwd)) {
            wcscpy(s->cwd,dir);
            cwd_changed(s);
        }
        return;
    }
    if (cmd == 133 && n >= 1) {
        s->marks = 1;
        if (p[0] == 'A' || p[0] == 'D') s->at_prompt = 0;
        if (p[0] == 'B') {
            s->at_prompt = 1;
            s->typed = 0;
            follow_pane(s);
        }
        if (p[0] == 'C') s->at_prompt = 0;
    }
}

static int cb_osc(int cmd, VTermStringFragment frag, void *user)
{
    sess *s = user;
    if (cmd != 7 && cmd != 9 && cmd != 133 && cmd != 1337) return 0;
    if (frag.initial) s->osc_n = 0;
    if (s->osc_n + frag.len + 1 > s->osc_cap) {
        size_t cap = s->osc_n + frag.len + 256;
        if (cap > 8192) return 1; /* nothing that long is a folder */
        char *b = realloc(s->osc, cap);
        if (!b) return 1;
        s->osc = b;
        s->osc_cap = cap;
    }
    memcpy(s->osc + s->osc_n, frag.str, frag.len);
    s->osc_n += frag.len;
    s->osc[s->osc_n] = 0;
    if (frag.final) osc_done(s, cmd);
    return 1;
}

static const VTermScreenCallbacks screen_cbs = {
    .settermprop = cb_prop,
    .sb_pushline = cb_pushline,
    .sb_popline = cb_popline,
};
static const VTermStateFallbacks fallbacks = { .osc = cb_osc };

/* a dark palette that reads well on C_BG */
static const uint32_t palette[16] = {
    0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC, 0x11A8CD, 0xE5E5E5,
    0x666666, 0xF14C4C, 0x23D18B, 0xF5F543, 0x3B8EEA, 0xD670D6, 0x29B8DB, 0xFFFFFF,
};

static int vt_make(sess *s)
{
    s->vt = vterm_new(s->rows, s->cols);
    if (!s->vt) return 0;
    vterm_set_utf8(s->vt, 1);
    vterm_output_set_callback(s->vt, vt_output, s);
    s->scr = vterm_obtain_screen(s->vt);
    vterm_screen_set_callbacks(s->scr, &screen_cbs, s);
    vterm_screen_set_unrecognised_fallbacks(s->scr, &fallbacks, s);
    vterm_screen_enable_altscreen(s->scr, 1);
    VTermState *st = vterm_obtain_state(s->vt);
    for (int i = 0; i < 16; i++) {
        VTermColor c;
        vterm_color_rgb(&c, (uint8_t)(palette[i] >> 16), (uint8_t)(palette[i] >> 8), (uint8_t)palette[i]);
        vterm_state_set_palette_color(st, i, &c);
    }
    VTermColor fg, bg;
    vterm_color_rgb(&fg, GetRValue(C_TEXT), GetGValue(C_TEXT), GetBValue(C_TEXT));
    vterm_color_rgb(&bg, GetRValue(C_BG), GetGValue(C_BG), GetBValue(C_BG));
    vterm_screen_set_default_colors(s->scr, &fg, &bg);
    vterm_state_set_bold_highbright(st, 1);
    vterm_screen_reset(s->scr, 1);
    s->cursor_on = 1;
    return 1;
}

/* ---- the panel's size in cells -------------------------------------------------------- */

static int tabbar_h(void) { return S(28); }
static int footer_h(void) { return S(34); }
static int pad_x(void) { return S(6); }

static void grid_size(int *rows, int *cols)
{
    int bw = A.w - 2 * pad_x() - S(6), bh = A.h - tabbar_h() - S(4);
    *cols = A.cw ? bw / A.cw : 80;
    *rows = A.chh ? bh / A.chh : 24;
    if (*cols < 10) *cols = 10;
    if (*rows < 3) *rows = 3;
}

static void sess_resize(sess *s)
{
    if (!s->vt || s->state != S_RUN) return;
    int rows, cols;
    grid_size(&rows, &cols);
    if (rows == s->rows && cols == s->cols) return;
    s->rows = rows;
    s->cols = cols;
    vterm_set_size(s->vt, rows, cols);
    if (s->pc) ResizePseudoConsole(s->pc, (COORD){ (SHORT)cols, (SHORT)rows });
}

/* ---- starting and ending programs ------------------------------------------------------- */

typedef struct out_arg {
    HANDLE from;
    HWND   to;
    int    id;
} out_arg;

static DWORD WINAPI out_reader(void *arg)
{
    out_arg a = *(out_arg *)arg;
    free(arg);
    for (;;) {
        chunk *c = malloc(sizeof *c + 32768);
        DWORD got = 0;
        if (!c || !ReadFile(a.from, c->data, 32768, &got, NULL) || !got) {
            free(c);
            break;
        }
        c->n = got;
        if (!PostMessageW(a.to, WM_T_OUT, (WPARAM)a.id, (LPARAM)c)) free(c);
    }
    CloseHandle(a.from);
    PostMessageW(a.to, WM_T_EOF, (WPARAM)a.id, 0);
    return 0;
}

static void CALLBACK on_exit_wait(void *arg, BOOLEAN timed_out)
{
    PostMessageW(A.hwnd, WM_T_EXIT, (WPARAM)(intptr_t)arg, 0);
}

typedef struct closer_arg {
    HPCON  pc;
    HANDLE proc;
} closer_arg;

/* Closing a pseudo console can wait for its program and for its output to be read: off the UI thread. */
static DWORD WINAPI closer(void *arg)
{
    closer_arg a = *(closer_arg *)arg;
    free(arg);
    if (a.pc) ClosePseudoConsole(a.pc);
    if (a.proc) {
        if (WaitForSingleObject(a.proc, 2000) != WAIT_OBJECT_0) TerminateProcess(a.proc, 1);
        CloseHandle(a.proc);
    }
    return 0;
}

/* The pseudo console goes, and with it the program if it still runs. */
static void sess_detach(sess *s)
{
    if (s->wait) UnregisterWaitEx(s->wait, NULL), s->wait = NULL;
    if (s->in_w) CloseHandle(s->in_w), s->in_w = NULL;
    if (s->pc || s->proc) {
        closer_arg *a = malloc(sizeof *a);
        HANDLE t = NULL;
        if (a) {
            *a = (closer_arg){ s->pc, s->proc };
            t = CreateThread(NULL, 64 * 1024, closer, a, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        }
        if (t) CloseHandle(t);
        else {
            free(a);
            if (s->proc) TerminateProcess(s->proc, 1), CloseHandle(s->proc);
            if (s->pc) ClosePseudoConsole(s->pc);
        }
    }
    s->pc = NULL;
    s->proc = NULL;
}

static void sess_free(sess *s)
{
    if (!s) return;
    sess_detach(s);
    if (s->vt) vterm_free(s->vt);
    for (int i = 0; i < s->sb_n; i++) free(sb_get(s, i));
    free(s->sb);
    free(s->osc);
    free(s->cmd);
    if (A.sel_s == s) A.sel_s = NULL, A.sel_on = 0;
    free(s);
}

static void vt_say(sess *s, const wchar_t *text)
{
    char buf[1024];
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, buf, sizeof buf, NULL, NULL);
    if (n > 1 && s->vt) vterm_input_write(s->vt, buf, (size_t)(n - 1));
}

static int find_exe(const wchar_t *name, wchar_t *out)
{
    return SearchPathW(NULL, name, NULL, MAX_PATH, out, NULL) > 0;
}

/* The environment for cmd: PROMPT also reports the folder and marks the prompt. */
static wchar_t *cmd_env(void)
{
    wchar_t user[512] = L"", prompt[1024];
    GetEnvironmentVariableW(L"PROMPT", user, 512);
    if (!term_cmd_prompt(user, prompt, 1024)) return NULL;
    wchar_t *env = GetEnvironmentStringsW();
    if (!env) return NULL;
    size_t total = 0;
    for (wchar_t *p = env; *p; p += wcslen(p) + 1) total += wcslen(p) + 1;
    size_t cap = total + wcslen(prompt) + 16;
    wchar_t *out = malloc(cap * sizeof(wchar_t)), *o = out;
    if (out) {
        for (wchar_t *p = env; *p; p += wcslen(p) + 1) {
            if (!_wcsnicmp(p, L"PROMPT=", 7)) continue;
            size_t n = wcslen(p) + 1;
            memcpy(o, p, n * sizeof(wchar_t));
            o += n;
        }
        o += swprintf(o, cap - (size_t)(o - out), L"PROMPT=%ls", prompt) + 1;
        *o = 0;
    }
    FreeEnvironmentStringsW(env);
    return out;
}

static void sess_start(sess *s)
{
    wchar_t exe[MAX_PATH] = L"", *line = NULL, *env = NULL;
    size_t cap = 0;
    wchar_t launch_dir[MAX_PATH]=L"";
    grid_size(&s->rows, &s->cols);
    if (!vt_make(s)) return;
    s->state = S_RUN;
    s->t_start = GetTickCount64();
    s->t_end = 0;
    s->exit_code = 0;
    s->eof = 0;
    s->marks = s->at_prompt = s->typed = 0;
    s->cwd_verified = 0;
    s->input_chars=s->input_enters=s->input_error=0;
    if (s->cmd) {
        cap = wcslen(s->cmd) + 1;
        line = _wcsdup(s->cmd);
        const wchar_t *p = s->cmd;
        wchar_t first[MAX_PATH];
        int k = 0;
        if (*p == L'"') {
            for (p++; *p && *p != L'"' && k < MAX_PATH - 1;) first[k++] = *p++;
        } else {
            while (*p && *p != L' ' && k < MAX_PATH - 1) first[k++] = *p++;
        }
        first[k] = 0;
        const wchar_t *base = wcsrchr(first, L'\\');
        _snwprintf(s->name, 64, L"%ls", base ? base + 1 : first);
        s->kind = term_shell_kind(first);
    } else {
        const wchar_t *want = s->shell[0] ? s->shell : L"auto";
        wchar_t distro[128],linux[MAX_PATH];
        int in_wsl=term_wsl_split(s->dir,distro,128,linux,MAX_PATH);
        int use_wsl=!_wcsicmp(want,L"wsl")||(!_wcsicmp(want,L"auto")&&in_wsl);
        if(use_wsl){
            wchar_t helper[MAX_PATH],arg[8192],distarg[160]=L"";
            GetModuleFileNameW(NULL,helper,MAX_PATH);wchar_t *slash=wcsrchr(helper,L'\\');if(slash)*slash=0;
            wcscpy(launch_dir,helper);
            wchar_t file[MAX_PATH];_snwprintf(file,MAX_PATH,L"%ls\\folderwing-wsl",helper);
            if(GetFileAttributesW(file)==INVALID_FILE_ATTRIBUTES || !term_wsl_arg(s->dir,arg,8192)){
                s->state=S_DONE;s->exit_code=ERROR_FILE_NOT_FOUND;return;
            }
            GetSystemDirectoryW(exe,MAX_PATH);wcscat(exe,L"\\wsl.exe");
            if(in_wsl)_snwprintf(distarg,160,L" --distribution %ls",distro);
            cap=9000;line=malloc(cap*sizeof(wchar_t));
            if(line)swprintf(line,cap,L"\"%ls\"%ls --cd \"%ls\" --exec ./folderwing-wsl --shell %ls",exe,distarg,helper,arg);
            s->kind=TERM_SH_WSL;_snwprintf(s->name,64,L"WSL%ls%ls",in_wsl?L" · ":L"",in_wsl?distro:L"");
        } else {
        if (!_wcsicmp(want, L"cmd")) {
            if (!GetEnvironmentVariableW(L"ComSpec", exe, MAX_PATH)) wcscpy(exe, L"C:\\Windows\\System32\\cmd.exe");
        } else if (!_wcsicmp(want, L"powershell")) {
            find_exe(L"powershell.exe", exe);
        } else if (!find_exe(L"pwsh.exe", exe)) {
            find_exe(L"powershell.exe", exe);
        }
        if (!exe[0]) {
            if (!GetEnvironmentVariableW(L"ComSpec", exe, MAX_PATH)) wcscpy(exe, L"C:\\Windows\\System32\\cmd.exe");
        }
        s->kind = term_shell_kind(exe);
        const wchar_t *base = wcsrchr(exe, L'\\');
        _snwprintf(s->name, 64, L"%ls", base ? base + 1 : exe);
        wchar_t *dot = wcsrchr(s->name, L'.');
        if (dot && !_wcsicmp(dot, L".exe")) *dot = 0;
        cap = MAX_PATH + 4096;
        line = malloc(cap * sizeof(wchar_t));
        if (line && s->kind == TERM_SH_PWSH) {
            int n = swprintf(line, cap, L"\"%ls\" -NoLogo -NoExit -EncodedCommand ", exe);
            if (n < 0 || !term_base64_utf16(term_pwsh_hook(), line + n, cap - (size_t)n)) swprintf(line, cap, L"\"%ls\" -NoLogo", exe);
        } else if (line) {
            swprintf(line, cap, L"\"%ls\"", exe);
            if (s->kind == TERM_SH_CMD) env = cmd_env();
        }
        } /* Windows shell */
    }
    wchar_t dir[MAX_PATH];
    wcscpy(dir,launch_dir[0]?launch_dir:s->dir);
    DWORD at = dir[0] ? GetFileAttributesW(dir) : INVALID_FILE_ATTRIBUTES;
    if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) {
        if (!GetEnvironmentVariableW(L"USERPROFILE", dir, MAX_PATH)) wcscpy(dir, L"C:\\");
    }
    wcscpy(s->cwd,launch_dir[0]?s->dir:dir);
    s->cwd_verified = s->kind != TERM_SH_WSL;

    HANDLE in_r = NULL, in_w = NULL, out_r = NULL, out_w = NULL;
    DWORD err = 0;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    PROCESS_INFORMATION pi = { 0 };
    if (!line || !CreatePipe(&in_r, &in_w, NULL, 0) || !CreatePipe(&out_r, &out_w, NULL, 0)) {
        err = GetLastError();
        goto fail;
    }
    HRESULT hr = CreatePseudoConsole((COORD){ (SHORT)s->cols, (SHORT)s->rows }, in_r, out_w, 0, &s->pc);
    if (FAILED(hr)) {
        err = (DWORD)hr;
        s->pc = NULL;
        goto fail;
    }
    SIZE_T asz = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &asz);
    attrs = malloc(asz);
    if (!attrs || !InitializeProcThreadAttributeList(attrs, 1, 0, &asz) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, s->pc, sizeof s->pc, NULL, NULL)) {
        err = GetLastError();
        goto fail;
    }
    STARTUPINFOEXW si = { { sizeof si } };
    /* no handles of ours: without this the program would get the panel's pipes */
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.lpAttributeList = attrs;
    if (!CreateProcessW(NULL, line, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, env, dir,
                        &si.StartupInfo, &pi)) {
        err = GetLastError();
        goto fail;
    }
    CloseHandle(pi.hThread);
    s->proc = pi.hProcess;
    s->in_w = in_w;
    in_w = NULL;
    out_arg *oa = malloc(sizeof *oa);
    HANDLE t = NULL;
    if (oa) {
        *oa = (out_arg){ out_r, A.hwnd, s->id };
        t = CreateThread(NULL, 64 * 1024, out_reader, oa, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    }
    if (t) {
        CloseHandle(t);
        out_r = NULL;
    } else {
        free(oa);
    }
    RegisterWaitForSingleObject(&s->wait, s->proc, on_exit_wait, (void *)(intptr_t)s->id, INFINITE, WT_EXECUTEONLYONCE);
    goto done;
fail: {
    wchar_t m[600];
    wchar_t why[256] = L"";
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0, why, 256, NULL);
    for (wchar_t *p = why; *p; p++)
        if (*p == L'\r' || *p == L'\n') *p = L' ';
    _snwprintf(m, 600, L"\x1b[31mCould not start %ls\x1b[m\r\n%ls\r\n", s->cmd ? s->cmd : exe, why);
    m[599] = 0;
    vt_say(s, m);
    sess_detach(s);
    s->state = S_DONE;
    s->exit_code = err ? err : 1;
    s->t_end = GetTickCount64();
    s->eof = 1;
}
done:
    if (attrs) DeleteProcThreadAttributeList(attrs), free(attrs);
    if (in_r) CloseHandle(in_r);
    if (out_w) CloseHandle(out_w);
    if (in_w) CloseHandle(in_w);
    if (out_r) CloseHandle(out_r);
    free(line);
    free(env);
}

/* A finished session goes to the history: its screen joins its scrollback and the terminal goes. */
static void archive(sess *s)
{
    if (s->vt) {
        int last = -1;
        tcell buf[512];
        for (int r = 0; r < s->rows; r++) {
            int n = line_cells(s, s->sb_n + r, buf, 512);
            for (int c = 0; c < n; c++)
                if (!is_blank(&buf[c])) last = r;
        }
        int nrow = last + 1;
        sline **keep = calloc((size_t)(nrow > 0 ? nrow : 1), sizeof *keep);
        for (int r = 0; r < nrow && keep; r++) {
            int n = line_cells(s, s->sb_n + r, buf, 512);
            keep[r] = line_make(buf, n);
        }
        vterm_free(s->vt);
        s->vt = NULL;
        s->scr = NULL;
        for (int r = 0; r < nrow && keep; r++)
            if (keep[r]) sb_push(s, keep[r]);
        free(keep);
    }
    sess_detach(s);
    s->scroll = 0;
    if (A.sel_s == s) A.sel_s = NULL, A.sel_on = 0;
    if (A.nhist == HIST_MAX || (A.nhist && A.nhist >= A.history)) sess_free(A.hist[--A.nhist]);
    if (A.history <= 0) {
        sess_free(s);
        return;
    }
    memmove(A.hist + 1, A.hist, (size_t)A.nhist * sizeof *A.hist);
    A.hist[0] = s;
    A.nhist++;
}

static sess *sess_new(const wchar_t *dir, const wchar_t *shell, const wchar_t *cmd, const wchar_t *title)
{
    if (A.ntabs == TABS_MAX) return NULL;
    sess *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->id = ++A.ids;
    _snwprintf(s->dir, MAX_PATH, L"%ls", dir && dir[0] ? dir : A.pane);
    _snwprintf(s->shell, 16, L"%ls", shell && shell[0] ? shell : A.shell);
    if (title) _snwprintf(s->title_req, 64, L"%ls", title);
    if (cmd && cmd[0]) s->cmd = _wcsdup(cmd);
    A.tabs[A.ntabs++] = s;
    sess_start(s);
    return s;
}

/* ---- the view -------------------------------------------------------------------------- */

static void select_none(void) { A.sel_on = A.selecting = 0, A.sel_s = NULL; }

static void tell_cwd(sess *s)
{
    if (s && A.visible && s->state == S_RUN && s->cwd_verified && s->cwd[0]) send_text(PANELP_TERM_CWD, s->cwd);
}

/* C:\x and c:\X\ are the same folder */
static int same_dir(const wchar_t *a, const wchar_t *b)
{
    wchar_t da[128],db[128],pa[2048],pb[2048];
    int wa=term_wsl_split(a,da,128,pa,2048),wb=term_wsl_split(b,db,128,pb,2048);
    if(wa||wb){
        if(!wa||!wb||_wcsicmp(da,db))return 0;
        size_t na=wcslen(pa),nb=wcslen(pb);
        if(na>1&&pa[na-1]==L'/')pa[--na]=0;
        if(nb>1&&pb[nb-1]==L'/')pb[--nb]=0;
        return !wcscmp(pa,pb);
    }
    size_t na = wcslen(a), nb = wcslen(b);
    if (na > 3 && a[na - 1] == L'\\') na--;
    if (nb > 3 && b[nb - 1] == L'\\') nb--;
    return na == nb && !_wcsnicmp(a, b, na);
}

static void cwd_changed(sess *s)
{
    if (s->cd_sent[0] && same_dir(s->cwd, s->cd_sent)) {
        s->cd_sent[0] = 0;
        return;
    }
    if (A.want[0]) return; /* the pane is on its way elsewhere: it leads */
    if (s == cur_sess()) tell_cwd(s);
}

/* The shell shown goes where the pane is, if it's idle at its prompt and nothing is typed. */
static void follow_pane(sess *s)
{
    if (!A.follow || !s || s != cur_sess() || !A.visible || s->state != S_RUN || !s->marks || !s->at_prompt || s->typed ||
        !A.want[0])
        return;
    if (same_dir(A.want, s->cwd)) {
        A.want[0] = 0;
        return;
    }
    wchar_t cd[8192];
    if (!term_cd_command(s->kind, s->cwd, A.want, cd, 8192)) {
        A.want[0] = 0;
        return;
    }
    wcscpy(s->cd_sent, A.want);
    A.want[0] = 0;
    char buf[8192 * 3];
    int n = WideCharToMultiByte(CP_UTF8, 0, cd, -1, buf, sizeof buf - 2, NULL, NULL);
    if (n <= 1) return;
    buf[n - 1] = '\r';
    s->at_prompt = 0;
    s->scroll = 0;
    write_pty(s, buf, (size_t)n);
}

/* a done tab that is left goes to the history */
static void leave_current(void)
{
    if (A.view != V_TAB || A.cur < 0 || A.cur >= A.ntabs) return;
    sess *s = A.tabs[A.cur];
    if (s->state != S_DONE) return;
    memmove(A.tabs + A.cur, A.tabs + A.cur + 1, (size_t)(A.ntabs - A.cur - 1) * sizeof *A.tabs);
    A.ntabs--;
    archive(s);
    if (A.cur >= A.ntabs) A.cur = A.ntabs - 1;
}

static void show_tab(int i)
{
    if (A.view == V_TAB && i == A.cur) return;
    sess *target = i >= 0 && i < A.ntabs ? A.tabs[i] : NULL;
    leave_current();
    A.view = V_TAB;
    A.cur = -1;
    for (int k = 0; k < A.ntabs; k++)
        if (A.tabs[k] == target) A.cur = k;
    if (A.cur < 0 && A.ntabs) A.cur = A.ntabs - 1;
    select_none();
    sess *s = cur_sess();
    if (s) {
        sess_resize(s);
        tell_cwd(s);
        follow_pane(s);
    }
    A.dirty = 1;
}

static void close_tab(int i)
{
    if (i < 0 || i >= A.ntabs) return;
    sess *s = A.tabs[i];
    memmove(A.tabs + i, A.tabs + i + 1, (size_t)(A.ntabs - i - 1) * sizeof *A.tabs);
    A.ntabs--;
    if (s->state == S_DONE) archive(s);
    else sess_free(s); /* still running: closing it ends it, nothing to keep */
    if (A.cur >= A.ntabs) A.cur = A.ntabs - 1;
    else if (i < A.cur) A.cur--;
    select_none();
    if (cur_sess()) sess_resize(cur_sess()), tell_cwd(cur_sess());
    A.dirty = 1;
}

static void new_tab(const wchar_t *dir, const wchar_t *shell, const wchar_t *cmd, const wchar_t *title)
{
    leave_current();
    sess *s = sess_new(dir, shell, cmd, title);
    A.view = V_TAB;
    if (s) A.cur = A.ntabs - 1;
    select_none();
    A.dirty = 1;
}

static void run_again(sess *s)
{
    wchar_t dir[MAX_PATH], shell[16], title[64];
    wchar_t *cmd = s->cmd ? _wcsdup(s->cmd) : NULL;
    wcscpy(dir, s->dir);
    wcscpy(shell, s->shell);
    wcscpy(title, s->title_req);
    if (A.view == V_HISTITEM) A.view = V_TAB;
    new_tab(dir, shell, cmd, title);
    free(cmd);
}

static void show_history(void)
{
    leave_current();
    A.view = V_HISTLIST;
    A.hsel = A.nhist ? 0 : -1;
    A.hscroll = 0;
    select_none();
    A.dirty = 1;
}

/* ---- drawing ---------------------------------------------------------------------------- */

static void fill(int x, int y, int w, int h, COLORREF c)
{
    RECT r = { x, y, x + w, y + h };
    SetDCBrushColor(A.mdc, c);
    FillRect(A.mdc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

static void text(HFONT f, const wchar_t *s, RECT r, COLORREF c, UINT fmt)
{
    SelectObject(A.mdc, f);
    SetTextColor(A.mdc, c);
    SetBkMode(A.mdc, TRANSPARENT);
    DrawTextW(A.mdc, s, -1, &r, fmt | DT_NOPREFIX);
}

static int text_w(HFONT f, const wchar_t *s)
{
    SIZE sz;
    SelectObject(A.mdc, f);
    GetTextExtentPoint32W(A.mdc, s, (int)wcslen(s), &sz);
    return sz.cx;
}

static void add_hot(int x, int y, int w, int h, int kind, int idx)
{
    if (A.nhot < HOTS_MAX) A.hots[A.nhot++] = (hot){ { x, y, x + w, y + h }, kind, idx };
}

static int hovered(int kind, int idx) { return A.hover_kind == kind && A.hover_idx == idx; }

static int button(int x, int y, int h, const wchar_t *label, int kind, int idx, int primary)
{
    int w = text_w(A.ui, label) + S(20);
    fill(x, y, w, h, hovered(kind, idx) ? (primary ? C_CURSOR : C_SEL_IDLE) : (primary ? C_SEL : C_EDIT));
    RECT r = { x, y, x + w, y + h };
    text(A.ui, label, r, C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    add_hot(x, y, w, h, kind, idx);
    return w;
}

static void tab_label(const sess *s, wchar_t *out, size_t cap)
{
    const wchar_t *t = s->title_req[0] ? s->title_req : s->title;
    /* a title that's just the program's path says less than its name and folder */
    int plain = !t[0] || wcschr(t, L'\\') || wcsstr(t, L".exe");
    if (plain) {
        const wchar_t *leaf = wcsrchr(s->cwd, L'\\');
        leaf = leaf && leaf[1] ? leaf + 1 : s->cwd;
        if (s->cmd || !leaf[0]) _snwprintf(out, cap, L"%ls", s->name);
        else _snwprintf(out, cap, L"%ls \x00B7 %ls", s->name, leaf);
    } else {
        _snwprintf(out, cap, L"%ls", t);
    }
    out[cap - 1] = 0;
}

static void paint_tabs(void)
{
    int th = tabbar_h(), x = S(4);
    fill(0, 0, A.w, th, C_HEADER);
    int right = A.w - S(4);
    if (A.nhist || A.view != V_TAB) {
        wchar_t hl[32];
        _snwprintf(hl, 32, L"History %d", A.nhist);
        int w = text_w(A.ui_small, hl) + S(16);
        right -= w;
        if (A.view == V_HISTLIST || A.view == V_HISTITEM) fill(right, 0, w, th, C_BG);
        else if (hovered(H_HIST, 0)) fill(right, 0, w, th, C_SEL_IDLE);
        RECT r = { right, 0, right + w, th };
        text(A.ui_small, hl, r, A.view != V_TAB ? C_TEXT : C_DIM, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        add_hot(right, 0, w, th, H_HIST, 0);
        right -= S(4);
    }
    int room = right - x - S(30), each = A.ntabs ? room / A.ntabs : 0;
    if (each > S(180)) each = S(180);
    for (int i = 0; i < A.ntabs; i++) {
        sess *s = A.tabs[i];
        int on = A.view == V_TAB && i == A.cur, w = each;
        if (w < S(40)) break;
        fill(x, 0, w, th, on ? C_BG : hovered(H_TAB, i) ? C_SEL_IDLE : C_HEADER);
        if (on) fill(x, th - S(2), w, S(2), C_ACCENT);
        wchar_t label[160], full[170];
        tab_label(s, label, 160);
        if (s->state == S_DONE) _snwprintf(full, 170, L"%ls %ls", s->exit_code ? L"\x2717" : L"\x2713", label);
        else _snwprintf(full, 170, L"%ls", label);
        full[169] = 0;
        int xw = on || hovered(H_TAB, i) || hovered(H_TABX, i) ? S(20) : 0;
        RECT r = { x + S(10), 0, x + w - S(6) - xw, th };
        text(A.ui_small, full, r, on ? C_TEXT : s->state == S_DONE ? C_DIM : RGB(190, 190, 195),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        add_hot(x, 0, w, th, H_TAB, i);
        if (xw) {
            RECT xr = { x + w - xw - S(4), S(4), x + w - S(4), th - S(4) };
            if (hovered(H_TABX, i)) fill(xr.left, xr.top, xr.right - xr.left, xr.bottom - xr.top, C_SEL_IDLE);
            text(A.ui_small, L"\x2715", xr, C_DIM, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            /* later hots win the hit test */
            add_hot(xr.left, xr.top, xr.right - xr.left, xr.bottom - xr.top, H_TABX, i);
        }
        x += w + S(1);
    }
    RECT nr = { x + S(2), S(3), x + S(28), th - S(3) };
    if (hovered(H_NEW, 0)) fill(nr.left, nr.top, nr.right - nr.left, nr.bottom - nr.top, C_SEL_IDLE);
    text(A.ui, L"+", nr, C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    add_hot(nr.left, nr.top, nr.right - nr.left, nr.bottom - nr.top, H_NEW, 0);
}

/* selection on line `al` (absolute) as [c0, c1), or 0 */
static int sel_cols(const sess *s, int64_t al, int *c0, int *c1)
{
    if (!A.sel_on || A.sel_s != s) return 0;
    int64_t l0 = A.sel_al, l1 = A.sel_bl;
    int k0 = A.sel_ac, k1 = A.sel_bc;
    if (l1 < l0 || (l1 == l0 && k1 < k0)) {
        int64_t t = l0;
        l0 = l1, l1 = t;
        int u = k0;
        k0 = k1, k1 = u;
    }
    if (al < l0 || al > l1) return 0;
    *c0 = al == l0 ? k0 : 0;
    *c1 = al == l1 ? k1 : 1 << 20;
    return *c1 > *c0;
}

static int is_block(uint32_t ch) { return ch >= 0x2580 && ch <= 0x259F; }

static COLORREF mix(COLORREF a, COLORREF b, int pct)
{
    return RGB((GetRValue(a) * pct + GetRValue(b) * (100 - pct)) / 100, (GetGValue(a) * pct + GetGValue(b) * (100 - pct)) / 100,
               (GetBValue(a) * pct + GetBValue(b) * (100 - pct)) / 100);
}

/* Block elements drawn as shapes: the font's glyphs leave seams between cells. */
static void draw_block(uint32_t ch, int x, int y, int w, COLORREF fg, COLORREF bg)
{
    int h = A.chh;
    static const uint8_t quads[10] = { 4, 8, 1, 1 | 4 | 8, 1 | 8, 1 | 2 | 4, 1 | 2 | 8, 2, 2 | 4, 2 | 4 | 8 };
    if (ch == 0x2580) fill(x, y, w, h / 2, fg);
    else if (ch >= 0x2581 && ch <= 0x2588) {
        int eh = h * (int)(ch - 0x2580) / 8;
        fill(x, y + h - eh, w, eh, fg);
    } else if (ch >= 0x2589 && ch <= 0x258F) {
        fill(x, y, w * (int)(0x2590 - ch) / 8, h, fg);
    } else if (ch == 0x2590) fill(x + w / 2, y, w - w / 2, h, fg);
    else if (ch >= 0x2591 && ch <= 0x2593) fill(x, y, w, h, mix(fg, bg, 25 * (int)(ch - 0x2590)));
    else if (ch == 0x2594) fill(x, y, w, h / 8 > 0 ? h / 8 : 1, fg);
    else if (ch == 0x2595) fill(x + w - (w / 8 > 0 ? w / 8 : 1), y, w / 8 > 0 ? w / 8 : 1, h, fg);
    else {
        int q = quads[ch - 0x2596], hw = w / 2, hh = h / 2;
        if (q & 1) fill(x, y, hw, hh, fg);
        if (q & 2) fill(x + hw, y, w - hw, hh, fg);
        if (q & 4) fill(x, y + hh, hw, h - hh, fg);
        if (q & 8) fill(x + hw, y + hh, w - hw, h - hh, fg);
    }
}

static void draw_line(const tcell *c, int n, int x0, int y, int s0, int s1, int cols)
{
    wchar_t buf[1100];
    INT dx[1100];
    int i = 0;
    while (i < cols) {
        tcell a = i < n ? c[i] : (tcell){ ' ', C_TEXT, C_BG };
        int sel = i >= s0 && i < s1;
        uint32_t fg = sel ? C_TEXT : a.fg, bg = sel ? C_SEL : a.bg, style = a.ch & (AT_BOLD | AT_ITALIC | AT_UNDER | AT_STRIKE);
        int k = 0, j = i;
        while (j < cols && k < 1090) {
            tcell b = j < n ? c[j] : (tcell){ ' ', C_TEXT, C_BG };
            int bs = j >= s0 && j < s1;
            uint32_t bfg = bs ? C_TEXT : b.fg, bbg = bs ? C_SEL : b.bg;
            if (b.ch & AT_SKIP) {
                if (k) dx[k - 1] += A.cw;
                else buf[k] = L' ', dx[k++] = A.cw;
                j++;
                continue;
            }
            if (bfg != fg || bbg != bg || (b.ch & (AT_BOLD | AT_ITALIC | AT_UNDER | AT_STRIKE)) != style) break;
            uint32_t ch = b.ch & CH_MASK;
            if (!ch || is_block(ch)) ch = ' ';
            if (ch >= 0x10000) {
                ch -= 0x10000;
                buf[k] = (wchar_t)(0xD800 + (ch >> 10)), dx[k++] = A.cw;
                buf[k] = (wchar_t)(0xDC00 + (ch & 0x3FF)), dx[k++] = 0;
            } else {
                buf[k] = (wchar_t)ch, dx[k++] = A.cw;
            }
            j++;
        }
        if (j == i) j++; /* never stuck */
        int font = (style & AT_BOLD ? 1 : 0) | (style & AT_ITALIC ? 2 : 0);
        SelectObject(A.mdc, A.mono[font]);
        SetTextColor(A.mdc, fg);
        SetBkColor(A.mdc, bg);
        RECT r = { x0 + i * A.cw, y, x0 + j * A.cw, y + A.chh };
        ExtTextOutW(A.mdc, r.left, y, ETO_OPAQUE | ETO_CLIPPED, &r, buf, (UINT)k, dx);
        if (style & AT_UNDER) fill(r.left, y + A.base + S(1), r.right - r.left, S(1) > 0 ? S(1) : 1, fg);
        if (style & AT_STRIKE) fill(r.left, y + A.chh / 2, r.right - r.left, S(1) > 0 ? S(1) : 1, fg);
        for (int q = i; q < j && q < n; q++)
            if (is_block(c[q].ch & CH_MASK)) draw_block(c[q].ch & CH_MASK, x0 + q * A.cw, y, A.cw, fg, bg);
        i = j;
    }
}

static int footer_lines(void) { return (footer_h() + A.chh - 1) / A.chh; }

/* Lines to show: a finished session's end where its output ends, then room for the footer. */
static int shown_total(sess *s)
{
    if (s->state != S_DONE) return total_lines(s);
    int end = s->sb_n;
    if (s->vt) {
        tcell buf[512];
        VTermPos p;
        vterm_state_get_cursorpos(vterm_obtain_state(s->vt), &p);
        int last = p.col ? p.row : p.row - 1;
        for (int r = last + 1; r < s->rows; r++) {
            int n = line_cells(s, s->sb_n + r, buf, 512);
            for (int c = 0; c < n; c++)
                if (!is_blank(&buf[c])) last = r;
        }
        end += last + 1;
    }
    return end + footer_lines();
}

/* the first line shown */
static int view_first(sess *s, int rows)
{
    int first = shown_total(s) - rows - s->scroll;
    return first < 0 ? 0 : first;
}

static int view_rows(void) { return (A.h - tabbar_h() - S(4)) / (A.chh ? A.chh : 16); }

static void paint_session(sess *s)
{
    int top = tabbar_h() + S(2), x0 = pad_x(), rows = view_rows();
    int done = s->state == S_DONE;
    int cols = s->vt ? s->cols : (A.w - 2 * pad_x() - S(6)) / A.cw;
    int first = view_first(s, rows);
    tcell buf[512];
    for (int r = 0; r < rows; r++) {
        int li = first + r, n = li < total_lines(s) ? line_cells(s, li, buf, 512) : 0;
        int s0 = 0, s1 = 0;
        if (!sel_cols(s, s->dropped + li, &s0, &s1)) s0 = s1 = 0;
        draw_line(buf, n, x0, top + r * A.chh, s0, s1, cols);
    }
    /* the cursor, where the program is */
    if (s->vt && s->state == S_RUN && s->cursor_on) {
        VTermPos p;
        vterm_state_get_cursorpos(vterm_obtain_state(s->vt), &p);
        int li = s->sb_n + p.row, r = li - first;
        if (r >= 0 && r < rows && p.col < cols) {
            int x = x0 + p.col * A.cw, y = top + r * A.chh;
            if (A.focused) {
                fill(x, y, A.cw, A.chh, C_CURSOR);
                tcell c;
                if (line_cells(s, li, buf, 512) > p.col) {
                    c = buf[p.col];
                    c.fg = C_BG;
                    c.bg = C_CURSOR;
                    draw_line(&c, 1, x, y, 0, 0, 1);
                }
            } else {
                fill(x, y, A.cw, 1, C_CURSOR);
                fill(x, y + A.chh - 1, A.cw, 1, C_CURSOR);
                fill(x, y, 1, A.chh, C_CURSOR);
                fill(x + A.cw - 1, y, 1, A.chh, C_CURSOR);
            }
            panelp_rect cr = { x, y, A.cw, A.chh };
            send_msg(PANELP_CARET, &cr, sizeof cr);
        }
    }
    /* scrolled back: where the view is */
    int total = shown_total(s);
    if (total > rows && s->scroll) {
        int vh = rows * A.chh, th = vh * rows / total;
        if (th < S(20)) th = S(20);
        int ty = top + (int)((int64_t)(vh - th) * first / (total - rows));
        fill(A.w - S(5), ty, S(3), th, C_BORDER);
    }
    if (done) {
        int fh = footer_h(), fy = A.h - fh;
        fill(0, fy, A.w, fh, C_HEADER);
        fill(0, fy, A.w, S(1), C_BORDER);
        double secs = (double)(s->t_end - s->t_start) / 1000.0;
        wchar_t what[160];
        if (s->exit_code >= 0xC0000000u) _snwprintf(what, 160, L"Ended: 0x%08lX after %.1f s", s->exit_code, secs);
        else _snwprintf(what, 160, L"Ended: exit %lu after %.1f s", s->exit_code, secs);
        int bx = A.w - S(8), bh = fh - S(10);
        int cw = text_w(A.ui, A.view == V_HISTITEM ? L"Back" : L"Close") + S(20);
        bx -= cw;
        button(bx, fy + S(5), bh, A.view == V_HISTITEM ? L"Back" : L"Close", A.view == V_HISTITEM ? H_BACK : H_CLOSE, 0, 0);
        int rw = text_w(A.ui, L"Run again") + S(20);
        bx -= rw + S(6);
        button(bx, fy + S(5), bh, L"Run again", H_RUN, 0, 1);
        RECT r = { S(10), fy, bx - S(8), A.h };
        text(A.ui_small, what, r, s->exit_code ? C_ERROR : C_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

static void ago(uint64_t t, wchar_t *out, size_t cap)
{
    uint64_t s = (GetTickCount64() - t) / 1000;
    if (s < 60) _snwprintf(out, cap, L"just now");
    else if (s < 3600) _snwprintf(out, cap, L"%llu min ago", s / 60);
    else _snwprintf(out, cap, L"%llu h ago", s / 3600);
}

static int hist_row_h(void) { return S(46); }

static void paint_history(void)
{
    int top = tabbar_h(), y = top + S(8), p = S(12);
    RECT hr = { p, y, A.w - p, y + S(22) };
    text(A.ui, A.nhist ? L"Finished sessions, newest first. Enter or click to read one." : L"No finished sessions yet.", hr, C_DIM,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    y += S(30);
    int rh = hist_row_h();
    for (int i = A.hscroll; i < A.nhist && y + rh <= A.h; i++, y += rh) {
        sess *s = A.hist[i];
        if (i == A.hsel) fill(S(4), y, A.w - S(8), rh - S(2), C_SEL);
        else if (hovered(H_HITEM, i)) fill(S(4), y, A.w - S(8), rh - S(2), C_SEL_IDLE);
        add_hot(S(4), y, A.w - S(8), rh - S(2), H_HITEM, i);
        wchar_t l1[300], l2[400], when[32], label[160];
        tab_label(s, label, 160);
        _snwprintf(l1, 300, L"%ls  %ls", s->exit_code ? L"\x2717" : L"\x2713", s->cmd ? s->cmd : label);
        ago(s->t_end, when, 32);
        _snwprintf(l2, 400, L"%ls \x00B7 exit %lu \x00B7 %ls \x00B7 %d lines", s->dir, s->exit_code, when, s->sb_n);
        l1[299] = l2[399] = 0;
        RECT r1 = { p, y + S(4), A.w - p, y + S(22) }, r2 = { p, y + S(22), A.w - p, y + S(40) };
        text(A.ui, l1, r1, s->exit_code ? C_ERROR : C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        text(A.ui_small, l2, r2, C_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    }
}

static void paint_empty(void)
{
    int y = tabbar_h() + S(40), p = S(16);
    RECT r = { p, y, A.w - p, y + S(24) };
    text(A.ui, L"No terminal open", r, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y += S(32);
    wchar_t l[MAX_PATH + 32];
    _snwprintf(l, MAX_PATH + 32, L"New terminal (Enter)");
    button(p, y, S(28), l, H_EMPTYNEW, 0, 1);
    y += S(36);
    if (A.pane[0]) {
        RECT d = { p, y, A.w - p, y + S(20) };
        _snwprintf(l, MAX_PATH + 32, L"in %ls", A.pane);
        l[MAX_PATH + 31] = 0;
        text(A.ui_small, l, d, C_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    }
}

static void present(void)
{
    if (!A.shared || !A.back_bits) return;
    if (A.waiting) {
        A.pending = 1;
        return;
    }
    size_t one = (size_t)A.w * A.h * 4;
    memcpy(A.shared + one * A.next, A.back_bits, one);
    panelp_frame f = { (uint32_t)A.next, A.seq, { 0, 0, A.w, A.h } };
    send_msg(PANELP_FRAME, &f, sizeof f);
    A.next ^= 1;
    A.waiting = 1;
    A.pending = 0;
}

static void paint(void)
{
    if (!A.mdc || !A.back) return;
    A.dirty = 0;
    A.last_paint = GetTickCount64();
    A.nhot = 0;
    fill(0, 0, A.w, A.h, C_BG);
    sess *s = cur_sess();
    if (A.view == V_HISTLIST) paint_history();
    else if (s) paint_session(s);
    else paint_empty();
    paint_tabs();
    GdiFlush();
    present();
}

/* ---- fonts and the shared pictures ------------------------------------------------------- */

static void make_fonts(void)
{
    for (int i = 0; i < 4; i++)
        if (A.mono[i]) DeleteObject(A.mono[i]);
    if (A.ui) DeleteObject(A.ui);
    if (A.ui_small) DeleteObject(A.ui_small);
    int px = -MulDiv(A.pt * 10, A.dpi, 720);
    const wchar_t *face = A.font[0] ? A.font : L"Cascadia Mono";
    for (int i = 0; i < 4; i++)
        A.mono[i] = CreateFontW(px, 0, 0, 0, i & 1 ? FW_BOLD : FW_NORMAL, (i & 2) != 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, face);
    HDC dc = A.mdc ? A.mdc : GetDC(NULL);
    HGDIOBJ old = SelectObject(dc, A.mono[0]);
    wchar_t got[64];
    GetTextFaceW(dc, 64, got);
    if (_wcsicmp(got, face)) { /* not installed: Consolas is */
        SelectObject(dc, old);
        for (int i = 0; i < 4; i++) {
            DeleteObject(A.mono[i]);
            A.mono[i] = CreateFontW(px, 0, 0, 0, i & 1 ? FW_BOLD : FW_NORMAL, (i & 2) != 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        }
        SelectObject(dc, A.mono[0]);
    }
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SIZE sz;
    GetTextExtentPoint32W(dc, L"MMMMMMMMMM", 10, &sz);
    A.cw = sz.cx / 10 > 0 ? sz.cx / 10 : 8;
    A.chh = tm.tmHeight > 0 ? tm.tmHeight : 16;
    A.base = tm.tmAscent;
    SelectObject(dc, old);
    if (dc != A.mdc) ReleaseDC(NULL, dc);
    A.ui = CreateFontW(-MulDiv(90, A.dpi, 720), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    A.ui_small = CreateFontW(-MulDiv(85, A.dpi, 720), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

static void resize_all(void)
{
    for (int i = 0; i < A.ntabs; i++) sess_resize(A.tabs[i]);
}

static int pixels_map(HANDLE section, int w, int h)
{
    if (A.shared) UnmapViewOfFile(A.shared);
    if (A.section) CloseHandle(A.section);
    A.section = section;
    A.shared = section ? MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 0) : NULL;
    A.w = w;
    A.h = h;
    if (A.back) DeleteObject(A.back);
    if (!A.mdc) A.mdc = CreateCompatibleDC(NULL);
    BITMAPINFO bi = { { sizeof bi.bmiHeader, w, -h, 1, 32, BI_RGB } };
    A.back = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &A.back_bits, NULL, 0);
    if (A.back) SelectObject(A.mdc, A.back);
    A.next = 0;
    A.waiting = 0;
    resize_all();
    return A.shared && A.back;
}

/* ---- clipboard -------------------------------------------------------------------------- */

static void copy_selection(void)
{
    sess *s = A.sel_s;
    if (!s || !A.sel_on) return;
    int64_t l0 = A.sel_al < A.sel_bl ? A.sel_al : A.sel_bl, l1 = A.sel_al < A.sel_bl ? A.sel_bl : A.sel_al;
    /* A selection can outlive dropped scrollback. Clip before subtracting or
     * sizing, and allow two UTF-16 units per cell plus CRLF and the final NUL. */
    int lines = total_lines(s);
    if (lines <= 0 || s->dropped < 0 || s->dropped > INT64_MAX - (lines - 1)) return;
    int64_t last = s->dropped + (lines - 1);
    if (l0 < s->dropped) l0 = s->dropped;
    if (l1 > last) l1 = last;
    if (l0 > l1) return;
    enum { COPY_CELLS = 512, LINE_UNITS = COPY_CELLS * 2 + 2 };
    size_t rows = (size_t)(l1 - l0) + 1;
    if (rows > (SIZE_MAX / sizeof(wchar_t) - 1) / LINE_UNITS) return;
    size_t cap = rows * LINE_UNITS + 1, k = 0;
    wchar_t *out = malloc(cap * sizeof(wchar_t));
    if (!out) return;
    tcell buf[COPY_CELLS];
    for (size_t row = 0; row < rows; row++) {
        int64_t al = l0 + (int64_t)row;
        int c0, c1, li = (int)(al - s->dropped);
        if (!sel_cols(s, al, &c0, &c1)) continue;
        int n = line_cells(s, li, buf, COPY_CELLS);
        size_t start = k;
        for (int c = c0 < 0 ? 0 : c0; c < c1 && c < n; c++) {
            if (buf[c].ch & AT_SKIP) continue;
            uint32_t ch = buf[c].ch & CH_MASK;
            if (!ch) ch = ' ';
            if (ch >= 0x10000) {
                ch -= 0x10000;
                out[k++] = (wchar_t)(0xD800 + (ch >> 10));
                out[k++] = (wchar_t)(0xDC00 + (ch & 0x3FF));
            } else {
                out[k++] = (wchar_t)ch;
            }
        }
        while (k > start && out[k - 1] == L' ') k--;
        if (al < l1) out[k++] = L'\r', out[k++] = L'\n';
    }
    out[k] = 0;
    /* Prepare the complete object before replacing the clipboard. Allocation
     * or lock failure must leave the previous clipboard contents alone. */
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (k + 1) * sizeof(wchar_t));
    wchar_t *text = g ? GlobalLock(g) : NULL;
    if (text) {
        memcpy(text, out, (k + 1) * sizeof(wchar_t));
        GlobalUnlock(g);
        if (OpenClipboard(A.hwnd)) {
            if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, g)) g = NULL;
            CloseClipboard();
        }
    }
    if (g) GlobalFree(g);
    free(out);
}

static void paste(sess *s)
{
    if (!s || s->state != S_RUN || !OpenClipboard(A.hwnd)) return;
    HANDLE g = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *t = g ? GlobalLock(g) : NULL;
    if (t) {
        size_t n = wcslen(t);
        wchar_t *w = malloc((n + 1) * sizeof(wchar_t));
        size_t k = 0;
        for (size_t i = 0; w && i < n; i++) {
            if (t[i] == L'\r' && t[i + 1] == L'\n') continue; /* a newline is Enter: one \r */
            w[k++] = t[i] == L'\n' ? L'\r' : t[i];
        }
        if (w) {
            int bytes = WideCharToMultiByte(CP_UTF8, 0, w, (int)k, NULL, 0, NULL, NULL);
            char *b = bytes > 0 ? malloc((size_t)bytes) : NULL;
            if (b) {
                WideCharToMultiByte(CP_UTF8, 0, w, (int)k, b, bytes, NULL, NULL);
                vterm_keyboard_start_paste(s->vt);
                write_pty(s, b, (size_t)bytes);
                vterm_keyboard_end_paste(s->vt);
                free(b);
            }
            free(w);
        }
        GlobalUnlock(g);
    }
    CloseClipboard();
    s->typed = 1;
    s->scroll = 0;
}

/* ---- input ---------------------------------------------------------------------------- */

static VTermModifier vmods(uint32_t m)
{
    return (VTermModifier)((m & PANELP_M_CTRL ? VTERM_MOD_CTRL : 0) | (m & PANELP_M_SHIFT ? VTERM_MOD_SHIFT : 0) |
                           (m & PANELP_M_ALT ? VTERM_MOD_ALT : 0));
}

static void typed_into(sess *s, int enter)
{
    s->scroll = 0;
    if (A.sel_s == s) select_none();
    if (enter) s->at_prompt = 0, s->typed = 0;
    else if (s->at_prompt) s->typed = 1;
}

static int scroll_by(sess *s, int lines)
{
    int max = shown_total(s) - view_rows();
    if (max < 0) max = 0;
    int was = s->scroll;
    s->scroll += lines;
    if (s->scroll > max) s->scroll = max;
    if (s->scroll < 0) s->scroll = 0;
    return s->scroll != was;
}

static void hist_key(UINT vk)
{
    int page = (A.h - tabbar_h() - S(38)) / hist_row_h();
    if (vk == VK_ESCAPE) show_tab(A.cur >= 0 ? A.cur : A.ntabs - 1);
    else if (vk == VK_UP && A.hsel > 0) A.hsel--;
    else if (vk == VK_DOWN && A.hsel + 1 < A.nhist) A.hsel++;
    else if (vk == VK_HOME) A.hsel = A.nhist ? 0 : -1;
    else if (vk == VK_END) A.hsel = A.nhist - 1;
    else if (vk == VK_RETURN && A.hsel >= 0) A.view = V_HISTITEM;
    else if (vk == VK_DELETE && A.hsel >= 0 && A.hsel < A.nhist) {
        sess_free(A.hist[A.hsel]);
        memmove(A.hist + A.hsel, A.hist + A.hsel + 1, (size_t)(A.nhist - A.hsel - 1) * sizeof *A.hist);
        A.nhist--;
        if (A.hsel >= A.nhist) A.hsel = A.nhist - 1;
    }
    if (A.hsel >= 0 && A.hsel < A.hscroll) A.hscroll = A.hsel;
    if (page > 0 && A.hsel >= A.hscroll + page) A.hscroll = A.hsel - page + 1;
    A.dirty = 1;
}

static void key(UINT vk, uint32_t mods)
{
    int ctrl = (mods & PANELP_M_CTRL) != 0, shift = (mods & PANELP_M_SHIFT) != 0, alt = (mods & PANELP_M_ALT) != 0;
    sess *s = cur_sess();
    A.eat_char = 1;
    /* the panel's own keys */
    if (ctrl && shift && (vk == 'T' || vk == 'N')) {
        new_tab(s && s->cwd[0] && s->state == S_RUN ? s->cwd : A.pane, NULL, NULL, NULL);
        return;
    }
    if (ctrl && shift && vk == 'W') {
        if (A.view == V_TAB) close_tab(A.cur);
        return;
    }
    if (ctrl && (vk == VK_TAB || vk == VK_NEXT || vk == VK_PRIOR) && A.ntabs && !(vk != VK_TAB && shift)) {
        int back = vk == VK_PRIOR || (vk == VK_TAB && shift);
        int base = A.view == V_TAB && A.cur >= 0 ? A.cur : back ? 0 : -1;
        show_tab((base + (back ? -1 : 1) + A.ntabs) % A.ntabs);
        return;
    }
    if (ctrl && shift && vk == 'H') {
        if (A.view == V_HISTLIST) show_tab(A.cur);
        else show_history();
        return;
    }
    if (A.view == V_HISTLIST) {
        hist_key(vk);
        return;
    }
    if (!s) {
        if (vk == VK_RETURN) new_tab(A.pane, NULL, NULL, NULL);
        return;
    }
    if (ctrl && shift && vk == 'C') {
        copy_selection();
        return;
    }
    if (ctrl && !shift && vk == 'C' && A.sel_on && A.sel_s == s) {
        copy_selection();
        select_none();
        A.dirty = 1;
        return;
    }
    if ((ctrl && vk == 'V') || (shift && vk == VK_INSERT)) {
        paste(s);
        return;
    }
    if (shift && !ctrl && (vk == VK_PRIOR || vk == VK_NEXT)) {
        if (scroll_by(s, (vk == VK_PRIOR ? 1 : -1) * (view_rows() - 1))) A.dirty = 1;
        return;
    }
    if (ctrl && shift && (vk == VK_UP || vk == VK_DOWN || vk == VK_HOME || vk == VK_END)) {
        int d = vk == VK_UP ? 1 : vk == VK_DOWN ? -1 : vk == VK_HOME ? 1 << 30 : -(1 << 30);
        if (scroll_by(s, d)) A.dirty = 1;
        return;
    }
    if (s->state == S_DONE) {
        if (vk == VK_RETURN) run_again(s);
        else if (vk == VK_ESCAPE) {
            if (A.view == V_HISTITEM) A.view = V_HISTLIST, A.dirty = 1;
            else close_tab(A.cur);
        } else if (vk == VK_PRIOR || vk == VK_NEXT) {
            if (scroll_by(s, (vk == VK_PRIOR ? 1 : -1) * (view_rows() - 1))) A.dirty = 1;
        } else if (vk == VK_UP || vk == VK_DOWN) {
            if (scroll_by(s, vk == VK_UP ? 1 : -1)) A.dirty = 1;
        }
        return;
    }
    VTermKey k = VTERM_KEY_NONE;
    switch (vk) {
    case VK_RETURN: k = VTERM_KEY_ENTER; break;
    case VK_TAB: k = VTERM_KEY_TAB; break;
    case VK_BACK: k = VTERM_KEY_BACKSPACE; break;
    case VK_ESCAPE: k = VTERM_KEY_ESCAPE; break;
    case VK_UP: k = VTERM_KEY_UP; break;
    case VK_DOWN: k = VTERM_KEY_DOWN; break;
    case VK_LEFT: k = VTERM_KEY_LEFT; break;
    case VK_RIGHT: k = VTERM_KEY_RIGHT; break;
    case VK_INSERT: k = VTERM_KEY_INS; break;
    case VK_DELETE: k = VTERM_KEY_DEL; break;
    case VK_HOME: k = VTERM_KEY_HOME; break;
    case VK_END: k = VTERM_KEY_END; break;
    case VK_PRIOR: k = VTERM_KEY_PAGEUP; break;
    case VK_NEXT: k = VTERM_KEY_PAGEDOWN; break;
    default:
        if (vk >= VK_F1 && vk <= VK_F12) k = VTERM_KEY_FUNCTION(vk - VK_F1 + 1);
        break;
    }
    if (k != VTERM_KEY_NONE) {
        vterm_keyboard_key(s->vt, k, vmods(mods));
        if(k==VTERM_KEY_ENTER&&!s->input_error)++s->input_enters;
        typed_into(s, k == VTERM_KEY_ENTER);
        A.dirty = 1;
        return;
    }
    /* Ctrl+letter is its control character, whatever the keyboard layout makes of it */
    if (ctrl && !alt && vk >= 'A' && vk <= 'Z') {
        char b = (char)(vk - 'A' + 1);
        write_pty(s, &b, 1);
        typed_into(s, vk == 'C');
        return;
    }
    if (ctrl && vk == VK_SPACE) {
        write_pty(s, "", 1); /* NUL */
        typed_into(s, 0);
        return;
    }
    A.eat_char = 0; /* the character comes as WM_CHAR */
}

static void typed(wchar_t c, uint32_t mods)
{
    sess *s = cur_sess();
    /* the character of a key used above: a control character (Enter, Ctrl+T...), never typed text */
    if (A.eat_char) {
        A.eat_char = 0;
        if (c < 0x20 || c == 0x7F || (c == L' ' && (mods & PANELP_M_CTRL))) return;
    }
    if (!s || s->state != S_RUN || A.view != V_TAB) return;
    ++s->input_chars;
    if (c >= 0xD800 && c < 0xDC00) {
        A.hi_surrogate = c;
        return;
    }
    uint32_t cp = c;
    if (c >= 0xDC00 && c < 0xE000 && A.hi_surrogate) cp = 0x10000 + ((uint32_t)(A.hi_surrogate - 0xD800) << 10) + (c - 0xDC00);
    A.hi_surrogate = 0;
    if (cp < 0x20 || cp == 0x7F) {
        char b = (char)cp; /* Ctrl+letter and friends, as they came */
        write_pty(s, &b, 1);
    } else {
        /* Ctrl+Alt is AltGr: the character it made is what was meant */
        int altgr = (mods & PANELP_M_CTRL) && (mods & PANELP_M_ALT);
        vterm_keyboard_unichar(s->vt, cp, altgr ? VTERM_MOD_NONE : (VTermModifier)(vmods(mods) & ~(VTERM_MOD_SHIFT | VTERM_MOD_CTRL)));
    }
    typed_into(s, 0);
    A.dirty = 1;
}

static const hot *hit(int x, int y)
{
    for (int i = A.nhot - 1; i >= 0; i--)
        if (x >= A.hots[i].r.left && x < A.hots[i].r.right && y >= A.hots[i].r.top && y < A.hots[i].r.bottom) return &A.hots[i];
    return NULL;
}

/* the cell under a point: absolute line and column */
static int cell_at(sess *s, int x, int y, int64_t *al, int *col)
{
    int top = tabbar_h() + S(2), rows = view_rows();
    int r = (y - top) / A.chh, c = (x - pad_x() + A.cw / 2) / A.cw;
    if (y < top) r = -1;
    if (r >= rows) r = rows - 1;
    if (c < 0) c = 0;
    *al = s->dropped + view_first(s, rows) + r;
    *col = c;
    return 1;
}

static int is_word(uint32_t c) { return c > ' ' && !wcschr(L"\"'()[]{}<>|;,`", (wchar_t)c); }

static void select_word(sess *s, int64_t al, int col)
{
    tcell buf[512];
    int n = line_cells(s, (int)(al - s->dropped), buf, 512);
    if (col >= n || !is_word(buf[col].ch & CH_MASK)) return;
    int a = col, b = col;
    while (a > 0 && is_word(buf[a - 1].ch & CH_MASK)) a--;
    while (b < n && (is_word(buf[b].ch & CH_MASK) || buf[b].ch & AT_SKIP)) b++;
    A.sel_s = s;
    A.sel_al = A.sel_bl = al;
    A.sel_ac = a;
    A.sel_bc = b;
    A.sel_on = 1;
}

static void mouse_to_app(sess *s, const panelp_input *in, int button, int pressed)
{
    int top = tabbar_h() + S(2), row = (in->y - top) / A.chh, col = (in->x - pad_x()) / A.cw;
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    VTermModifier m = vmods(in->mods);
    vterm_mouse_move(s->vt, row, col, m);
    if (button) vterm_mouse_button(s->vt, button, pressed, m);
}

static void act(const hot *h)
{
    switch (h->kind) {
    case H_TAB: show_tab(h->idx); break;
    case H_TABX: close_tab(h->idx); break;
    case H_NEW:
    case H_EMPTYNEW: new_tab(A.pane, NULL, NULL, NULL); break;
    case H_HIST:
        if (A.view == V_HISTLIST) show_tab(A.cur);
        else show_history();
        break;
    case H_RUN:
        if (cur_sess()) run_again(cur_sess());
        break;
    case H_CLOSE: close_tab(A.cur); break;
    case H_BACK: A.view = V_HISTLIST; break;
    case H_HITEM:
        A.hsel = h->idx;
        A.view = V_HISTITEM;
        break;
    }
    A.dirty = 1;
}

static void input(const panelp_input *in)
{
    sess *s = cur_sess();
    int in_body = in->y >= tabbar_h() && !(s && s->state == S_DONE && in->y >= A.h - footer_h());
    const hot *h = hit(in->x, in->y);
    int app_mouse = s && s->vt && s->state == S_RUN && s->mouse && !(in->mods & PANELP_M_SHIFT) && A.view == V_TAB && in_body;
    switch (in->msg) {
    case WM_MOUSEMOVE: {
        int hk = h ? h->kind : 0, hi = h ? h->idx : -1;
        if (hk != A.hover_kind || hi != A.hover_idx) A.hover_kind = hk, A.hover_idx = hi, A.dirty = 1;
        uint32_t cur = h ? 32649 /* IDC_HAND */ : in_body && s && A.view != V_HISTLIST ? 32513 /* IDC_IBEAM */ : 32512;
        if (cur != A.cursor) send_msg(PANELP_CURSOR, &cur, 4), A.cursor = cur;
        if (app_mouse && !A.selecting) {
            if (s->mouse >= VTERM_PROP_MOUSE_DRAG) mouse_to_app(s, in, 0, 0);
            break;
        }
        if (A.selecting && s) {
            int64_t al;
            int col;
            cell_at(s, in->x, in->y, &al, &col);
            if (in->y < tabbar_h() + S(2)) scroll_by(s, 1);
            else if (in->y > A.h - S(4)) scroll_by(s, -1);
            if (al != A.sel_bl || col != A.sel_bc) A.sel_bl = al, A.sel_bc = col, A.sel_on = 1, A.dirty = 1;
        }
        break;
    }
    case WM_LBUTTONDOWN:
        if (h) {
            act(h);
            break;
        }
        if (app_mouse) {
            mouse_to_app(s, in, 1, 1);
            break;
        }
        if (s && in_body && A.view != V_HISTLIST) {
            int64_t al;
            int col;
            cell_at(s, in->x, in->y, &al, &col);
            A.sel_s = s;
            A.sel_al = A.sel_bl = al;
            A.sel_ac = A.sel_bc = col;
            A.sel_on = 0;
            A.selecting = 1;
            A.dirty = 1;
        }
        break;
    case WM_LBUTTONUP:
        if (app_mouse && !A.selecting) {
            mouse_to_app(s, in, 1, 0);
            break;
        }
        A.selecting = 0;
        break;
    case WM_LBUTTONDBLCLK:
        if (h) {
            if (h->kind == H_HITEM) act(h);
            break;
        }
        if (s && in_body && !app_mouse) {
            int64_t al;
            int col;
            cell_at(s, in->x, in->y, &al, &col);
            select_word(s, al, (in->x - pad_x()) / A.cw);
            A.selecting = 0;
            A.dirty = 1;
        }
        break;
    case WM_RBUTTONDOWN:
        if (app_mouse) {
            mouse_to_app(s, in, 3, 1);
            break;
        }
        /* as in the console: copy what is selected, or paste */
        if (s && A.sel_on && A.sel_s == s) {
            copy_selection();
            select_none();
            A.dirty = 1;
        } else if (s && s->state == S_RUN) {
            paste(s);
        }
        break;
    case WM_RBUTTONUP:
        if (app_mouse) mouse_to_app(s, in, 3, 0);
        break;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        if (app_mouse) mouse_to_app(s, in, 2, in->msg == WM_MBUTTONDOWN);
        else if (h && h->kind == H_TAB && in->msg == WM_MBUTTONUP) close_tab(h->idx);
        break;
    case WM_MOUSEWHEEL: {
        int notches = (short)HIWORD(in->wparam) / WHEEL_DELTA;
        if (!notches) notches = (short)HIWORD(in->wparam) > 0 ? 1 : -1;
        if (A.view == V_HISTLIST) {
            A.hscroll -= notches;
            if (A.hscroll > A.nhist - 1) A.hscroll = A.nhist - 1;
            if (A.hscroll < 0) A.hscroll = 0;
            A.dirty = 1;
        } else if (app_mouse) {
            mouse_to_app(s, in, notches > 0 ? 4 : 5, 1);
        } else if (s && scroll_by(s, notches * 3)) {
            A.dirty = 1;
        }
        break;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: key(in->wparam, in->mods); break;
    case WM_CHAR: typed((wchar_t)in->wparam, in->mods); break;
    }
}

/* ---- host messages ------------------------------------------------------------------------ */

/* "key\tvalue" lines: value of key into out */
static int field(const char *data, uint32_t size, const char *key, wchar_t *out, int cap)
{
    size_t kl = strlen(key);
    const char *p = data, *end = data + size;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        if ((size_t)(le - p) > kl && !memcmp(p, key, kl) && p[kl] == '\t') {
            utf8_to_w(p + kl + 1, (size_t)(le - p - (ptrdiff_t)kl - 1), out, cap);
            return 1;
        }
        p = le + 1;
    }
    out[0] = 0;
    return 0;
}

static int field_path(const char *data, uint32_t size, const char *key, wchar_t *out, size_t cap)
{
    size_t kl = strlen(key);
    const char *p = data, *end = data + size;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        if ((size_t)(le - p) > kl && !memcmp(p, key, kl) && p[kl] == '\t')
            return pathwire_decode(p + kl + 1, (size_t)(le - p - (ptrdiff_t)kl - 1), out, cap);
        p = nl ? nl + 1 : end;
    }
    return 0;
}

static void on_open(const char *data, uint32_t size)
{
    wchar_t dir[MAX_PATH], shell[16], title[64], *cmd = malloc(32768 * sizeof(wchar_t)); /* a relay line can be that long */
    if (!cmd) return;
    if (!field_path(data, size, "dir16", dir, MAX_PATH)) { free(cmd); return; }
    field(data, size, "shell", shell, 16);
    field(data, size, "title", title, 64);
    field(data, size, "cmd", cmd, 32768);
    new_tab(dir, shell, cmd[0] ? cmd : NULL, title[0] ? title : NULL);
    free(cmd);
}

static void on_config(const char *data, uint32_t size)
{
    wchar_t v[64];
    int old_pt = A.pt;
    wchar_t old_font[64];
    wcscpy(old_font, A.font);
    if (field(data, size, "font", v, 64)) wcscpy(A.font, v);
    if (field(data, size, "shell", v, 16)) wcscpy(A.shell, v);
    if (field(data, size, "size", v, 64) && _wtoi(v) >= 6 && _wtoi(v) <= 32) A.pt = _wtoi(v);
    if (field(data, size, "scrollback", v, 64) && _wtoi(v) >= 0) A.scrollback = _wtoi(v);
    if (field(data, size, "history", v, 64) && _wtoi(v) >= 0) A.history = _wtoi(v) < HIST_MAX ? _wtoi(v) : HIST_MAX;
    if (field(data, size, "follow", v, 64)) A.follow = !_wcsicmp(v, L"on");
    if (A.pt != old_pt || wcscmp(A.font, old_font)) {
        make_fonts();
        resize_all();
    }
    while (A.nhist > A.history) sess_free(A.hist[--A.nhist]);
    A.dirty = 1;
}

static void on_host(msg *m)
{
    const char *d = (const char *)m->data;
    uint32_t n = m->head.size;
    switch (m->head.type) {
    case PANELP_PING: send_msg(PANELP_PONG, m->data, 4); break;
    case PANELP_FRAME_ACK:
        A.waiting = 0;
        if (A.pending) present();
        break;
    case PANELP_INPUT:
        if (n >= sizeof(panelp_input)) input((const panelp_input *)m->data);
        break;
    case PANELP_FOCUS: {
        A.focused = n >= 4 && *(uint32_t *)m->data;
        sess *s = cur_sess();
        if (s && s->vt && s->state == S_RUN) {
            VTermState *st = vterm_obtain_state(s->vt);
            if (A.focused) vterm_state_focus_in(st);
            else vterm_state_focus_out(st);
        }
        A.dirty = 1;
        break;
    }
    case PANELP_SHOW:
        A.visible = n >= 4 && *(uint32_t *)m->data;
        if (A.visible) {
            A.dirty = 1;
            sess *s = cur_sess();
            if (s) follow_pane(s);
        }
        break;
    case PANELP_RESIZE:
        if (n >= sizeof(panelp_size)) {
            panelp_size sz;
            memcpy(&sz, m->data, sizeof sz);
            if (sz.dpi && (int)sz.dpi != A.dpi) {
                A.dpi = (int)sz.dpi;
                make_fonts();
            }
            A.seq = sz.seq;
            pixels_map((HANDLE)(uintptr_t)sz.section, (int)sz.w, (int)sz.h);
            paint(); /* the host waits for a frame at the new size */
        }
        break;
    case PANELP_TERM_OPEN: on_open(d, n); break;
    case PANELP_TERM_CONFIG: on_config(d, n); break;
    case PANELP_TERM_PANE:
        { wchar_t pane[MAX_PATH];
        if (pathwire_decode(d, n, pane, MAX_PATH)) {
            wcscpy(A.pane, pane);
            wcscpy(A.want, pane);
            follow_pane(cur_sess());
        }
        }
        break;
    case PANELP_QUIT: PostQuitMessage(0); break;
    }
}

static void on_output(sess *s, chunk *c)
{
    if (!s->vt) return;
    vterm_input_write(s->vt, c->data, c->n);
    if (s == cur_sess()) A.dirty = 1;
}

static void on_exit_msg(sess *s)
{
    if (s->state != S_RUN) return;
    DWORD code = 0;
    if (s->proc) GetExitCodeProcess(s->proc, &code);
    s->exit_code = code;
    s->t_end = GetTickCount64();
    s->state = S_DONE;
    sess_detach(s); /* its output pipe ends once the console is closed: what's still on the way arrives first */
    A.dirty = 1;
}

static LRESULT CALLBACK hidden_proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    panel_startup_trace("terminal.entry");
    (void)prev, (void)cmd, (void)show;
    A.in = GetStdHandle(STD_INPUT_HANDLE);
    A.out = GetStdHandle(STD_OUTPUT_HANDLE);
    A.pt = 10;
    A.scrollback = 5000;
    A.history = 10;
    A.follow = 1;
    A.cur = -1;
    A.hsel = -1;
    A.visible = 1;
    wcscpy(A.shell, L"auto");
    GetEnvironmentVariableW(L"USERPROFILE", A.pane, MAX_PATH);
    panelp_head h;
    panelp_hello hi;
    if (!read_all(A.in, &h, sizeof h) || h.type != PANELP_HELLO || h.size != sizeof hi || !read_all(A.in, &hi, sizeof hi) ||
        hi.magic != PANELP_MAGIC || hi.version != PANELP_VERSION)
        return 1;
    panel_startup_trace("terminal.hello.read");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    A.dpi = hi.dpi ? (int)hi.dpi : 96;
    A.seq = hi.seq;
    A.mdc = CreateCompatibleDC(NULL);
    make_fonts();
    panel_startup_trace("terminal.fonts.ready");

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = hidden_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"FpTermHidden";
    RegisterClassW(&wc);
    A.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!A.hwnd) return 1;
    panel_startup_trace("terminal.window.created");
    pixels_map((HANDLE)(uintptr_t)hi.section, (int)hi.w, (int)hi.h);
    HANDLE t = CreateThread(NULL, 64 * 1024, host_reader, A.hwnd, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (t) CloseHandle(t);
    panelp_ready r = { GetCurrentProcessId(), 0, L"Terminal" };
    send_msg(PANELP_READY, &r, sizeof r);
    panel_startup_trace("terminal.ready.sent");
    paint();
    panel_startup_trace("terminal.first_frame.sent");

    MSG m;
    for (;;) {
        BOOL got = PeekMessageW(&m, NULL, 0, 0, PM_REMOVE);
        if (!got) {
            /* nothing waiting: draw what changed, then sleep until something comes */
            if (A.dirty && A.visible && !A.waiting) paint();
            if (GetMessageW(&m, NULL, 0, 0) <= 0) break;
        } else if (m.message == WM_QUIT) {
            break;
        }
        if (m.message == WM_HOST) {
            on_host((msg *)m.lParam);
            free((void *)m.lParam);
        } else if (m.message == WM_HOST_GONE) {
            break;
        } else if (m.message == WM_T_OUT) {
            sess *s = by_id((int)m.wParam);
            if (s) on_output(s, (chunk *)m.lParam);
            free((void *)m.lParam);
        } else if (m.message == WM_T_EXIT) {
            sess *s = by_id((int)m.wParam);
            if (s) on_exit_msg(s);
        } else if (m.message == WM_T_EOF) {
            sess *s = by_id((int)m.wParam);
            if (s) s->eof = 1;
        } else {
            DispatchMessageW(&m);
        }
        publish_readiness();
        /* a steady stream still shows: at least every 33 ms */
        if (A.dirty && A.visible && !A.waiting && GetTickCount64() - A.last_paint > 33) paint();
    }
    return 0;
}
