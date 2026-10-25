/*
 * fpsettings - the settings app shown in the side panel (app/panelproto.h).
 *
 * Every function with its state, keys, where it shows up and its options,
 * as the explorer applies them. The explorer sends everything again
 * (PANELP_SETTINGS, see fn_describe in app/fn.h) whenever the settings
 * change, so a saved edit to the file shows here at once; what changed is
 * marked for a moment.
 *
 * Editing: the On/Off pill; keys (remove one, record a new one, reset);
 * whether it shows in context menus and the palette; a button per allowed
 * value, and a field for numbers and text (Enter or clicking elsewhere
 * saves it, Esc puts it back). Each change is one PANELP_SET naming the
 * snapshot it was made on; the explorer writes the file only if nothing
 * changed it since, applies it and answers PANELP_SAVED after the new
 * snapshot. One change is in flight at a time; one more made meanwhile
 * waits for the answer. Recording a key asks the explorer for every key
 * (PANELP_CAPTURE), so Alt combinations, F10 and Ctrl+, can be recorded too.
 *
 * Keyboard: Tab and Shift+Tab go through the search box and every control,
 * Space or Enter uses one, Left and Right step between neighbours, Esc goes
 * back to the search box; typing anywhere searches.
 *
 * It draws into the shared bitmaps by hand with GDI; the explorer forwards
 * mouse and keys. What is clickable is laid out with the cards (in list
 * coordinates), so the keyboard reaches controls scrolled out of view.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "keys.h"
#include "relayedit.h"
#include "relaycmd.h"
#include "panelproto.h"
#include "startuptrace.h"
#include "theme.h"

#define WM_HOST      (WM_APP + 1) /* lParam: msg * from the reader thread */
#define WM_BUILDER_RESOLVED (WM_APP + 3)
#define WM_BUILDER_CATALOGUE (WM_APP + 4)
#define WM_HOST_GONE (WM_APP + 2)
#define TIMER_CARET  1
#define TIMER_MARK   2
#define TIMER_NOTICE 3
#define TIMER_PASTE  5
#define TIMER_WAIT   4
#define MARK_MS      2500 /* how long a change stays marked */
#define NOTICE_MS    6000
#define WAIT_MS      3000 /* no answer to a change by then: stop waiting for it */
#define KEYS_MAX     4    /* FN_KEYS_MAX in app/fn.h */

typedef struct msg {
    panelp_head head;
    uint8_t     data[];
} msg;

typedef struct opt {
    wchar_t *name, *value, *def, *values, *help;
    int      set;
} opt;

typedef struct rec {
    wchar_t *id, *title, *life, *state, *keys, *defkeys, *flags, *needs, *about;
    opt     *opts;
    int      nopts, group, order, err;
    uint64_t hash, marked_at;
    wchar_t *search; /* lower case: everything searchable */
    int      y, h;   /* layout, in content coordinates; h 0 = filtered out */
    int      head;   /* draws its group's header above it */
} rec;

typedef struct snap {
    wchar_t  *text; /* owns every string */
    rec      *recs;
    int       n;
    opt      *opts;
    wchar_t **errors;
    int       nerr;
    wchar_t  *file;
    int       exists;
    unsigned  rev;
} snap;

/* A line of text being typed: the search, or a value. */
typedef struct ebox {
    wchar_t t[2048];
    int     len, caret, sel, max;
} ebox;

/* Something clickable and focusable. Its id survives new snapshots (the
 * functions keep their order), so hover and focus stay where they were. */
enum {
    H_SEARCH, H_PILL, H_KEY_DEL, H_KEY_ADD, H_KEY_RESET, H_SHOW_MENU, H_SHOW_PAL, H_CONF_ADD, H_CONF_MOVE, H_CONF_CANCEL,
    H_CHOICE, H_FIELD, H_RESET, H_RELAY_EDIT, H_B_FIELD, H_B_ACTION,
};
enum {BA_CANCEL=1,BA_IMPORT,BA_BACK,BA_REFRESH_CATALOGUE,BA_PICK,BA_BROWSE,BA_TOKEN,BA_RECORD,BA_SHARE,BA_SCOPE,BA_MENU,BA_PALETTE,BA_COUNT,BA_WINDOW,BA_REFRESH,BA_PREVIEW,BA_COPY,BA_SAVE,BA_FALLBACK};
typedef struct hid {
    int kind, rec, opt, sub;
} hid;
typedef struct hot {
    RECT           r; /* list coordinates */
    hid            id;
    const wchar_t *word; /* H_CHOICE: in the option's values */
    int            wlen;
} hot;

#define HOTS_MAX 2048

/* Changes saved together (a key moved: several functions' keys). */
#define CHANGE_ITEMS 12
typedef struct change {
    int used, field, n;
    struct {
        int     reset;
        wchar_t sec[64], key[64], val[256];
    } item[CHANGE_ITEMS];
} change;

static struct {
    HANDLE   in, out;
    int      dpi, w, h;
    HWND     hwnd; /* hidden: timers and the clipboard */
    HFONT    f_text, f_bold, f_small, f_mono, f_head;

    HANDLE   section;
    uint32_t seq;
    uint8_t *shared;
    HDC      mdc;
    HBITMAP  back;
    void     *back_bits;
    int      next, waiting, pending, focused, caret_on;

    uint64_t editor_generation;
    struct {ebox *target;uint64_t hash,until,generation;int caret,sel;} paste;
    RECT r_commands;
    struct {
        int open, focus, recording, saving, base_rev, ticket, context_ticket, browse_ticket, catalogue_busy, resolve_busy, catalogue_dirty;
        relay_draft draft;
        ebox fields[7], query;
        RECT field_rects[7], field_text[7];
        wchar_t message[400], preview[32768], program_status[240], source[512];
        wchar_t *context;
        const wchar_t *dir, *paths[RELAY_CONTEXT_MAX];
        uint32_t count, context_flags;
        relay_catalog *catalogue;
        wchar_t observed[256][80];unsigned n_observed;
    } br;

    snap     s;
    int      have;
    int      scroll, content_h, errors_h;
    int      laid_w; /* the width the layout is for */

    ebox     q;      /* the search */
    RECT     r_field, r_edit, r_list, r_toast;
    int      hover_edit;
    hid      hover, kf; /* under the mouse; has the keyboard (H_SEARCH: the search box) */
    uint32_t cursor;

    hot      hots[HOTS_MAX];
    int      nhot;

    /* the value field: open on one option at a time */
    ebox     fe;
    int      f_open, f_saving;
    wchar_t  f_id[64], f_opt[64];
    wchar_t  f_msg[200];
    RECT     f_at; /* where its text starts, as last painted */

    /* keys: recording one, what went wrong, a key other functions use too */
    int      rec_ri;  /* -1: not recording */
    int      eat_char; /* the recorded key's character is still to come: not typed */
    int      msg_ri;
    wchar_t  key_msg[200];
    struct {
        int     ri, n, others[16];
        wchar_t name[48];
    } conf; /* ri -1: none */

    unsigned req, wait_req; /* the last change sent; the one an answer is due for */
    unsigned last_request, completed, save_status;
    unsigned input_keys, input_chars, input_clicks;
    int      wait_field;    /* that change is the field's */
    change   queued;
    wchar_t  notice[220];
    int      notice_err;
} A;

static int S(int v) { return MulDiv(v, A.dpi, 96); }

/* Storage such as A.fe and builder fields is reused for unrelated editors.
 * A deferred clipboard read belongs to one logical lifetime, not its address. */
static void paste_cancel(void)
{
    A.paste.target = NULL;
    KillTimer(A.hwnd, TIMER_PASTE);
}
static void editor_rebound(void)
{
    ++A.editor_generation;
    paste_cancel();
    if (A.br.open && (!wcsncmp(A.br.message, L"Waiting for", 11) ||
                      !wcsncmp(A.br.message, L"Wait for the pending", 20))) {
        wcscpy(A.br.message, L"Paste cancelled because the field changed.");
        A.laid_w = 0;
    }
}

/* ---- host pipe ------------------------------------------------------------------------- */

static void send_msg(uint32_t type, const void *data, uint32_t size)
{
    panelp_head h = { type, size };
    DWORD w;
    WriteFile(A.out, &h, sizeof h, &w, NULL);
    if (size) WriteFile(A.out, data, size, &w, NULL);
}

static int read_all(void *buf, DWORD n)
{
    uint8_t *p = buf;
    while (n) {
        DWORD got = 0;
        if (!ReadFile(A.in, p, n, &got, NULL) || !got) return 0;
        p += got;
        n -= got;
    }
    return 1;
}

static DWORD WINAPI reader(void *arg)
{
    HWND to = arg;
    for (;;) {
        panelp_head h;
        if (!read_all(&h, sizeof h) || h.size > PANELP_MAX) break;
        msg *m = malloc(sizeof *m + h.size);
        if (!m || !read_all(m->data, h.size)) {
            free(m);
            break;
        }
        m->head = h;
        if (!PostMessageW(to, WM_HOST, 0, (LPARAM)m)) free(m);
    }
    PostMessageW(to, WM_HOST_GONE, 0, 0);
    return 0;
}

static void run_fn(const char *id) { send_msg(PANELP_RUN, id, (uint32_t)strlen(id)); }

static void notice(int err, const wchar_t *text)
{
    _snwprintf(A.notice, 220, L"%ls", text);
    A.notice[219] = 0;
    A.notice_err = err;
    SetTimer(A.hwnd, TIMER_NOTICE, NOTICE_MS, NULL);
}

static void send_change(const change *c)
{
    size_t cap = 64 + (size_t)c->n * 1200, n;
    char *buf = malloc(cap);
    if (!buf) return;
    A.wait_req = ++A.req;
    A.last_request = A.wait_req;
    A.wait_field = c->field;
    n = (size_t)snprintf(buf, cap, "%u\t%u", A.wait_req, A.s.rev);
    for (int i = 0; i < c->n; i++) {
        char sec[192], key[192], val[768];
        WideCharToMultiByte(CP_UTF8, 0, c->item[i].sec, -1, sec, sizeof sec, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, c->item[i].key, -1, key, sizeof key, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, c->item[i].val, -1, val, sizeof val, NULL, NULL);
        n += (size_t)snprintf(buf + n, cap - n, "\n%s\t%s\t%s\t%s", sec, key, c->item[i].reset ? "reset" : "set",
                              c->item[i].reset ? "" : val);
    }
    send_msg(PANELP_SET, buf, (uint32_t)(n < cap ? n : 0));
    free(buf);
    SetTimer(A.hwnd, TIMER_WAIT, WAIT_MS, NULL);
}

static void change_add(change *c, const wchar_t *sec, const wchar_t *key, const wchar_t *val)
{
    if (c->n >= CHANGE_ITEMS) return;
    c->used = 1;
    c->item[c->n].reset = val == NULL;
    _snwprintf(c->item[c->n].sec, 64, L"%ls", sec);
    _snwprintf(c->item[c->n].key, 64, L"%ls", key);
    _snwprintf(c->item[c->n].val, 256, L"%ls", val ? val : L"");
    c->item[c->n].sec[63] = c->item[c->n].key[63] = c->item[c->n].val[255] = 0;
    c->n++;
}

static void change_send(const change *c)
{
    if (!A.wait_req) send_change(c);
    else if (!A.queued.used || !A.queued.field) A.queued = *c; /* sent once the answer (and its snapshot) is in */
    else notice(1, L"Not saved: one change at a time, try again");
}

/* sec "functions", "keys", "menu" or "palette" with key an id, or an id with key an option; val NULL resets. */
static void change_setting(const wchar_t *sec, const wchar_t *key, const wchar_t *val, int from_field)
{
    change c = { 0 };
    c.field = from_field;
    change_add(&c, sec, key, val);
    change_send(&c);
}

/* ---- the snapshot ---------------------------------------------------------------------- */

static uint64_t whash(const wchar_t *s, uint64_t h)
{
    for (; *s; s++) h = (h ^ *s) * 1099511628211ull;
    return (h ^ 0xFFFF) * 1099511628211ull;
}

/* Read-only automation state, published after a complete UI message. Input
 * delivery to the explorer is earlier than processing in this helper; a file
 * write is earlier than PANELP_SAVED. Neither is a completion acknowledgment.
 * Focus kinds use the H_* enum above. Text identities hash UTF-16 code units
 * with whash(text, 0); lengths distinguish empty text from absent controls. */
static void observe_state(void)
{
    static uintptr_t previous[22];
    static unsigned revision;
    const wchar_t *id = L"";
    if (A.kf.kind > H_SEARCH && A.kf.kind <= H_RELAY_EDIT && A.kf.rec >= 0 && A.kf.rec < A.s.n)
        id = A.s.recs[A.kf.rec].id;
    uintptr_t values[] = {
        A.have, A.s.rev, A.input_keys, A.input_chars, A.input_clicks,
        A.last_request, A.completed, A.save_status, A.wait_req,
        A.f_open, A.f_saving, A.kf.kind, (uintptr_t)whash(id, 0), A.kf.sub,
        (uintptr_t)whash(A.q.t, 0), A.q.len, A.nhot,
        A.rec_ri >= 0, A.conf.ri >= 0, A.br.open, A.br.saving, A.br.base_rev
    };
    static const wchar_t *names[] = {
        L"Have", L"Snapshot", L"Keys", L"Chars", L"Clicks",
        L"Request", L"Completed", L"Status", L"Pending",
        L"FieldOpen", L"FieldSaving", L"FocusKind", L"FocusId", L"FocusSub",
        L"QueryHash", L"QueryLength", L"Controls",
        L"Recording", L"Conflict", L"Builder", L"BuilderSaving", L"BaseRevision"
    };
    if (revision && !memcmp(previous, values, sizeof values)) return;
    SetPropW(A.hwnd, L"Settings.State", (HANDLE)(uintptr_t)++revision);
    for (unsigned i = 0; i < sizeof values / sizeof *values; ++i) {
        wchar_t name[64];
        _snwprintf(name, 64, L"Settings.%ls", names[i]);
        SetPropW(A.hwnd, name, (HANDLE)values[i]);
    }
    memcpy(previous, values, sizeof values);
    SetPropW(A.hwnd, L"Settings.State", (HANDLE)(uintptr_t)++revision);
}

/* Splits a line into tab-separated fields, in place. */
static int fields(wchar_t *line, wchar_t **f, int max)
{
    int n = 0;
    f[n++] = line;
    for (wchar_t *p = line; *p && n < max; p++)
        if (*p == L'\t') {
            *p = 0;
            f[n++] = p + 1;
        }
    for (int i = n; i < max; i++) f[i] = L"";
    return n;
}

/* "F3, Ctrl+Shift+X" -> names */
static int split_keys(const wchar_t *s, wchar_t out[][48], int max)
{
    int n = 0;
    for (const wchar_t *p = s; *p && n < max;) {
        while (*p == L' ' || *p == L',') p++;
        const wchar_t *e = p;
        while (*e && *e != L',') e++;
        const wchar_t *t = e;
        while (t > p && t[-1] == L' ') t--;
        if (t > p) {
            _snwprintf(out[n], 48, L"%.*ls", (int)(t - p), p);
            out[n++][47] = 0;
        }
        p = e;
    }
    return n;
}

static void join_keys(wchar_t keys[][48], int n, int skip, const wchar_t *add, wchar_t *buf, size_t cap)
{
    size_t len = 0;
    buf[0] = 0;
    for (int i = 0; i < n; i++)
        if (i != skip) len += (size_t)_snwprintf(buf + len, len < cap ? cap - len : 0, L"%ls%ls", len ? L", " : L"", keys[i]);
    if (add) len += (size_t)_snwprintf(buf + len, len < cap ? cap - len : 0, L"%ls%ls", len ? L", " : L"", add);
    if (!buf[0] || len >= cap) _snwprintf(buf, cap, L"none");
}

static const struct {
    const wchar_t *area, *name;
} groups[] = {
    { L"view", L"View" },         { L"nav", L"Navigation" },     { L"tab", L"Tabs" },
    { L"item", L"Items" },        { L"file", L"Files" },         { L"edit", L"Clipboard" },
    { L"sel", L"Selection" },     { L"tree", L"Tree" },          { L"window", L"Window" },
    { L"palette", L"Palette" },   { L"search", L"Search" },      { L"preview", L"PDF preview" },
    { L"wsl", L"WSL" },           { L"shell", L"Shell" },        { L"menu", L"Menus" },
    { L"session", L"Session" },   { L"settings", L"Settings" },  { L"panel", L"Side panel" },
    { L"svc", L"Services" },      { L"debug", L"Debug" },
};

static int area_len(const wchar_t *id)
{
    const wchar_t *dot = wcschr(id, L'.');
    return dot ? (int)(dot - id) : (int)wcslen(id);
}

static void group_name(const rec *r, wchar_t *buf, size_t cap)
{
    int al = area_len(r->id);
    for (size_t i = 0; i < sizeof groups / sizeof groups[0]; i++)
        if ((int)wcslen(groups[i].area) == al && !wcsncmp(groups[i].area, r->id, (size_t)al)) {
            _snwprintf(buf, cap, L"%ls", groups[i].name);
            return;
        }
    _snwprintf(buf, cap, L"%.*ls", al, r->id);
    if (buf[0] >= L'a' && buf[0] <= L'z') buf[0] -= 32;
}

/* Does `line` name `id` as a whole word? ("[preview.pdf] colors = x" or "... for file.rename") */
static int names(const wchar_t *line, const wchar_t *id)
{
    size_t n = wcslen(id);
    for (const wchar_t *p = line; (p = wcsstr(p, id)); p++) {
        wchar_t before = p > line ? p[-1] : L' ', after = p[n];
        int word_b = !(iswalnum(before) || before == L'.' || before == L'_');
        int word_a = !(iswalnum(after) || after == L'.' || after == L'_');
        if (word_b && word_a) return 1;
    }
    return 0;
}

static void snap_free(snap *s)
{
    if (s->recs)
        for (int i = 0; i < s->n; i++) free(s->recs[i].search);
    free(s->text);
    free(s->recs);
    free(s->opts);
    free(s->errors);
    memset(s, 0, sizeof *s);
}

static int cmp_group(const void *a, const void *b)
{
    const rec *x = a, *y = b;
    return x->group != y->group ? x->group - y->group : x->order - y->order;
}

static const opt *find_field_opt(void)
{
    for (int i = 0; i < A.s.n; i++)
        if (!wcscmp(A.s.recs[i].id, A.f_id))
            for (int k = 0; k < A.s.recs[i].nopts; k++)
                if (!wcscmp(A.s.recs[i].opts[k].name, A.f_opt)) return &A.s.recs[i].opts[k];
    return NULL;
}

static void measured_clear(void);
static void record_stop(void);

/* A layout index belongs to one snapshot. Preserve a control only while its
 * record/option identity (and any positional sub-list) still identifies it. */
static hid snapshot_control(hid id, const snap *next, int focus)
{
    hid gone = { focus ? H_SEARCH : -1, -1, -1, 0 };
    if (id.kind == H_SEARCH || id.kind < 0 || id.kind == H_B_FIELD || id.kind == H_B_ACTION) return id;
    if (id.kind >= H_CONF_ADD && id.kind <= H_CONF_CANCEL) return gone;
    if (id.rec < 0 || id.rec >= A.s.n) return gone;
    const rec *old = &A.s.recs[id.rec];
    int ri;
    for (ri = 0; ri < next->n; ri++)
        if (!wcscmp(old->id, next->recs[ri].id)) break;
    if (ri == next->n) return gone;
    const rec *r = &next->recs[ri];
    if (id.kind == H_KEY_DEL && wcscmp(old->keys, r->keys)) return gone;
    if (id.kind == H_CHOICE || id.kind == H_FIELD || id.kind == H_RESET) {
        if (id.opt < 0 || id.opt >= old->nopts) return gone;
        const opt *o = &old->opts[id.opt];
        int oi;
        for (oi = 0; oi < r->nopts; oi++)
            if (!wcscmp(o->name, r->opts[oi].name)) break;
        if (oi == r->nopts) return gone;
        if (id.kind == H_CHOICE && wcscmp(o->values, r->opts[oi].values)) return gone;
        id.opt = oi;
    }
    id.rec = ri;
    return id;
}

static void take_snapshot(const char *utf8, uint32_t size)
{
    snap s = { 0 };
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)size, NULL, 0);
    s.text = malloc(((size_t)wn + 1) * sizeof(wchar_t));
    if (!s.text) return;
    MultiByteToWideChar(CP_UTF8, 0, utf8, (int)size, s.text, wn);
    s.text[wn] = 0;
    int nfn = 0, nopt = 0, nerr = 0;
    for (wchar_t *p = s.text; p && *p;) {
        nfn += !wcsncmp(p, L"fn\t", 3);
        nopt += !wcsncmp(p, L"opt\t", 4);
        nerr += !wcsncmp(p, L"error\t", 6);
        p = wcschr(p, L'\n');
        if (p) p++;
    }
    s.recs = calloc((size_t)nfn + 1, sizeof *s.recs);
    s.opts = calloc((size_t)nopt + 1, sizeof *s.opts);
    s.errors = calloc((size_t)nerr + 1, sizeof *s.errors);
    if (!s.recs || !s.opts || !s.errors) {
        snap_free(&s);
        return;
    }
    s.file = L"";
    rec *cur = NULL;
    int no = 0;
    for (wchar_t *p = s.text; p && *p;) {
        wchar_t *nl = wcschr(p, L'\n');
        if (nl) *nl = 0;
        wchar_t *f[10];
        fields(p, f, 10);
        if (!wcscmp(f[0], L"rev")) {
            s.rev = (unsigned)wcstoul(f[1], NULL, 10);
        } else if (!wcscmp(f[0], L"file")) {
            s.file = f[1];
            s.exists = _wtoi(f[2]);
        } else if (!wcscmp(f[0], L"error")) {
            s.errors[s.nerr++] = f[1];
        } else if (!wcscmp(f[0], L"fn")) {
            cur = &s.recs[s.n++];
            cur->id = f[1], cur->title = f[2], cur->life = f[3], cur->state = f[4], cur->keys = f[5];
            cur->defkeys = f[6], cur->flags = f[7], cur->needs = f[8], cur->about = f[9];
            cur->opts = &s.opts[no];
            uint64_t h = 1469598103934665603ull;
            for (int i = 1; i < 10; i++) h = whash(f[i], h);
            cur->hash = h;
        } else if (!wcscmp(f[0], L"opt") && cur) {
            opt *o = &s.opts[no++];
            cur->nopts++;
            o->name = f[1], o->value = f[2], o->def = f[3], o->set = _wtoi(f[4]), o->values = f[5], o->help = f[6];
            for (int i = 1; i < 7; i++) cur->hash = whash(f[i], cur->hash);
        }
        p = nl ? nl + 1 : NULL;
    }

    /* groups in the order they first appear; functions keep the registry's order inside them */
    for (int i = 0; i < s.n; i++) {
        rec *r = &s.recs[i];
        r->group = r->order = i;
        for (int k = 0; k < i; k++)
            if (area_len(s.recs[k].id) == area_len(r->id) && !wcsncmp(s.recs[k].id, r->id, (size_t)area_len(r->id))) {
                r->group = s.recs[k].group;
                break;
            }
    }
    qsort(s.recs, (size_t)s.n, sizeof *s.recs, cmp_group);

    uint64_t now = GetTickCount64();
    for (int i = 0; i < s.n; i++) {
        rec *r = &s.recs[i];
        for (int e = 0; e < s.nerr && !r->err; e++) r->err = names(s.errors[e], r->id);
        /* changed since the last snapshot: marked for a while */
        if (A.have) {
            const rec *old = NULL;
            for (int k = 0; k < A.s.n && !old; k++)
                if (!wcscmp(A.s.recs[k].id, r->id)) old = &A.s.recs[k];
            r->marked_at = !old || old->hash != r->hash ? now : old->marked_at;
        }
        /* what search looks at */
        size_t len = 64 + wcslen(r->id) + wcslen(r->title) + wcslen(r->about) + wcslen(r->keys);
        for (int k = 0; k < r->nopts; k++) len += wcslen(r->opts[k].name) + wcslen(r->opts[k].help) + wcslen(r->opts[k].value) + 3;
        r->search = malloc(len * sizeof(wchar_t));
        if (r->search) {
            int at = _snwprintf(r->search, len, L"%ls %ls %ls %ls", r->id, r->title, r->about, r->keys);
            for (int k = 0; k < r->nopts && at > 0 && (size_t)at < len; k++)
                at += _snwprintf(r->search + at, len - (size_t)at, L" %ls %ls %ls", r->opts[k].name, r->opts[k].value,
                                 r->opts[k].help);
            r->search[len - 1] = 0;
            CharLowerW(r->search);
        }
    }
    editor_rebound(); /* accepted reload, including unchanged or removed targets */
    hid focus = snapshot_control(A.kf, &s, 1);
    hid hover = snapshot_control(A.hover, &s, 0);
    /* Only an accepted replacement cancels recording. Release the host's
     * capture while the old records still exist, including unchanged reloads. */
    record_stop();
    A.conf.ri = A.msg_ri = -1;
    A.conf.n = 0;
    A.conf.name[0] = A.key_msg[0] = 0;
    memset(A.conf.others, 0, sizeof A.conf.others);
    snap_free(&A.s);
    measured_clear(); /* only what this snapshot shows gets measured again */
    A.s = s;
    A.have = 1;
    A.laid_w = 0;
    A.nhot = 0;
    A.kf = focus;
    A.hover = hover;
    A.hover_edit = 0;
    if (A.f_open && (!A.s.file[0] || !find_field_opt())) A.f_open = 0; /* its option is gone */
    KillTimer(A.hwnd, TIMER_MARK);
    SetTimer(A.hwnd, TIMER_MARK, MARK_MS + 50, NULL);
}

/* ---- typing ------------------------------------------------------------------------------- */

static int lo_of(const ebox *e) { return e->sel < e->caret ? e->sel : e->caret; }
static int hi_of(const ebox *e) { return e->sel < e->caret ? e->caret : e->sel; }

static void eb_set(ebox *e, const wchar_t *s)
{
    _snwprintf(e->t, (size_t)e->max + 1, L"%ls", s);
    e->t[e->max] = 0;
    e->len = (int)wcslen(e->t);
    e->sel = 0;
    e->caret = e->len;
}

/* Replaces the selection with s (control characters dropped). */
static void eb_insert(ebox *e, const wchar_t *s, int n)
{
    int lo = lo_of(e), hi = hi_of(e);
    memmove(e->t + lo, e->t + hi, (size_t)(e->len - hi + 1) * sizeof(wchar_t));
    e->len -= hi - lo;
    e->caret = lo;
    for (int i = 0; i < n && e->len < e->max; i++) {
        if (s[i] < 32) continue;
        memmove(e->t + e->caret + 1, e->t + e->caret, (size_t)(e->len - e->caret + 1) * sizeof(wchar_t));
        e->t[e->caret++] = s[i];
        e->len++;
    }
    e->t[e->len] = 0;
    e->sel = e->caret;
}

/* A Cut may remove its draft only after the clipboard accepts the new text.
 * Keep a private copy of existing Unicode bytes before EmptyClipboard so a
 * failed publication can restore them. Other clipboard formats/ownership are
 * not a transaction Windows can roll back here. */
static int copy_text(const wchar_t *s, int n)
{
    HGLOBAL g = NULL, previous = NULL;
    int opened = 0, copied = 0, restore_failed = 0;
    g = GlobalAlloc(GMEM_MOVEABLE, ((size_t)n + 1) * sizeof(wchar_t));
    if (!g) goto done;
    wchar_t *d = GlobalLock(g);
    if (!d) goto done;
    memcpy(d, s, (size_t)n * sizeof(wchar_t));
    d[n] = 0;
    GlobalUnlock(g);
    if (!(opened = OpenClipboard(A.hwnd))) goto done;
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE old = GetClipboardData(CF_UNICODETEXT);
        SIZE_T bytes = old ? GlobalSize(old) : 0;
        const void *before = bytes ? GlobalLock(old) : NULL;
        if (!before) goto done;
        previous = GlobalAlloc(GMEM_MOVEABLE, bytes);
        void *backup = previous ? GlobalLock(previous) : NULL;
        if (backup) {
            memcpy(backup, before, bytes);
            GlobalUnlock(previous);
        }
        GlobalUnlock(old);
        if (!backup) goto done;
    }
    if (!EmptyClipboard()) goto done;
    if (SetClipboardData(CF_UNICODETEXT, g)) {
        g = NULL;
        copied = 1;
    } else if (previous) {
        if (SetClipboardData(CF_UNICODETEXT, previous)) previous = NULL;
        else restore_failed = 1;
    }
done:
    if (opened) CloseClipboard();
    if (g) GlobalFree(g);
    if (previous) GlobalFree(previous);
    if (!copied) notice(1, restore_failed
        ? L"Copy failed and the previous clipboard text could not be restored. The draft was kept."
        : L"Could not copy the text. The draft was kept.");
    return copied;
}

static int paste_read(ebox *e)
{
    if (!OpenClipboard(A.hwnd)) return -1;
    HANDLE g = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *s = g ? GlobalLock(g) : NULL;
    int valid=0;
    if(s){size_t capacity=GlobalSize(g)/sizeof(wchar_t);size_t n=wcsnlen(s,capacity);
        int room=e->max-(e->len-(hi_of(e)-lo_of(e)));
        int single=1;for(size_t i=0;i<n;++i)if(s[i]<32||s[i]==127)single=0;
        if(n==capacity||n>(size_t)room||!single){
            if(A.br.open){wcscpy(A.br.message,n>(size_t)room?L"The pasted value is too long for this field; it was not inserted.":L"Paste one line of text; it was not inserted.");A.laid_w=0;}
            else notice(1,L"The pasted value must fit the field on one line; it was not inserted.");
        }else{eb_insert(e,s,(int)n);valid=1;}
        GlobalUnlock(g);
    }
    CloseClipboard();
    return valid;
}

static int eb_paste(ebox *e)
{
    paste_cancel(); /* a new explicit paste supersedes any older request */
    int rc=paste_read(e);
    if(rc<0){A.paste.target=e;A.paste.generation=A.editor_generation;A.paste.hash=whash(e->t,0);A.paste.caret=e->caret;A.paste.sel=e->sel;
        A.paste.until=GetTickCount64()+1500;SetTimer(A.hwnd,TIMER_PASTE,40,NULL);
        if(A.br.open){wcscpy(A.br.message,L"Waiting for the clipboard...");A.laid_w=0;}}
    return rc>0;
}

/* Editing keys; 1 = the text changed. */
static int eb_key(ebox *e, UINT vk, uint32_t mods)
{
    int shift = (mods & PANELP_M_SHIFT) != 0, ctrl = (mods & PANELP_M_CTRL) != 0;
    int lo = lo_of(e), hi = hi_of(e);
    switch (vk) {
    case VK_HOME: e->caret = 0; break;
    case VK_END: e->caret = e->len; break;
    case VK_LEFT: if (e->caret) e->caret--; break;
    case VK_RIGHT: if (e->caret < e->len) e->caret++; break;
    case VK_DELETE:
        if (hi == lo && hi < e->len) e->sel = hi + 1, e->caret = lo;
        eb_insert(e, L"", 0);
        return 1;
    case 'A': if (ctrl) e->sel = 0, e->caret = e->len; return 0;
    case 'C': if (ctrl && hi > lo) copy_text(e->t + lo, hi - lo); return 0;
    case 'X':
        if (!ctrl || hi == lo) return 0;
        if (!copy_text(e->t + lo, hi - lo)) return 0;
        eb_insert(e, L"", 0);
        return 1;
    case 'V': return ctrl && eb_paste(e);
    default: return 0;
    }
    if (!shift) e->sel = e->caret;
    return 0;
}

static int eb_char(ebox *e, wchar_t c, uint32_t mods)
{
    if (mods & PANELP_M_CTRL) return 0;
    if (c == 8) {
        int lo = lo_of(e), hi = hi_of(e);
        if (hi == lo && lo) e->sel = lo - 1;
        eb_insert(e, L"", 0);
        return 1;
    }
    if (c < 32) return 0;
    if(e->len-(hi_of(e)-lo_of(e))>=e->max){if(A.br.open){wcscpy(A.br.message,L"This field has reached its character limit.");A.laid_w=0;}return 0;}
    eb_insert(e, &c, 1);
    return 1;
}

/* ---- layout and drawing ------------------------------------------------------------------- */

enum { LAYOUT, DRAW };

static int matches(const rec *r)
{
    if (!A.q.len) return 1;
    if (!r->search) return 0;
    wchar_t q[256];
    wcscpy(q, A.q.t);
    CharLowerW(q);
    for (wchar_t *word = q; *word;) {
        while (*word == L' ') word++;
        wchar_t *end = word;
        while (*end && *end != L' ') end++;
        wchar_t keep = *end;
        *end = 0;
        int found = !*word || wcsstr(r->search, word);
        *end = keep;
        if (!found) return 0;
        word = end;
    }
    return 1;
}

static void fill(int x, int y, int w, int h, COLORREF c)
{
    RECT r = { x, y, x + w, y + h };
    SetDCBrushColor(A.mdc, c);
    FillRect(A.mdc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

static void text(HFONT f, const wchar_t *s, int n, RECT r, COLORREF c, UINT fmt)
{
    SelectObject(A.mdc, f);
    SetTextColor(A.mdc, c);
    DrawTextW(A.mdc, s, n, &r, fmt | DT_NOPREFIX);
}

/* Measured text. A string's character advances don't depend on the width it
 * is wrapped to, so each string (by content, in a font) is measured once and
 * wrapping at any width is arithmetic: resizing the panel re-wraps every
 * card without asking GDI again. Emptied with each snapshot and font change. */
typedef struct measured {
    uint64_t key; /* content hash and font; 0 = free */
    int      n;
    int     *adv; /* adv[i]: width of the first i + 1 characters */
} measured;

#define MEASURED_MAX 8192 /* a power of two */
static measured *g_meas;
static int       g_nmeas;

static void measured_clear(void)
{
    if (!g_meas) return;
    for (int i = 0; i < MEASURED_MAX; i++) free(g_meas[i].adv);
    memset(g_meas, 0, MEASURED_MAX * sizeof *g_meas);
    g_nmeas = 0;
}

/* Advances of s[0..n) in f, or NULL (then callers ask GDI directly). */
static const int *advances(HFONT f, const wchar_t *s, int n)
{
    if (n <= 0) return NULL;
    if (!g_meas && !(g_meas = calloc(MEASURED_MAX, sizeof *g_meas))) return NULL;
    uint64_t h = 1469598103934665603ull ^ (uint64_t)(uintptr_t)f;
    for (int i = 0; i < n; i++) h = (h ^ s[i]) * 1099511628211ull;
    h = (h ^ (uint64_t)n) * 1099511628211ull;
    if (!h) h = 1;
    if (g_nmeas > MEASURED_MAX * 3 / 4) measured_clear();
    for (uint32_t at = (uint32_t)h & (MEASURED_MAX - 1);; at = (at + 1) & (MEASURED_MAX - 1)) {
        measured *m = &g_meas[at];
        if (m->key == h && m->n == n) return m->adv;
        if (m->key) continue;
        int *adv = malloc((size_t)n * sizeof(int));
        SIZE sz;
        SelectObject(A.mdc, f);
        if (!adv || !GetTextExtentExPointW(A.mdc, s, n, 0, NULL, adv, &sz)) {
            free(adv);
            return NULL;
        }
        m->key = h, m->n = n, m->adv = adv;
        g_nmeas++;
        return adv;
    }
}

static int text_w(HFONT f, const wchar_t *s, int n)
{
    if (n < 0) n = (int)wcslen(s);
    if (!n) return 0;
    const int *adv = advances(f, s, n);
    if (adv) return adv[n - 1];
    SIZE sz = { 0 };
    SelectObject(A.mdc, f);
    GetTextExtentPoint32W(A.mdc, s, n, &sz);
    return sz.cx;
}

static int line_h(HFONT f)
{
    TEXTMETRICW tm;
    SelectObject(A.mdc, f);
    return GetTextMetricsW(A.mdc, &tm) ? tm.tmHeight : 16;
}

/* Wraps s at spaces to width w, like DT_WORDBREAK: a word wider than w gets
 * a line of its own. Draws each line at (x, y) when asked; returns the height. */
static int wrap(int mode, HFONT f, const wchar_t *s, int x, int y, int w, COLORREF c)
{
    int n = s ? (int)wcslen(s) : 0;
    if (!n || w < 10) return 0;
    const int *adv = advances(f, s, n);
    int lh = line_h(f), lines = 0;
    if (mode == DRAW) {
        SelectObject(A.mdc, f);
        SetTextColor(A.mdc, c);
        SetTextAlign(A.mdc, TA_TOP | TA_LEFT);
    }
#define W(a, b) (adv ? ((b) > (a) ? adv[(b) - 1] - ((a) ? adv[(a) - 1] : 0) : 0) : text_w(f, s + (a), (b) - (a)))
    for (int a = 0; a < n;) {
        int end = a, e = a; /* end: the last break that fits */
        while (e < n && s[e] != L'\n') {
            int we = e;
            while (we < n && s[we] != L' ' && s[we] != L'\n') we++; /* the word's end */
            if (end > a && W(a, we) > w) break;
            end = we;
            e = we;
            while (e < n && s[e] == L' ') e++;
        }
        if (end == a) end = e; /* only spaces */
        if (mode == DRAW) ExtTextOutW(A.mdc, x, y + lines * lh, 0, NULL, s + a, (UINT)(end - a), NULL);
        lines++;
        a = end;
        while (a < n && s[a] == L' ') a++;
        if (a < n && s[a] == L'\n') a++;
    }
#undef W
    return lines * lh;
}

static int wrapped_h(HFONT f, const wchar_t *s, int w) { return wrap(LAYOUT, f, s, 0, 0, w, 0); }

/* Wrapped text at (x, y); returns its height. */
static int para(int mode, HFONT f, const wchar_t *s, int x, int y, int w, COLORREF c) { return wrap(mode, f, s, x, y, w, c); }

static int can_edit(void) { return A.have && A.s.file[0]; }
static int builder_allowed(void)
{
    for(int i=0;i<A.s.n;++i)if(!wcscmp(A.s.recs[i].id,L"settings.commands"))return !wcscmp(A.s.recs[i].state,L"on");return 0;
}

static int hid_eq(hid a, hid b) { return a.kind == b.kind && a.rec == b.rec && a.opt == b.opt && a.sub == b.sub; }
static hid mk(int kind, int ri, int k, int sub) { return (hid){ kind, ri, k, sub }; }
static int is_hover(hid id) { return hid_eq(A.hover, id); }
static int is_focus(hid id) { return A.focused && hid_eq(A.kf, id); }

/* A control at r: registered while laying out, given a focus ring while drawing. */
static void control(int mode, hid id, RECT r, const wchar_t *word, int wlen)
{
    if (mode == LAYOUT) {
        if (A.nhot < HOTS_MAX && (can_edit()||id.kind>=H_B_FIELD)) A.hots[A.nhot++] = (hot){ r, id, word, wlen };
    } else if (is_focus(id)) {
        int t = S(1) > 0 ? S(1) : 1, g = S(2);
        fill(r.left - g, r.top - g, r.right - r.left + 2 * g, t, C_ACCENT);
        fill(r.left - g, r.bottom + g - t, r.right - r.left + 2 * g, t, C_ACCENT);
        fill(r.left - g, r.top - g, t, r.bottom - r.top + 2 * g, C_ACCENT);
        fill(r.right + g - t, r.top - g, t, r.bottom - r.top + 2 * g, C_ACCENT);
    }
}

/* A small button with a label; returns its width. */
static int chip(int mode, hid id, int x, int y, int h, const wchar_t *label, int on, COLORREF color)
{
    int w = text_w(A.f_small, label, -1) + S(16);
    RECT b = { x, y, x + w, y + h };
    if (mode == DRAW) {
        fill(x, y, w, h, on ? C_SEL : is_hover(id) ? C_SEL_IDLE : C_EDIT);
        text(A.f_small, label, -1, b, color, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }
    control(mode, id, b, NULL, 0);
    return w;
}

static int link(int mode, hid id, int x, int y, int h, const wchar_t *label)
{
    int w = text_w(A.f_small, label, -1) + S(12);
    RECT b = { x, y, x + w, y + h };
    if (mode == DRAW) text(A.f_small, label, -1, b, is_hover(id) ? C_TEXT : C_ACCENT, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    control(mode, id, b, NULL, 0);
    return w;
}

static int field_is(const rec *r, const opt *o) { return A.f_open && !wcscmp(r->id, A.f_id) && !wcscmp(o->name, A.f_opt); }

/* One of a few words: a row of choices. "+a b c" (some of them) is typed. */
static int is_word_list(const opt *o) { return o->values[0] && o->values[0] != L'#' && o->values[0] != L'+'; }

static int choices_w(const opt *o)
{
    int w = 0;
    for (const wchar_t *p = o->values; *p;) {
        while (*p == L' ') p++;
        const wchar_t *e = p;
        while (*e && *e != L' ') e++;
        if (e > p) w += text_w(A.f_small, p, (int)(e - p)) + S(16) + S(2);
        p = e;
    }
    return w;
}

/* differs from its default (words in any case, numbers by value) */
static int differs(const opt *o)
{
    if (!o->set) return 0;
    if (o->values[0] == L'#') return _wtoi(o->value) != _wtoi(o->def);
    return o->values[0] ? _wcsicmp(o->value, o->def) != 0 : wcscmp(o->value, o->def) != 0;
}

/* A line being typed: its text, scrolled so the caret shows. */
static void draw_ebox(const ebox *e, RECT box, int focused, const wchar_t *placeholder, COLORREF color, RECT *text_at)
{
    int inner = box.right - box.left - S(16);
    int caret_x = text_w(A.f_text, e->t, e->caret);
    int shift = focused && caret_x > inner ? caret_x - inner : 0;
    int tx = box.left + S(8) - shift;
    if (text_at) *text_at = (RECT){ tx, box.top, box.right - S(8), box.bottom };
    int saved = SaveDC(A.mdc);
    IntersectClipRect(A.mdc, box.left + S(4), box.top, box.right - S(4), box.bottom);
    int lo = lo_of(e), hi = hi_of(e);
    if (focused && hi > lo)
        fill(tx + text_w(A.f_text, e->t, lo), box.top + S(3), text_w(A.f_text, e->t + lo, hi - lo), box.bottom - box.top - S(6),
             C_SEL);
    RECT tr = { tx, box.top, box.right + shift, box.bottom };
    if (e->len) text(A.f_text, e->t, e->len, tr, color, DT_SINGLELINE | DT_VCENTER);
    else if (placeholder) text(A.f_text, placeholder, -1, tr, C_DIM, DT_SINGLELINE | DT_VCENTER);
    if (focused && A.focused && A.caret_on) fill(tx + caret_x, box.top + S(4), S(1) > 0 ? S(1) : 1, box.bottom - box.top - S(8), C_TEXT);
    RestoreDC(A.mdc, saved);
}

/* "0 to 3600", or "some of: a b c" */
static void range_text(const wchar_t *v, wchar_t *buf, size_t cap)
{
    buf[0] = 0;
    if (v[0] == L'+') {
        _snwprintf(buf, cap, L"some of: %ls", v + 1);
        buf[cap - 1] = 0;
        return;
    }
    if (v[0] != L'#') return;
    int lo = 0, hi = 0;
    swscanf(v + 1, L"%d..%d", &lo, &hi);
    _snwprintf(buf, cap, L"%d to %d", lo, hi);
}

/* An option: its name and control on one line (the buttons on their own
 * when they don't fit), then its help, then a problem with what was typed. */
static int option_row(const rec *r, int ri, int k, int x, int y, int w, int mode)
{
    const opt *o = &r->opts[k];
    int ox = x + S(12), ow = w - S(12), row = S(24), cy = y;
    int words = is_word_list(o), open = field_is(r, o), edit = can_edit();
    int cw;
    if (words) {
        cw = choices_w(o);
    } else {
        const wchar_t *shown = open ? A.fe.t : o->value[0] ? o->value : L"(none)";
        cw = text_w(A.f_text, shown, -1) + S(18);
        if (cw < S(90)) cw = S(90);
        if (cw > ow / 2) cw = ow / 2;
    }
    int own_line = words && cw > ow * 3 / 5;
    int reset = edit && differs(o) && !open;
    int rw = reset ? text_w(A.f_small, L"Reset", -1) + S(12) : 0;
    int right = own_line ? x + w : x + w - cw;
    if (mode == DRAW) {
        fill(x, cy + S(5), S(2), S(14), C_BORDER);
        RECT nr = { ox, cy, right - rw - S(8), cy + row };
        text(A.f_text, o->name, -1, nr, C_TEXT, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
    if (reset) link(mode, mk(H_RESET, ri, k, 0), right - rw - S(4), cy + S(2), row - S(4), L"Reset");
    if (own_line) cy += row;
    int cx = own_line ? ox : x + w - cw;
    if (words) {
        int sub = 0;
        for (const wchar_t *p = o->values; *p; sub++) {
            while (*p == L' ') p++;
            const wchar_t *e = p;
            while (*e && *e != L' ') e++;
            if (e == p) break;
            int n = (int)(e - p), bw = text_w(A.f_small, p, n) + S(16);
            RECT b = { cx, cy + S(3), cx + bw, cy + row - S(3) };
            int sel = (int)wcslen(o->value) == n && !_wcsnicmp(o->value, p, (size_t)n);
            hid id = mk(H_CHOICE, ri, k, sub);
            if (mode == DRAW) {
                fill(b.left, b.top, bw, b.bottom - b.top, sel ? C_SEL : is_hover(id) ? C_SEL_IDLE : C_EDIT);
                text(A.f_small, p, n, b, sel ? C_TEXT : C_DIM, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            }
            control(mode, id, b, p, n);
            cx += bw + S(2);
            p = e;
        }
    } else {
        RECT b = { cx, cy + S(2), cx + cw, cy + row - S(2) };
        hid id = mk(H_FIELD, ri, k, 0);
        if (mode == DRAW) {
            fill(b.left, b.top, cw, b.bottom - b.top, open || is_hover(id) ? C_SEL_IDLE : C_EDIT);
            if (open) {
                fill(b.left, b.bottom - S(2), cw, S(2), A.f_msg[0] ? C_ERROR : C_ACCENT);
                draw_ebox(&A.fe, b, !A.f_saving, NULL, A.f_saving ? C_DIM : C_TEXT, &A.f_at);
            } else {
                ebox shown = { .max = 255 };
                eb_set(&shown, o->value);
                draw_ebox(&shown, b, 0, L"(none)", differs(o) ? C_ACCENT : C_TEXT, NULL);
            }
        }
        if (!open) control(mode, id, b, NULL, 0);
        else if (mode == LAYOUT && A.nhot < HOTS_MAX) A.hots[A.nhot++] = (hot){ b, id, NULL, 0 }; /* clicks in it place the caret */
    }
    cy += row;
    wchar_t help[700], range[64];
    range_text(o->values, range, 64);
    int n = _snwprintf(help, 700, L"%ls", o->help);
    if (range[0] && n >= 0 && n < 700) n += _snwprintf(help + n, 700 - (size_t)n, L"%ls%ls", n ? L"  \x2022  " : L"", range);
    if (differs(o) && n >= 0 && n < 700)
        _snwprintf(help + n, 700 - (size_t)n, L"%lsdefault %ls", n ? L"  \x2022  " : L"", o->def[0] ? o->def : L"none");
    help[699] = 0;
    cy += para(mode, A.f_small, help, ox, cy, ow - S(2), C_DIM);
    if (open && A.f_msg[0]) cy += S(2) + para(mode, A.f_small, A.f_msg, ox, cy + S(2), ow - S(2), C_ERROR);
    return cy - y + S(6);
}

/* Keys: a chip per key (x removes it), Add key (records the next one), Reset;
 * then what went wrong, or who else has the key just recorded. */
static int keys_row(const rec *r, int ri, int x, int y, int w, int mode)
{
    int lh = S(24), ch = S(20), lw = S(66), cx = x + lw, cy = y, edit = can_edit();
    wchar_t keys[KEYS_MAX][48];
    int nk = split_keys(r->keys, keys, KEYS_MAX);
    if (mode == DRAW) {
        RECT lr = { x, cy, x + lw, cy + lh };
        text(A.f_small, L"Keys", -1, lr, C_DIM, DT_SINGLELINE | DT_VCENTER);
    }
#define WRAP(cw) if (cx + (cw) > x + w && cx > x + lw) cx = x + lw, cy += lh
    for (int k = 0; k < nk; k++) {
        int nw = text_w(A.f_mono, keys[k], -1), xw = edit ? S(18) : 0, cw = nw + S(12) + xw;
        WRAP(cw);
        RECT c = { cx, cy + (lh - ch) / 2, cx + cw, cy + (lh + ch) / 2 };
        hid id = mk(H_KEY_DEL, ri, -1, k);
        if (mode == DRAW) {
            fill(c.left, c.top, cw, ch, C_EDIT);
            RECT nr = { c.left + S(6), c.top, c.left + S(6) + nw, c.bottom };
            text(A.f_mono, keys[k], -1, nr, C_TEXT, DT_SINGLELINE | DT_VCENTER);
            if (xw) {
                RECT xr = { c.right - xw, c.top, c.right, c.bottom };
                if (is_hover(id)) fill(xr.left, xr.top, xw, ch, C_SEL_IDLE);
                text(A.f_small, L"\x00D7", -1, xr, is_hover(id) ? C_TEXT : C_DIM, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            }
        }
        if (xw) control(mode, id, (RECT){ c.right - xw, c.top, c.right, c.bottom }, NULL, 0);
        cx += cw + S(4);
    }
    if (!nk && !edit && mode == DRAW) {
        RECT nr = { cx, cy, x + w, cy + lh };
        text(A.f_small, L"none", -1, nr, C_DIM, DT_SINGLELINE | DT_VCENTER);
    }
    if (edit) {
        int recording = A.rec_ri == ri;
        const wchar_t *add = recording ? L"Press a key\x2026  Esc cancels" : L"+ Add key";
        int aw = text_w(A.f_small, add, -1) + S(16);
        WRAP(aw);
        chip(mode, mk(H_KEY_ADD, ri, -1, 0), cx, cy + (lh - ch) / 2, ch, add, recording, recording ? C_TEXT : C_DIM);
        cx += aw + S(4);
        if (wcscmp(r->keys, r->defkeys)) {
            int rw = text_w(A.f_small, L"Reset", -1) + S(12);
            WRAP(rw);
            link(mode, mk(H_KEY_RESET, ri, -1, 0), cx, cy + (lh - ch) / 2, ch, L"Reset");
        }
    }
#undef WRAP
    cy += lh;
    if (A.msg_ri == ri && A.key_msg[0]) cy += para(mode, A.f_small, A.key_msg, x + lw, cy, w - lw, C_ERROR) + S(4);
    if (A.conf.ri == ri) {
        wchar_t t[600];
        size_t n = (size_t)_snwprintf(t, 600, L"%ls is also the key of ", A.conf.name);
        for (int i = 0; i < A.conf.n && n < 600; i++) {
            const rec *o = &A.s.recs[A.conf.others[i]];
            n += (size_t)_snwprintf(t + n, 600 - n, L"%ls%ls", i ? L", " : L"", o->title[0] ? o->title : o->id);
        }
        if (n < 600)
            _snwprintf(t + n, 600 - n, L". Shared, it runs the first of them that applies to what is selected.");
        t[599] = 0;
        cy += para(mode, A.f_small, t, x + lw, cy, w - lw, C_TEXT) + S(4);
        int bx = x + lw;
        bx += chip(mode, mk(H_CONF_ADD, ri, -1, 0), bx, cy, ch, L"Add anyway", 0, C_TEXT) + S(4);
        bx += chip(mode, mk(H_CONF_MOVE, ri, -1, 0), bx, cy, ch, L"Move it here", 0, C_TEXT) + S(4);
        chip(mode, mk(H_CONF_CANCEL, ri, -1, 0), bx, cy, ch, L"Cancel", 0, C_DIM);
        cy += ch + S(6);
    }
    return cy - y;
}

/* Where it shows up besides its keys: context menus, the palette. */
static int show_row(const rec *r, int ri, int x, int y, int w, int mode)
{
    int lh = S(24), ch = S(20), lw = S(66), cx = x + lw;
    if (mode == DRAW) {
        RECT lr = { x, y, x + lw, y + lh };
        text(A.f_small, L"Shown in", -1, lr, C_DIM, DT_SINGLELINE | DT_VCENTER);
    }
    if (wcschr(r->flags, L'M')) {
        int on = !wcschr(r->flags, L'm');
        cx += chip(mode, mk(H_SHOW_MENU, ri, -1, 0), cx, y + (lh - ch) / 2, ch, on ? L"\x2713 Context menus" : L"Context menus",
                   on, on ? C_TEXT : C_DIM) + S(4);
    }
    if (wcschr(r->flags, L'P')) {
        int on = !wcschr(r->flags, L'p');
        chip(mode, mk(H_SHOW_PAL, ri, -1, 0), cx, y + (lh - ch) / 2, ch, on ? L"\x2713 Command palette" : L"Command palette", on,
             on ? C_TEXT : C_DIM);
    }
    (void)w;
    return lh;
}

static void notes_text(const rec *r, wchar_t *buf, size_t cap)
{
    size_t n = 0;
    buf[0] = 0;
#define NOTE(...) n += (size_t)_snwprintf(buf + n, n < cap ? cap - n : 0, __VA_ARGS__)
    if (!wcscmp(r->state, L"needs")) NOTE(L"Inactive: it needs %ls, which is off.  ", r->needs);
    else if (r->needs[0]) NOTE(L"Needs %ls.  ", r->needs);
    if (wcschr(r->flags, L'r')) NOTE(L"Changes apply at the next start.  ");
    if (wcschr(r->flags, L'o')) NOTE(L"Off unless switched on.  ");
#undef NOTE
    if (n >= cap) buf[cap - 1] = 0;
    for (size_t i = wcslen(buf); i && buf[i - 1] == L' ';) buf[--i] = 0;
}

static int pad(void) { return S(14); }

/* A function's card, its top at y (list coordinates while laying out,
 * screen while drawing); returns its height. */
static int card(const rec *r, int ri, int y, int mode)
{
    int p = pad(), w = A.w - 2 * p, x = p;
    if (mode == DRAW) {
        uint64_t now = GetTickCount64();
        if (r->head) {
            wchar_t g[64];
            group_name(r, g, 64);
            RECT hr = { x, y - S(34) + S(10), A.w - p, y };
            text(A.f_head, g, -1, hr, C_TEXT, DT_SINGLELINE | DT_VCENTER);
            fill(x, y - S(1), w, S(1), C_SPLIT);
        } else {
            fill(x, y, w, S(1), C_SPLIT);
        }
        if (r->err) fill(0, y + S(6), S(3), r->h - S(12), C_ERROR);
        else if (r->marked_at && now - r->marked_at < MARK_MS) fill(0, y + S(6), S(3), r->h - S(12), C_ACCENT);
    }
    int cy = y + S(10);
    int on = !wcscmp(r->state, L"on"), needs = !wcscmp(r->state, L"needs");
    const wchar_t *pill = on ? L"On" : needs ? L"On (inactive)" : L"Off";
    int pw = text_w(A.f_small, pill, -1) + S(16);
    RECT pr = { A.w - p - pw, cy, A.w - p, cy + S(20) };
    hid pid = mk(H_PILL, ri, -1, 0);
    if (mode == DRAW) {
        fill(pr.left, pr.top, pw, pr.bottom - pr.top, on ? C_SEL : is_hover(pid) ? C_SEL_IDLE : C_EDIT);
        if (on && is_hover(pid)) fill(pr.left, pr.bottom - S(2), pw, S(2), C_ACCENT);
        text(A.f_small, pill, -1, pr, on ? C_TEXT : C_DIM, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        RECT tr = { x, cy, pr.left - S(10), cy + S(20) };
        text(A.f_bold, r->title[0] ? r->title : r->id, -1, tr, on ? C_TEXT : C_DIM, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        RECT ir = { x, cy + S(20), A.w - p, cy + S(36) };
        text(A.f_mono, r->id, -1, ir, C_DIM, DT_SINGLELINE | DT_VCENTER);
    }
    control(mode, pid, pr, NULL, 0);
    cy += S(36);
    if(builder_allowed()&&!wcsncmp(r->id,L"relay.",6)){link(mode,mk(H_RELAY_EDIT,ri,-1,0),x,cy,S(24),L"Edit command");cy+=S(30);}
    cy += para(mode, A.f_text, r->about, x, cy, w, C_DIM) + S(2);
    wchar_t notes[300];
    notes_text(r, notes, 300);
    if (notes[0]) cy += para(mode, A.f_small, notes, x, cy, w, needs ? C_ERROR : C_DIM) + S(2);
    if (wcschr(r->flags, L'k')) cy += S(2) + keys_row(r, ri, x, cy + S(2), w, mode);
    if (wcschr(r->flags, L'M') || wcschr(r->flags, L'P')) cy += show_row(r, ri, x, cy, w, mode);
    if (r->nopts) cy += S(6);
    for (int k = 0; k < r->nopts; k++) cy += option_row(r, ri, k, x, cy, w, mode);
    return cy + S(10) - y;
}

static int top_h(void) { return A.r_list.top; }

static const hot *find_hot(hid id)
{
    for (int i = 0; i < A.nhot; i++)
        if (hid_eq(A.hots[i].id, id)) return &A.hots[i];
    return NULL;
}

/* The focused control went away (a key removed, a search): the nearest one left. */
static void keep_focus(void)
{
    if (A.kf.kind == H_SEARCH || find_hot(A.kf)) return;
    const hot *best = NULL;
    for (int i = 0; i < A.nhot && !best; i++)
        if (A.hots[i].id.rec == A.kf.rec && A.hots[i].id.kind == A.kf.kind) best = &A.hots[i];
    for (int i = 0; i < A.nhot && !best; i++)
        if (A.hots[i].id.rec == A.kf.rec && A.hots[i].id.kind == H_KEY_ADD) best = &A.hots[i];
    for (int i = 0; i < A.nhot && !best; i++)
        if (A.hots[i].id.rec == A.kf.rec) best = &A.hots[i];
    A.kf = best ? best->id : mk(H_SEARCH, -1, -1, 0);
}

static void builder_layout(int mode);
static void builder_open(void);
static void builder_close(void);
static int builder_edit(const rec *r);
static void builder_context(const void *data,uint32_t size);
static void builder_record_stop(void);
static void builder_observe(void);
static void scroll_by(int dy);
static void reveal(hid id);
static void record_stop(void);
static const hot *hit(int x,int y);
static int caret_at(const ebox *e,int left,int x);
static void paint(void);

static void layout(void)
{
    int p = pad();
    A.r_commands=(RECT){A.w-p-S(106),S(12),A.w-p,S(40)};
    if(!builder_allowed()&&!A.br.open)SetRectEmpty(&A.r_commands);
    A.r_field = (RECT){ p, S(12), A.r_commands.right?A.r_commands.left-S(8):A.w-p, S(12) + S(28) };
    A.r_edit = (RECT){ A.w - p - S(110), A.r_field.bottom + S(8), A.w - p, A.r_field.bottom + S(8) + S(24) };
    A.r_list = (RECT){ 0, A.r_edit.bottom + S(10), A.w, A.h };
    A.nhot = 0;
    if (!A.have) return;
    if(A.br.open){builder_layout(LAYOUT);return;}
    int w = A.w - 2 * p, y = 0;
    A.errors_h = 0;
    if (A.s.nerr) {
        A.errors_h = S(12) + S(20);
        for (int e = 0; e < A.s.nerr; e++) A.errors_h += wrapped_h(A.f_small, A.s.errors[e], w - S(16)) + S(4);
        A.errors_h += S(10);
        y += A.errors_h + S(8);
    }
    int last_group = -1;
    for (int i = 0; i < A.s.n; i++) {
        rec *r = &A.s.recs[i];
        if (!matches(r)) {
            r->h = 0;
            continue;
        }
        r->head = r->group != last_group;
        last_group = r->group;
        if (r->head) y += S(34);
        r->y = y;
        r->h = card(r, i, y, LAYOUT);
        y += r->h;
    }
    A.content_h = y + S(20);
    A.laid_w = A.w;
    int max = A.content_h - (A.h - top_h());
    if (A.scroll > max) A.scroll = max;
    if (A.scroll < 0) A.scroll = 0;
    keep_focus();
}

static void present(void)
{
    if (!A.shared || !A.back_bits) return;
    if (A.waiting) {
        A.pending = 1; /* drawn again once the host lets go of the other buffer */
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

static int search_has_keys(void) { return !A.f_open && A.kf.kind == H_SEARCH && A.rec_ri < 0; }

static void paint_search(void)
{
    if(A.br.open){text(A.f_bold,L"Custom commands",-1,A.r_field,C_TEXT,DT_SINGLELINE|DT_VCENTER);return;}
    RECT f = A.r_field;
    fill(f.left, f.top, f.right - f.left, f.bottom - f.top, C_EDIT);
    if (A.focused && search_has_keys()) fill(f.left, f.bottom - S(2), f.right - f.left, S(2), C_ACCENT);
    draw_ebox(&A.q, f, search_has_keys(), L"Search: names, keys, what they do", C_TEXT, NULL);
}

static void paint_file_row(void)
{
    int p = pad(), y = A.r_edit.top, h = A.r_edit.bottom - A.r_edit.top;
    const wchar_t *what = !A.have ? L"" : !A.s.file[0] ? L"Defaults only (started with --settings=): nothing to change"
                                       : !A.s.exists ? L"No settings file yet: changes made here create it"
                                                     : A.s.file;
    int button = can_edit();
    RECT r = { p, y, button ? A.r_edit.left - S(10) : A.w - p, y + h };
    text(A.f_small, what, -1, r, C_DIM, DT_SINGLELINE | DT_VCENTER | (A.s.exists ? DT_PATH_ELLIPSIS : DT_END_ELLIPSIS));
    if (button) {
        RECT b = A.r_edit;
        fill(b.left, b.top, b.right - b.left, b.bottom - b.top, A.hover_edit ? C_SEL_IDLE : C_HEADER);
        text(A.f_text, A.s.exists ? L"Edit file" : L"Create file", -1, b, C_TEXT, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }
}

static void paint(void)
{
    if (!A.mdc) return;
    if (A.have && A.laid_w != A.w) layout();
    SetBkMode(A.mdc, TRANSPARENT);
    fill(0, 0, A.w, A.h, C_BG);

    /* the list, clipped below the fixed top */
    int top = top_h(), view = A.h - top;
    HRGN clip = CreateRectRgn(0, top, A.w, A.h);
    SelectClipRgn(A.mdc, clip);
    if (!A.have) {
        RECT r = { pad(), top + S(20), A.w - pad(), top + S(60) };
        text(A.f_text, L"Loading\x2026", -1, r, C_DIM, DT_SINGLELINE);
    } else if(A.br.open){
        int saved=SaveDC(A.mdc);SetViewportOrgEx(A.mdc,0,top-A.scroll,NULL);builder_layout(DRAW);RestoreDC(A.mdc,saved);
        if(A.content_h>view&&view>0){int th=view*view/A.content_h;if(th<S(24))th=S(24);
            int ty=top+(int)((int64_t)(view-th)*A.scroll/(A.content_h-view));fill(A.w-S(5),ty,S(3),th,C_BORDER);}
    } else {
        int y0 = top - A.scroll, p = pad();
        if (A.s.nerr) {
            int y = y0;
            fill(p, y, A.w - 2 * p, A.errors_h, RGB(58, 36, 36));
            RECT hr = { p + S(10), y + S(10), A.w - p - S(10), y + S(10) + S(20) };
            text(A.f_bold, A.s.nerr == 1 ? L"A problem in the settings file (the rest applies)"
                                          : L"Problems in the settings file (the rest applies)",
                 -1, hr, C_ERROR, DT_SINGLELINE | DT_VCENTER);
            y += S(12) + S(20);
            for (int e = 0; e < A.s.nerr; e++) y += para(DRAW, A.f_small, A.s.errors[e], p + S(10), y, A.w - 2 * p - S(16), C_TEXT) + S(4);
        }
        int shown = 0;
        for (int i = 0; i < A.s.n; i++) {
            const rec *r = &A.s.recs[i];
            if (!r->h) continue;
            shown++;
            int y = y0 + r->y;
            if (y + r->h < top || y - S(34) > A.h) continue;
            card(r, i, y, DRAW);
        }
        if (!shown) {
            RECT r = { p, y0 + S(20), A.w - p, y0 + S(60) };
            text(A.f_text, L"Nothing matches the search", -1, r, C_DIM, DT_SINGLELINE);
        }
        /* where the view is in the list */
        if (A.content_h > view && view > 0) {
            int th = view * view / A.content_h;
            if (th < S(24)) th = S(24);
            int ty = top + (int)((int64_t)(view - th) * A.scroll / (A.content_h - view));
            fill(A.w - S(5), ty, S(3), th, C_BORDER);
        }
    }
    SelectClipRgn(A.mdc, NULL);
    DeleteObject(clip);

    /* a change that didn't save: on top of the list's bottom, wherever it is scrolled; a click dismisses it */
    SetRectEmpty(&A.r_toast);
    if (A.notice[0]) {
        int p = pad(), tw = A.w - 2 * p, th = wrapped_h(A.f_small, A.notice, tw - S(20)) + S(20);
        A.r_toast = (RECT){ p, A.h - p - th, A.w - p, A.h - p };
        fill(p - S(1), A.r_toast.top - S(1), tw + S(2), th + S(2), A.notice_err ? C_ERROR : C_ACCENT);
        fill(p, A.r_toast.top, tw, th, A.notice_err ? RGB(58, 36, 36) : C_SEL);
        para(DRAW, A.f_small, A.notice, p + S(10), A.r_toast.top + S(10), tw - S(20), A.notice_err ? C_ERROR : C_TEXT);
    }

    fill(0, 0, A.w, top, C_BG);
    paint_search();
    paint_file_row();
    fill(A.r_commands.left,A.r_commands.top,A.r_commands.right-A.r_commands.left,A.r_commands.bottom-A.r_commands.top,C_HEADER);
    text(A.f_text,A.br.open?L"Functions":L"Commands",-1,A.r_commands,C_TEXT,DT_SINGLELINE|DT_VCENTER|DT_CENTER);
    fill(0, top - S(1), A.w, S(1), C_SPLIT);
    builder_observe();
    GdiFlush();
    present();
    if(A.br.open&&A.focused&&A.br.focus>=0&&A.br.focus<7){
        ebox *e=&A.br.fields[A.br.focus];RECT at=A.br.field_text[A.br.focus];
        panelp_rect c={at.left+text_w(A.f_text,e->t,e->caret),at.top+top-A.scroll,1,at.bottom-at.top};send_msg(PANELP_CARET,&c,sizeof c);
    }else if (A.focused && !A.br.open && (A.f_open || search_has_keys())) {
        const ebox *e = A.f_open ? &A.fe : &A.q;
        RECT at = A.f_open ? A.f_at : (RECT){ A.r_field.left + S(8), A.r_field.top, 0, A.r_field.bottom };
        panelp_rect c = { at.left + text_w(A.f_text, e->t, e->caret), at.top, 1, at.bottom - at.top };
        send_msg(PANELP_CARET, &c, sizeof c);
    }
}

/* ---- fonts and the shared pictures ------------------------------------------------------- */

static void make_fonts(void)
{
    HFONT *all[] = { &A.f_text, &A.f_bold, &A.f_small, &A.f_mono, &A.f_head };
    for (int i = 0; i < 5; i++)
        if (*all[i]) DeleteObject(*all[i]);
    measured_clear(); /* new fonts may get the old ones' handles */
#define FONT(pt10, weight, face) \
    CreateFontW(-MulDiv(pt10, A.dpi, 720), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, \
                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face)
    A.f_text = FONT(90, FW_NORMAL, L"Segoe UI");
    A.f_bold = FONT(90, FW_SEMIBOLD, L"Segoe UI");
    A.f_small = FONT(85, FW_NORMAL, L"Segoe UI");
    A.f_mono = FONT(85, FW_NORMAL, L"Consolas");
    A.f_head = FONT(105, FW_SEMIBOLD, L"Segoe UI");
#undef FONT
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
    A.waiting = 0; /* a new section: nothing of the old one is shown any more */
    A.laid_w = 0;
    layout();
    return A.shared && A.back;
}

/* ---- editing ------------------------------------------------------------------------------ */

static void field_open(const rec *r, const opt *o)
{
    if(wcslen(o->value)>(size_t)A.fe.max){notice(1,L"Use Edit command or Edit file to edit this longer value");return;}
    editor_rebound();
    A.f_open = 1;
    A.f_saving = 0;
    A.f_msg[0] = 0;
    _snwprintf(A.f_id, 64, L"%ls", r->id);
    _snwprintf(A.f_opt, 64, L"%ls", o->name);
    A.f_id[63] = A.f_opt[63] = 0;
    eb_set(&A.fe, o->value);
    A.fe.sel = 0; /* all selected: typing replaces it */
    layout();
}

static void field_close(void)
{
    editor_rebound();
    A.f_open = A.f_saving = 0;
    A.f_msg[0] = 0;
    layout();
}

/* Enter, Tab, or a click elsewhere: saved if it changed. */
static void field_commit(void)
{
    if (!A.f_open || A.f_saving) return;
    const opt *o = find_field_opt();
    wchar_t v[256];
    wcscpy(v, A.fe.t);
    wchar_t *s = v;
    while (*s == L' ') s++;
    for (size_t n = wcslen(s); n && s[n - 1] == L' ';) s[--n] = 0;
    if (!o || !wcscmp(s, o->value)) { /* nothing to save */
        field_close();
        return;
    }
    A.f_saving = 1;
    change_setting(A.f_id, A.f_opt, s, 1);
    layout();
}

static void key_message(int ri, const wchar_t *text)
{
    A.msg_ri = text ? ri : -1;
    _snwprintf(A.key_msg, 200, L"%ls", text ? text : L"");
    A.key_msg[199] = 0;
}

static void record_stop(void)
{
    if (A.rec_ri < 0) return;
    A.rec_ri = -1;
    uint32_t off = 0;
    send_msg(PANELP_CAPTURE, &off, 4);
    layout();
}

static void record_start(int ri)
{
    A.conf.ri = -1;
    key_message(-1, NULL);
    A.rec_ri = ri;
    uint32_t on = 1;
    send_msg(PANELP_CAPTURE, &on, 4);
    layout();
}

/* Saves r's keys plus `name`; with `move`, the functions in A.conf lose it. */
static void keys_add(int ri, const wchar_t *name, int move)
{
    const rec *r = &A.s.recs[ri];
    wchar_t keys[KEYS_MAX][48], list[400];
    int n = split_keys(r->keys, keys, KEYS_MAX);
    join_keys(keys, n, -1, name, list, 400);
    change c = { 0 };
    change_add(&c, L"keys", r->id, list);
    for (int i = 0; move && i < A.conf.n; i++) {
        const rec *o = &A.s.recs[A.conf.others[i]];
        wchar_t ok[KEYS_MAX][48], ol[400];
        int on = split_keys(o->keys, ok, KEYS_MAX), at = -1;
        for (int k = 0; k < on; k++)
            if (!wcscmp(ok[k], name)) at = k;
        join_keys(ok, on, at, NULL, ol, 400);
        change_add(&c, L"keys", o->id, ol);
    }
    A.conf.ri = -1;
    change_send(&c);
    layout();
}

/* A key pressed while recording: named as the settings file names it. */
static void recorded(UINT vk, uint32_t mods)
{
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) return; /* wait for the key */
    int ri = A.rec_ri;
    record_stop();
    A.eat_char = 1;
    if (vk == VK_ESCAPE && !mods) return;
    fn_binding b = { (uint8_t)vk, (uint8_t)(mods & (FN_CTRL | FN_SHIFT | FN_ALT)) };
    if (!fn_key_nameable(b)) {
        key_message(ri, L"That key can't be written in the settings file (or needs AltGr): try another");
        layout();
        return;
    }
    wchar_t name[48];
    fn_format_key(b, name, 48);
    const rec *r = &A.s.recs[ri];
    wchar_t keys[KEYS_MAX][48];
    int n = split_keys(r->keys, keys, KEYS_MAX);
    for (int k = 0; k < n; k++)
        if (!wcscmp(keys[k], name)) {
            wchar_t t[120];
            _snwprintf(t, 120, L"%ls is already one of its keys", name);
            key_message(ri, t);
            layout();
            return;
        }
    if (n >= KEYS_MAX) {
        key_message(ri, L"At most 4 keys: remove one first");
        layout();
        return;
    }
    A.conf.n = 0;
    for (int i = 0; i < A.s.n && A.conf.n < 16; i++) {
        if (i == ri) continue;
        wchar_t ok[KEYS_MAX][48];
        int on = split_keys(A.s.recs[i].keys, ok, KEYS_MAX);
        for (int k = 0; k < on; k++)
            if (!wcscmp(ok[k], name)) A.conf.others[A.conf.n++] = i;
    }
    if (!A.conf.n) {
        keys_add(ri, name, 0);
        return;
    }
    A.conf.ri = ri;
    _snwprintf(A.conf.name, 48, L"%ls", name);
    A.kf = mk(H_CONF_MOVE, ri, -1, 0);
    layout();
}

/* ---- custom commands and selected Explorer imports ------------------------------------ */
enum { BF_NAME, BF_TITLE, BF_PROGRAM, BF_ARGS, BF_ACTION, BF_TYPES, BF_KEYS, BF_COUNT };
static const wchar_t *builder_labels[] = {L"Command ID (optional)", L"Menu title",
                                          L"Program or script",     L"Arguments",
                                          L"Explorer action",       L"File extensions (empty: all files)",
                                          L"Keyboard shortcut(s)"};
static const wchar_t *builder_hints[] = {L"letters, numbers, - or _",
                                         L"How it appears in menus",
                                         L"Program path or name",
                                         L"%1 selected file, %* all files, %D folder",
                                         L"Exact menu text or submenu > item",
                                         L"txt md json",
                                         L"e.g. Ctrl+Shift+E"};
static void builder_context_request(void)
{
    A.br.context_ticket = (int)++A.req;
    uint32_t request = A.br.context_ticket;
    send_msg(PANELP_RELAY_GET_CONTEXT, &request, 4);
}
static void builder_to_fields(void)
{
    editor_rebound();
    const char *strings[] = {A.br.draft.name,   A.br.draft.title, A.br.draft.program, A.br.draft.arguments,
                             A.br.draft.action, A.br.draft.types, A.br.draft.keys};
    const int max[] = {32, 79, 511, 1023, 127, 63, 191};
    for (int i = 0; i < BF_COUNT; ++i) {
        wchar_t text[2048];
        A.br.fields[i].max = max[i];
        if (!MultiByteToWideChar(CP_UTF8, 0, strings[i], -1, text, 2048))
            text[0] = 0;
        eb_set(&A.br.fields[i], text);
    }
}
static int builder_from_fields(void)
{
    char *strings[] = {A.br.draft.name,   A.br.draft.title, A.br.draft.program, A.br.draft.arguments,
                       A.br.draft.action, A.br.draft.types, A.br.draft.keys};
    const int sizes[] = {sizeof A.br.draft.name,      sizeof A.br.draft.title,  sizeof A.br.draft.program,
                         sizeof A.br.draft.arguments, sizeof A.br.draft.action, sizeof A.br.draft.types,
                         sizeof A.br.draft.keys};
    for (int i = 0; i < BF_COUNT; ++i)
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, A.br.fields[i].t, -1, strings[i], sizes[i],
                                 NULL, NULL)) {
            _snwprintf(A.br.message, 400, L"%ls is too long", builder_labels[i]);
            return -1;
        }
    return 0;
}
static void builder_record_stop(void)
{
    if (!A.br.recording)
        return;
    A.br.recording = 0;
    uint32_t capture = 0;
    send_msg(PANELP_CAPTURE, &capture, 4);
}
static void builder_open(void)
{
    if (A.wait_req) {
        notice(1, L"Wait for the current settings change to finish");
        return;
    }
    editor_rebound();
    builder_record_stop();
    A.f_open = 0;
    if (A.rec_ri >= 0)
        record_stop();
    relay_draft_init(&A.br.draft);
    A.br.base_rev = (int)A.s.rev;
    A.br.open = 1;
    A.br.focus = BF_TITLE;
    A.kf = mk(H_B_FIELD, -1, BF_TITLE, 0);
    A.br.message[0] = A.br.preview[0] = A.br.source[0] = A.br.program_status[0] = 0;
    ++A.br.ticket;
    A.notice[0] = 0;
    A.br.saving = 0;
    A.br.query.max = 127;
    eb_set(&A.br.query, L"");
    builder_to_fields();
    A.scroll = 0;
    builder_context_request();
    A.laid_w = 0;
}
static const wchar_t *record_option(const rec *r, const wchar_t *name)
{
    for (int i = 0; i < r->nopts; ++i)
        if (!wcscmp(r->opts[i].name, name))
            return r->opts[i].value;
    return L"";
}
static int builder_edit(const rec *r)
{
    const wchar_t *run = record_option(r, L"run"), *item = record_option(r, L"item"),
                  *verb = record_option(r, L"verb"), *sendto = record_option(r, L"sendto");
    if (!run[0] && !item[0] && !verb[0] && !sendto[0]) {
        notice(1, L"This relay uses the existing option editor");
        return -1;
    }
    builder_open();
    if (!A.br.open)
        return -1;
    editor_rebound();
    A.br.draft.editing = 1;
    A.br.draft.kind = run[0] ? RELAY_RUN : item[0] ? RELAY_ITEM : verb[0] ? RELAY_VERB : RELAY_SENDTO;
    wchar_t program[2048];
    const wchar_t *arguments = L"";
    program[0] = 0;
    if (run[0])
        relay_split(run, program, 2048, &arguments);
    const wchar_t *strings[] = {r->id + 6,
                                r->title,
                                program,
                                arguments,
                                item[0] ? item : verb[0] ? verb : sendto,
                                record_option(r, L"types"),
                                r->keys};
    for (int i = 0; i < BF_COUNT; ++i) {
        if (wcslen(strings[i]) > (size_t)A.br.fields[i].max) {
            editor_rebound();
            A.br.open = 0;
            notice(1, L"This command exceeds the form's limits; use Edit file");
            return -1;
        }
        eb_set(&A.br.fields[i], strings[i]);
    }
    const wchar_t *on = record_option(r, L"on");
    A.br.draft.scopes = on[0] ? 0 : RELAY_FILES | RELAY_FOLDERS;
    if (wcsstr(on, L"files"))
        A.br.draft.scopes |= RELAY_FILES;
    if (wcsstr(on, L"folders"))
        A.br.draft.scopes |= RELAY_FOLDERS;
    if (wcsstr(on, L"background"))
        A.br.draft.scopes |= RELAY_BACKGROUND;
    const wchar_t *handler = record_option(r, L"handler");
    WideCharToMultiByte(CP_UTF8, 0, handler, -1, A.br.draft.handler, sizeof A.br.draft.handler, NULL, NULL);
    A.br.draft.menu = wcschr(r->flags, L'm') == NULL;
    A.br.draft.palette = wcschr(r->flags, L'p') == NULL;
    A.br.draft.count_one = !wcscmp(record_option(r, L"count"), L"one");
    static const wchar_t *windows[] = {L"auto", L"panel", L"normal", L"minimized", L"hidden"};
    for (int i = 0; i < 5; ++i)
        if (!wcscmp(record_option(r, L"window"), windows[i]))
            A.br.draft.window = i;
    A.br.draft.fallback = wcscmp(record_option(r, L"fallback"), L"off") != 0;
    A.br.focus = BF_TITLE;
    A.kf = mk(H_B_FIELD, -1, BF_TITLE, 0);
    builder_from_fields();
    return 0;
}
static void builder_close(void)
{
    if (A.br.saving)
        return;
    editor_rebound();
    builder_record_stop();
    ++A.br.ticket;
    A.br.open = 0;
    A.laid_w = 0;
    A.kf = mk(H_SEARCH, -1, -1, 0);
    A.scroll = 0;
}
static int builder_field_visible(int i)
{
    return A.br.draft.kind == RELAY_RUN ? i != BF_ACTION : i != BF_PROGRAM && i != BF_ARGS;
}
static int builder_conflict(void)
{
    char list[192];
    if (!WideCharToMultiByte(CP_UTF8, 0, A.br.fields[BF_KEYS].t, -1, list, sizeof list, NULL, NULL))
        return 0;
    for (char *p = list; p;) {
        char *comma = strchr(p, ',');
        if (comma)
            *comma++ = 0;
        fn_binding want;
        if (!fn_parse_key(p, &want))
            for (int i = 0; i < A.s.n; ++i) {
                const rec *r = &A.s.recs[i];
                if (!wcscmp(r->state, L"off") ||
                    (!wcsncmp(r->id, L"relay.", 6) && !wcscmp(r->id + 6, A.br.fields[BF_NAME].t)))
                    continue;
                wchar_t names[KEYS_MAX][48];
                int n = split_keys(r->keys, names, KEYS_MAX);
                for (int k = 0; k < n; ++k) {
                    char name[192];
                    fn_binding key;
                    WideCharToMultiByte(CP_UTF8, 0, names[k], -1, name, sizeof name, NULL, NULL);
                    if (!fn_parse_key(name, &key) && key.vk == want.vk && key.mods == want.mods) {
                        _snwprintf(A.br.message, 400,
                                   L"Shortcut also belongs to %ls. Change it or explicitly share it.",
                                   r->title);
                        return 1;
                    }
                }
            }
        p = comma;
    }
    return 0;
}
typedef struct builder_resolve {
    int ticket;
    wchar_t program[2048], status[240];
} builder_resolve;
static DWORD WINAPI builder_resolve_worker(void *arg)
{
    builder_resolve *r = arg;
    wchar_t expanded[32768], resolved[32768];
    DWORD n = ExpandEnvironmentStringsW(r->program, expanded, 32768);
    int exists = 0;
    if (n && n <= 32768) {
        DWORD got = SearchPathW(NULL, expanded, L".exe", 32768, resolved, NULL);
        exists = got && got < 32768 && GetFileAttributesW(resolved) != INVALID_FILE_ATTRIBUTES;
        if (!exists) {
            DWORD a = GetFileAttributesW(expanded);
            exists = a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
        }
        if (!exists) {
            wchar_t key[2200];
            _snwprintf(key, 2200, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\%ls", expanded);
            for (int hive = 0; hive < 2 && !exists; ++hive) {
                HKEY h;
                DWORD bytes = sizeof resolved, type = 0;
                if (RegOpenKeyExW(hive ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, key, 0, KEY_READ, &h) ==
                    ERROR_SUCCESS) {
                    if (RegQueryValueExW(h, NULL, NULL, &type, (BYTE *)resolved, &bytes) == ERROR_SUCCESS &&
                        (type == REG_SZ || type == REG_EXPAND_SZ)) {
                        resolved[32767] = 0;
                        DWORD a = GetFileAttributesW(resolved);
                        exists = a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
                    }
                    RegCloseKey(h);
                }
            }
        }
    }
    wcscpy(r->status, exists ? L"Program found. Preview does not run it."
                             : L"Program not found in this environment. You can save it for later use.");
    if (!PostMessageW(A.hwnd, WM_BUILDER_RESOLVED, 0, (LPARAM)r))
        free(r);
    return 0;
}
static void builder_auto_name(void)
{
    if (!A.br.fields[BF_NAME].len && !A.br.draft.editing) {
        relay_catalog_entry entry = {0};
        relay_draft suggested;
        WideCharToMultiByte(CP_UTF8, 0, A.br.fields[BF_TITLE].t, -1, entry.title, sizeof entry.title, NULL,
                            NULL);
        relay_catalog_draft(&entry, &suggested);
        char base[33];
        snprintf(base, sizeof base, "%s", suggested.name);
        for (int suffix = 0; suffix < 192; ++suffix) {
            char id[48];
            if (suffix)
                snprintf(suggested.name, sizeof suggested.name, "%.26s_%d", base, suffix + 1);
            snprintf(id, sizeof id, "relay.%s", suggested.name);
            int taken = 0;
            for (int i = 0; i < A.s.n; ++i) {
                char existing[192];
                WideCharToMultiByte(CP_UTF8, 0, A.s.recs[i].id, -1, existing, sizeof existing, NULL, NULL);
                if (!strcmp(existing, id))
                    taken = 1;
            }
            if (!taken)
                break;
        }
        wchar_t name[40];
        MultiByteToWideChar(CP_UTF8, 0, suggested.name, -1, name, 40);
        eb_set(&A.br.fields[BF_NAME], name);
    }
}
static void builder_preview(void)
{
    builder_auto_name();
    if (builder_from_fields())
        return;
    char why[200];
    if (relay_draft_validate(&A.br.draft, why, sizeof why)) {
        MultiByteToWideChar(CP_UTF8, 0, why, -1, A.br.message, 400);
        return;
    }
    A.br.message[0] = 0;
    A.br.preview[0] = 0;
    if (A.br.draft.kind != RELAY_RUN) {
        wchar_t action[512];
        MultiByteToWideChar(CP_UTF8, 0, A.br.draft.action, -1, action, 512);
        _snwprintf(A.br.preview, 32768, L"Explorer action: %ls\n%ls", action,
                   A.br.draft.handler[0]
                       ? (A.br.draft.fallback
                              ? L"Tries the selected extension, then the full Explorer menu if needed."
                              : L"Only the selected extension is requested.")
                       : L"Uses the existing Explorer helper.");
        return;
    }
    if (!A.br.context || A.br.context_flags) {
        wcscpy(A.br.message, L"Select fewer files and Refresh selection to make a complete preview.");
        return;
    }
    wchar_t line[RELAY_LINE_MAX];
    if (relay_draft_line(&A.br.draft, line, RELAY_LINE_MAX) < 0 ||
        relay_expand(line, A.br.paths, A.br.count, -1, A.br.dir, A.br.preview, 32768) < 0) {
        wcscpy(A.br.message, L"The resolved command exceeds Windows' command-line limit.");
        return;
    }
    if (!A.br.resolve_busy) {
        builder_resolve *r = calloc(1, sizeof *r);
        if (r) {
            r->ticket = ++A.br.ticket;
            wcscpy(r->program, A.br.fields[BF_PROGRAM].t);
            HANDLE thread = CreateThread(NULL, 0, builder_resolve_worker, r, 0, NULL);
            if (thread) {
                CloseHandle(thread);
                A.br.resolve_busy = 1;
                wcscpy(A.br.program_status, L"Checking the program...");
            } else
                free(r);
        }
    }
}
static void builder_save(void)
{
    if (A.paste.target) {
        wcscpy(A.br.message, L"Wait for the pending paste to finish before saving");
        return;
    }
    if (A.wait_req || A.br.saving) {
        wcscpy(A.br.message, L"Wait for the previous save to finish");
        return;
    }
    if (!A.s.file[0]) {
        wcscpy(A.br.message, L"There is no settings file to save to");
        return;
    }
    builder_auto_name();
    if (builder_from_fields())
        return;
    char why[200];
    if (relay_draft_validate(&A.br.draft, why, sizeof why)) {
        MultiByteToWideChar(CP_UTF8, 0, why, -1, A.br.message, 400);
        return;
    }
    if (!A.br.draft.share_key && builder_conflict())
        return;
    A.br.draft.request = ++A.req;
    A.br.draft.revision = (uint32_t)A.br.base_rev;
    A.wait_req = A.br.draft.request;
    A.last_request = A.wait_req;
    A.wait_field = 0;
    A.br.saving = 1;
    wcscpy(A.br.message, L"Saving...");
    send_msg(PANELP_RELAY_SAVE, &A.br.draft, sizeof A.br.draft);
    SetTimer(A.hwnd, TIMER_WAIT, WAIT_MS, NULL);
}
static DWORD WINAPI builder_catalogue_worker(void *arg)
{
    wchar_t *file = arg;
    relay_catalog *c = malloc(sizeof *c);
    if (c)
        relay_catalog_scan(c, file);
    if (!PostMessageW(A.hwnd, WM_BUILDER_CATALOGUE, 0, (LPARAM)c))
        free(c);
    free(file);
    return 0;
}
static void builder_catalogue_start(void)
{
    if (A.br.catalogue_busy)
        return;
    const wchar_t *file = A.br.count ? A.br.paths[0] : L"";
    size_t n = (wcslen(file) + 1) * 2;
    wchar_t *p = malloc(n);
    if (!p)
        return;
    memcpy(p, file, n);
    HANDLE thread = CreateThread(NULL, 0, builder_catalogue_worker, p, 0, NULL);
    if (thread) {
        CloseHandle(thread);
        A.br.catalogue_busy = 1;
    } else
        free(p);
}
static int builder_catalogue_match(const relay_catalog_entry *e)
{
    if (!A.br.query.len)
        return 1;
    wchar_t value[1024], query[2048];
    char all[1200];
    snprintf(all, sizeof all, "%s %s %s", e->title, e->action, e->types);
    MultiByteToWideChar(CP_UTF8, 0, all, -1, value, 1024);
    wcscpy(query, A.br.query.t);
    CharLowerW(query);
    CharLowerW(value);
    return wcsstr(value, query) != NULL;
}
static int builder_button(int mode, int action, int sub, int x, int y, const wchar_t *label, int on)
{
    return chip(mode, mk(H_B_ACTION, action, -1, sub), x, y, S(24), label, on, C_TEXT) + S(6);
}
static int builder_field(int mode, int i, int y)
{
    int p = pad(), w = A.w - 2 * p;
    RECT label = {p, y, A.w - p, y + S(18)}, r = {p, y + S(21), A.w - p, y + S(51)};
    if (mode == DRAW) {
        text(A.f_small, builder_labels[i], -1, label, C_DIM, DT_SINGLELINE | DT_VCENTER);
        fill(r.left, r.top, w, r.bottom - r.top, C_EDIT);
        int focused = A.br.focus == i && !A.br.recording;
        draw_ebox(&A.br.fields[i], r, focused, builder_hints[i], C_TEXT, &A.br.field_text[i]);
        if (focused)
            fill(r.left, r.bottom - S(2), w, S(2), C_ACCENT);
    } else {
        A.br.field_rects[i] = r;
        if (!(i == BF_NAME && A.br.draft.editing))
            control(mode, mk(H_B_FIELD, -1, i, 0), r, NULL, 0);
    }
    return S(62);
}
static void builder_layout(int mode)
{
    int p = pad(), y = S(10), x = p;
    if (A.br.open == 2) {
        if (mode == DRAW) {
            RECT r = {p, y, A.w - p, y + S(25)};
            text(A.f_bold, L"Import an Explorer action", -1, r, C_TEXT, DT_SINGLELINE);
        }
        y += S(32);
        y += para(mode, A.f_small,
                  L"Installed verbs, menu extensions and Send to. Selecting an entry prepares a draft; it "
                  L"does not run an action.",
                  p, y, A.w - 2 * p, C_DIM) +
             S(8);
        x += builder_button(mode, BA_BACK, 0, x, y, L"Back", 0);
        builder_button(mode, BA_REFRESH_CATALOGUE, 0, x, y, L"Refresh actions", 0);
        y += S(36);
        RECT query = {p, y, A.w - p, y + S(28)};
        if (mode == DRAW) {
            fill(p, y, A.w - 2 * p, S(28), C_EDIT);
            draw_ebox(&A.br.query, query, A.br.focus == BF_COUNT, L"Filter installed actions", C_TEXT, NULL);
        } else
            control(mode, mk(H_B_FIELD, -1, BF_COUNT, 0), query, NULL, 0);
        y += S(40);
        if (A.br.message[0])
            y += para(mode, A.f_text, A.br.message, p, y, A.w - 2 * p, C_ERROR) + S(8);
        if (A.br.catalogue_busy)
            y += para(mode, A.f_text, L"Reading installed action metadata...", p, y, A.w - 2 * p, C_DIM);
        if (A.br.catalogue)
            for (unsigned i = 0; i < A.br.catalogue->count; ++i) {
                const relay_catalog_entry *e = &A.br.catalogue->entries[i];
                if (!builder_catalogue_match(e))
                    continue;
                wchar_t title[256], detail[640];
                MultiByteToWideChar(CP_UTF8, 0, e->title, -1, title, 256);
                _snwprintf(detail, 640, L"%ls%ls%ls",
                           e->kind == RELAY_SENDTO ? L"Send to"
                           : e->kind == RELAY_RUN  ? L"Explorer command"
                           : e->kind == RELAY_VERB ? L"Explorer verb"
                                                   : L"Menu extension",
                           e->types[0] ? L" | ." : L"", L"");
                if (e->types[0]) {
                    wchar_t types[256];
                    MultiByteToWideChar(CP_UTF8, 0, e->types, -1, types, 256);
                    wcscat(detail, types);
                }
                RECT r = {p, y, A.w - p, y + S(50)};
                hid id = mk(H_B_ACTION, BA_PICK, -1, (int)i);
                if (mode == DRAW) {
                    fill(p, y, A.w - 2 * p, S(50), is_hover(id) ? C_SEL_IDLE : C_EDIT);
                    RECT t = {p + S(8), y + S(5), A.w - p - S(8), y + S(25)};
                    text(A.f_text, title, -1, t, C_TEXT, DT_SINGLELINE | DT_END_ELLIPSIS);
                    t.top = y + S(27);
                    t.bottom = y + S(46);
                    text(A.f_small, detail, -1, t, C_DIM, DT_SINGLELINE | DT_END_ELLIPSIS);
                }
                control(mode, id, r, NULL, 0);
                y += S(56);
            }
        if (A.br.catalogue && A.br.catalogue->omitted)
            y +=
                para(mode, A.f_small,
                     L"The installed-action list reached its limit; use filters or a manual Explorer action.",
                     p, y, A.w - 2 * p, C_DIM);
    } else {
        if (mode == DRAW) {
            RECT r = {p, y, A.w - p, y + S(25)};
            text(A.f_bold, A.br.draft.editing ? L"Edit command" : L"New command", -1, r, C_TEXT,
                 DT_SINGLELINE);
        }
        y += S(32);
        if (A.br.source[0])
            y += para(mode, A.f_small, A.br.source, p, y, A.w - 2 * p, C_DIM) + S(6);
        x = p;
        x += builder_button(mode, BA_CANCEL, 0, x, y, L"Cancel", 0);
        builder_button(mode, BA_IMPORT, 0, x, y, L"Import Explorer action", 0);
        y += S(36);
        for (int i = 0; i < BF_COUNT; ++i)
            if (builder_field_visible(i)) {
                y += builder_field(mode, i, y);
                if (i == BF_PROGRAM) {
                    builder_button(mode, BA_BROWSE, 0, p, y, L"Browse...", 0);
                    y += S(34);
                }
                if (i == BF_ARGS) {
                    x = p;
                    x += builder_button(mode, BA_TOKEN, 0, x, y, L"%1 File", 0);
                    x += builder_button(mode, BA_TOKEN, 1, x, y, L"%* All files", 0);
                    builder_button(mode, BA_TOKEN, 2, x, y, L"%D Folder", 0);
                    y += S(34);
                }
                if (i == BF_KEYS) {
                    x = p;
                    x += builder_button(mode, BA_RECORD, 0, x, y,
                                        A.br.recording ? L"Press shortcut... (Esc cancels)"
                                                       : L"Record shortcut",
                                        A.br.recording);
                    if (x + S(120) > A.w - p) {
                        x = p;
                        y += S(30);
                    }
                    builder_button(mode, BA_SHARE, 0, x, y, L"Share shortcut", A.br.draft.share_key);
                    y += S(34);
                }
            }
        if (A.br.draft.kind != RELAY_RUN) {
            if (A.br.draft.handler[0]) {
                y += para(mode, A.f_small, L"Enter the exact menu text, including submenu names.", p, y,
                          A.w - 2 * p, C_DIM) +
                     S(6);
                builder_button(mode, BA_FALLBACK, 0, p, y, L"Use full Explorer menu if needed",
                               A.br.draft.fallback);
                y += S(34);
            }
        }
        y += para(mode, A.f_small, L"Where it appears", p, y, A.w - 2 * p, C_DIM) + S(6);
        x = p;
        x += builder_button(mode, BA_SCOPE, RELAY_FILES, x, y, L"Files", A.br.draft.scopes & RELAY_FILES);
        x += builder_button(mode, BA_SCOPE, RELAY_FOLDERS, x, y, L"Folders",
                            A.br.draft.scopes & RELAY_FOLDERS);
        builder_button(mode, BA_SCOPE, RELAY_BACKGROUND, x, y, L"Empty space",
                       A.br.draft.scopes & RELAY_BACKGROUND);
        y += S(34);
        x = p;
        x += builder_button(mode, BA_MENU, 0, x, y, L"Context menu", A.br.draft.menu);
        builder_button(mode, BA_PALETTE, 0, x, y, L"Command palette", A.br.draft.palette);
        y += S(34);
        x = p;
        x += builder_button(mode, BA_COUNT, 0, x, y, L"Any selection", !A.br.draft.count_one);
        builder_button(mode, BA_COUNT, 1, x, y, L"One item", A.br.draft.count_one);
        y += S(34);
        if (A.br.draft.kind == RELAY_RUN) {
            y += para(mode, A.f_small, L"Window", p, y, A.w - 2 * p, C_DIM) + S(6);
            static const wchar_t *windows[] = {L"Auto", L"Panel", L"Normal", L"Minimized", L"Hidden"};
            x = p;
            for (int i = 0; i < 5; ++i) {
                int w = text_w(A.f_small, windows[i], -1) + S(22);
                if (x + w > A.w - p) {
                    x = p;
                    y += S(30);
                }
                x += builder_button(mode, BA_WINDOW, i, x, y, windows[i], A.br.draft.window == (uint32_t)i);
            }
            y += S(34);
        }
        wchar_t context[240];
        _snwprintf(context, 240, L"Preview for %u selected item%ls%ls", A.br.count,
                   A.br.count == 1 ? L"" : L"s", A.br.context_flags ? L" (selection too large)" : L"");
        y += para(mode, A.f_small, context, p, y, A.w - 2 * p, C_DIM) + S(6);
        if (A.br.dir && A.br.dir[0])
            y += para(mode, A.f_small, A.br.dir, p, y, A.w - 2 * p, C_DIM) + S(6);
        x = p;
        x += builder_button(mode, BA_REFRESH, 0, x, y, L"Refresh selection", 0);
        builder_button(mode, BA_PREVIEW, 0, x, y, L"Preview", 0);
        y += S(34);
        if (A.br.preview[0]) {
            wchar_t rule[200];
            if (A.br.draft.kind == RELAY_RUN) {
                wchar_t template[RELAY_LINE_MAX];
                relay_draft_line(&A.br.draft, template, RELAY_LINE_MAX);
                int all = relay_takes_all(template);
                _snwprintf(rule, 200, L"%ls%ls",
                           all || A.br.count < 2
                               ? L"One command invocation."
                               : L"One invocation per selected item; first invocation shown.",
                           !all && A.br.count > 16 ? L" More than 16 items will be refused." : L"");
                y += para(mode, A.f_small, rule, p, y, A.w - 2 * p, C_DIM) + S(6);
            }
            wchar_t display[4097];
            wcsncpy(display, A.br.preview, 4096);
            display[4096] = 0;
            y += para(mode, A.f_mono, display, p, y, A.w - 2 * p, C_TEXT) + S(8);
            if (wcslen(A.br.preview) > 4096)
                y += para(mode, A.f_small, L"Display shortened; Copy preview copies the complete command.", p,
                          y, A.w - 2 * p, C_DIM) +
                     S(6);
            builder_button(mode, BA_COPY, 0, p, y, L"Copy preview", 0);
            y += S(34);
        }
        if (A.br.program_status[0])
            y += para(mode, A.f_small, A.br.program_status, p, y, A.w - 2 * p, C_DIM) + S(6);
        if (A.br.message[0])
            y += para(mode, A.f_text, A.br.message, p, y, A.w - 2 * p,
                      (A.br.saving || !wcsncmp(A.br.message, L"Saved.", 6)) ? C_ACCENT : C_ERROR) +
                 S(10);
        builder_button(mode, BA_SAVE, 0, p, y,
                       A.br.saving          ? L"Saving..."
                       : A.br.draft.editing ? L"Save changes"
                                            : L"Add command",
                       0);
        y += S(38);
    }
    if (mode == LAYOUT) {
        A.content_h = y + S(20);
        A.laid_w = A.w;
        int max = A.content_h - (A.h - top_h());
        if (A.scroll > max)
            A.scroll = max;
        if (A.scroll < 0)
            A.scroll = 0;
    }
}
static void builder_action(int action, int sub)
{
    if (A.br.saving)
        return;
    switch (action) {
    case BA_CANCEL:
        builder_close();
        break;
    case BA_IMPORT:
        editor_rebound();
        A.br.open = 2;
        A.br.focus = BF_COUNT;
        A.kf = mk(H_B_FIELD, -1, BF_COUNT, 0);
        A.scroll = 0;
        builder_context_request();
        builder_catalogue_start();
        break;
    case BA_BACK:
        editor_rebound();
        A.br.open = 1;
        A.br.focus = BF_TITLE;
        A.kf = mk(H_B_FIELD, -1, BF_TITLE, 0);
        A.scroll = 0;
        break;
    case BA_REFRESH_CATALOGUE:
        free(A.br.catalogue);
        A.br.catalogue = NULL;
        builder_context_request();
        builder_catalogue_start();
        break;
    case BA_PICK:
        if (A.br.catalogue && sub >= 0 && (unsigned)sub < A.br.catalogue->count) {
            relay_draft candidate;
            if (relay_catalog_draft(&A.br.catalogue->entries[sub], &candidate)) {
                wcscpy(A.br.message,
                       L"This imported command exceeds the form's limits; it was not copied or saved.");
                break;
            }
            A.br.draft = candidate;
            builder_to_fields();
            wcscpy(A.br.source, A.br.catalogue->entries[sub].kind == RELAY_SENDTO
                                    ? L"Imported from Windows Send to"
                                    : L"Imported from Explorer action metadata");
            A.br.base_rev = (int)A.s.rev;
            A.br.preview[0] = A.br.message[0] = A.br.program_status[0] = 0;
            A.br.open = 1;
            A.br.focus = BF_TITLE;
            A.kf = mk(H_B_FIELD, -1, BF_TITLE, 0);
            A.scroll = 0;
        }
        break;
    case BA_BROWSE: {
        A.br.browse_ticket = (int)++A.req;
        uint32_t request = A.br.browse_ticket;
        send_msg(PANELP_RELAY_BROWSE, &request, 4);
        break;
    }
    case BA_TOKEN: {
        static const wchar_t *tokens[] = {L"%1", L"%*", L"%D"};
        A.br.focus = BF_ARGS;
        A.kf = mk(H_B_FIELD, -1, BF_ARGS, 0);
        eb_insert(&A.br.fields[BF_ARGS], tokens[sub], 2);
        A.br.preview[0] = A.br.program_status[0] = 0;
        ++A.br.ticket;
        break;
    }
    case BA_RECORD:
        if (A.br.recording)
            builder_record_stop();
        else {
            A.br.recording = 1;
            uint32_t capture = 1;
            send_msg(PANELP_CAPTURE, &capture, 4);
        }
        break;
    case BA_SHARE:
        A.br.draft.share_key = !A.br.draft.share_key;
        break;
    case BA_SCOPE:
        A.br.draft.scopes ^= (uint32_t)sub;
        break;
    case BA_MENU:
        A.br.draft.menu = !A.br.draft.menu;
        break;
    case BA_PALETTE:
        A.br.draft.palette = !A.br.draft.palette;
        break;
    case BA_COUNT:
        A.br.draft.count_one = sub;
        break;
    case BA_WINDOW:
        A.br.draft.window = sub;
        break;
    case BA_FALLBACK:
        A.br.draft.fallback = !A.br.draft.fallback;
        A.br.preview[0] = 0;
        break;
    case BA_REFRESH:
        builder_context_request();
        break;
    case BA_PREVIEW:
        builder_preview();
        break;
    case BA_COPY:
        copy_text(A.br.preview, (int)wcslen(A.br.preview));
        break;
    case BA_SAVE:
        builder_save();
        break;
    }
    A.laid_w = 0;
}
static void builder_changed(void)
{
    A.br.preview[0] = A.br.program_status[0] = A.br.message[0] = 0;
    ++A.br.ticket;
    A.laid_w = 0;
}
static void builder_key(UINT vk, uint32_t mods)
{
    if (A.br.saving)
        return;
    if (A.br.recording) {
        if (vk == VK_ESCAPE) {
            builder_record_stop();
            return;
        }
        fn_binding key = {(uint8_t)vk, (uint8_t)mods};
        if (!fn_key_nameable(key))
            return;
        wchar_t name[48];
        fn_format_key(key, name, 48);
        eb_set(&A.br.fields[BF_KEYS], name);
        builder_record_stop();
        builder_changed();
        A.br.focus = BF_KEYS;
        A.kf = mk(H_B_FIELD, -1, BF_KEYS, 0);
        A.eat_char = 1;
        return;
    }
    if (vk == VK_TAB) {
        int at = -1;
        for (int i = 0; i < A.nhot; ++i)
            if (hid_eq(A.hots[i].id, A.kf))
                at = i;
        int next = (at + ((mods & PANELP_M_SHIFT) ? A.nhot - 1 : 1)) % (A.nhot ? A.nhot : 1);
        if (A.nhot) {
            A.kf = A.hots[next].id;
            A.br.focus = A.kf.kind == H_B_FIELD ? A.kf.opt : -1;
            reveal(A.kf);
        }
        return;
    }
    if (vk == VK_ESCAPE) {
        if (A.br.open == 2)
            builder_action(BA_BACK, 0);
        else
            builder_close();
        return;
    }
    if (vk == 'S' && (mods & PANELP_M_CTRL)) {
        builder_save();
        return;
    }
    if (vk == VK_PRIOR || vk == VK_NEXT) {
        scroll_by((vk == VK_NEXT ? 1 : -1) * (A.h - top_h() - S(30)));
        return;
    }
    if (A.br.focus >= 0) {
        ebox *e = A.br.focus == BF_COUNT ? &A.br.query : &A.br.fields[A.br.focus];
        if (vk == VK_RETURN) {
            builder_preview();
            return;
        }
        if (eb_key(e, vk, mods)) {
            if (A.br.open == 2)
                A.laid_w = 0;
            else
                builder_changed();
        }
        return;
    }
    if (vk == VK_RETURN || vk == VK_SPACE) {
        const hot *h = find_hot(A.kf);
        if (h && h->id.kind == H_B_ACTION)
            builder_action(h->id.rec, h->id.sub);
    } else if (vk == VK_UP || vk == VK_DOWN)
        scroll_by((vk == VK_DOWN ? 1 : -1) * S(40));
}
static void builder_typed(wchar_t ch, uint32_t mods)
{
    if (A.eat_char) {
        A.eat_char = 0;
        return;
    }
    if (A.br.saving || A.br.recording || A.br.focus < 0)
        return;
    ebox *e = A.br.focus == BF_COUNT ? &A.br.query : &A.br.fields[A.br.focus];
    if (eb_char(e, ch, mods)) {
        if (A.br.open == 2)
            A.laid_w = 0;
        else
            builder_changed();
    }
}
static void builder_context(const void *data, uint32_t size)
{
    if (size < 14 || (size - 12) % 2 || size > 12 + RELAY_CONTEXT_CHARS * 2)
        return;
    uint32_t head[3];
    memcpy(head, data, 12);
    if (head[0] != (uint32_t)A.br.context_ticket || head[1] > RELAY_CONTEXT_MAX)
        return;
    const wchar_t *s = (const wchar_t *)((const uint8_t *)data + 12);
    size_t n = (size - 12) / 2, at = 0;
    for (uint32_t i = 0; i <= head[1]; ++i) {
        size_t len = wcsnlen(s + at, n - at);
        if (len == n - at)
            return;
        at += len + 1;
    }
    if (at != n)
        return;
    wchar_t *copy = malloc(n * 2);
    if (!copy)
        return;
    memcpy(copy, s, n * 2);
    free(A.br.context);
    A.br.context = copy;
    A.br.dir = copy;
    at = wcslen(copy) + 1;
    A.br.count = head[1];
    A.br.context_flags = head[2];
    for (uint32_t i = 0; i < A.br.count; ++i) {
        A.br.paths[i] = copy + at;
        at += wcslen(copy + at) + 1;
    }
    A.br.preview[0] = 0;
    if (A.br.open == 2) {
        if (A.br.catalogue_busy)
            A.br.catalogue_dirty = 1;
        else {
            free(A.br.catalogue);
            A.br.catalogue = NULL;
            builder_catalogue_start();
        }
    }
    A.laid_w = 0;
}

static void act(const hot *h)
{
    const rec *r = &A.s.recs[h->id.rec];
    switch (h->id.kind) {
    case H_RELAY_EDIT: builder_edit(r);A.laid_w=0;break;
    case H_PILL: change_setting(L"functions", r->id, !wcscmp(r->state, L"off") ? L"on" : L"off", 0); break;
    case H_CHOICE: {
        wchar_t w[64];
        _snwprintf(w, 64, L"%.*ls", h->wlen, h->word);
        w[63] = 0;
        change_setting(r->id, r->opts[h->id.opt].name, w, 0);
        break;
    }
    case H_RESET: change_setting(r->id, r->opts[h->id.opt].name, NULL, 0); break;
    case H_FIELD: field_open(r, &r->opts[h->id.opt]); break;
    case H_KEY_DEL: {
        wchar_t keys[KEYS_MAX][48], list[400];
        int n = split_keys(r->keys, keys, KEYS_MAX);
        join_keys(keys, n, h->id.sub, NULL, list, 400);
        key_message(-1, NULL);
        change_setting(L"keys", r->id, list, 0);
        break;
    }
    case H_KEY_ADD:
        if (A.rec_ri == h->id.rec) record_stop();
        else record_start(h->id.rec);
        break;
    case H_KEY_RESET: key_message(-1, NULL); change_setting(L"keys", r->id, NULL, 0); break;
    case H_SHOW_MENU: change_setting(L"menu", r->id, wcschr(r->flags, L'm') ? L"on" : L"off", 0); break;
    case H_SHOW_PAL: change_setting(L"palette", r->id, wcschr(r->flags, L'p') ? L"on" : L"off", 0); break;
    case H_CONF_ADD: keys_add(h->id.rec, A.conf.name, 0); break;
    case H_CONF_MOVE: keys_add(h->id.rec, A.conf.name, 1); break;
    case H_CONF_CANCEL: A.conf.ri = -1; layout(); break;
    }
}

static void saved(const char *utf8, uint32_t size)
{
    wchar_t t[400];
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)(size < 1000 ? size : 1000), t, 399);
    t[n > 0 ? n : 0] = 0;
    wchar_t *f[3];
    fields(t, f, 3);
    if ((unsigned)wcstoul(f[0], NULL, 10) != A.wait_req) return;
    A.completed = A.wait_req;
    /* 1 ok, 2 conflict, 3 invalid, 4 failed, 5 no answer. A stale reply
     * cannot acknowledge a later request or replace its completion status. */
    A.save_status = !wcscmp(f[1], L"ok") ? 1 : !wcscmp(f[1], L"conflict") ? 2
                      : !wcscmp(f[1], L"invalid") ? 3 : 4;
    int ok = !wcscmp(f[1], L"ok"), field = A.wait_field;
    A.wait_req = 0;
    KillTimer(A.hwnd, TIMER_WAIT);
    if(A.br.saving){
        A.br.saving=0;
        if(ok){A.br.draft.editing=1;A.br.base_rev=(int)A.s.rev;wcscpy(A.br.message,L"Saved. Changes apply immediately.");}
        else{_snwprintf(A.br.message,400,L"%ls",f[2]);if(!wcscmp(f[1],L"conflict"))A.br.base_rev=(int)A.s.rev;}
        A.laid_w=0;
    }else if (field && A.f_open) {
        if (ok) {
            field_close();
        } else {
            A.f_saving = 0;
            _snwprintf(A.f_msg, 200, L"%ls", f[2]);
            A.f_msg[199] = 0;
            layout();
        }
    } else if (!ok) {
        notice(1, f[2]);
    }
    change c = A.queued;
    A.queued.used = 0;
    if (ok && c.used) {
        send_change(&c);
    } else if (c.used && c.field && A.f_open) { /* dropped with the one before it: the field says why */
        A.f_saving = 0;
        _snwprintf(A.f_msg, 200, L"%ls", f[2]);
        A.f_msg[199] = 0;
        layout();
    }
}

/* ---- input ---------------------------------------------------------------------------------- */

static void query_changed(void)
{
    A.scroll = 0;
    layout();
}

static void scroll_by(int dy)
{
    A.scroll += dy;
    int max = A.content_h - (A.h - top_h());
    if (A.scroll > max) A.scroll = max;
    if (A.scroll < 0) A.scroll = 0;
}

/* The focused control in view, with a little room around it. */
static void reveal(hid id)
{
    const hot *h = find_hot(id);
    if (!h) return;
    int view = A.h - top_h(), m = S(24);
    if (h->r.top - m < A.scroll) A.scroll = h->r.top - m;
    else if (h->r.bottom + m > A.scroll + view) A.scroll = h->r.bottom + m - view;
    scroll_by(0);
}

/* Tab and Shift+Tab: the search box, then every control in order. */
static void focus_step(int dir)
{
    int at = -1;
    for (int i = 0; i < A.nhot && at < 0; i++)
        if (hid_eq(A.hots[i].id, A.kf)) at = i;
    int next = A.kf.kind == H_SEARCH ? (dir > 0 ? 0 : A.nhot - 1) : at + dir;
    if (!A.nhot || next < 0 || next >= A.nhot) {
        A.kf = mk(H_SEARCH, -1, -1, 0);
        return;
    }
    A.kf = A.hots[next].id;
    reveal(A.kf);
}

static void key(UINT vk, uint32_t mods)
{
    int ctrl = (mods & PANELP_M_CTRL) != 0, shift = (mods & PANELP_M_SHIFT) != 0;
    int view = A.h - top_h();
    if (A.rec_ri >= 0) {
        recorded(vk, mods);
        return;
    }
    A.eat_char = 0; /* the recorded key made no character */
    if (A.f_open) {
        switch (vk) {
        case VK_ESCAPE: field_close(); return;
        case VK_RETURN: field_commit(); return;
        case VK_TAB:
            field_commit();
            if (!A.f_saving) focus_step(shift ? -1 : 1);
            return;
        case VK_UP: scroll_by(-S(40)); return;
        case VK_DOWN: scroll_by(S(40)); return;
        }
        if (!A.f_saving && eb_key(&A.fe, vk, mods)) A.f_msg[0] = 0, layout();
        return;
    }
    switch (vk) {
    case VK_TAB: focus_step(shift ? -1 : 1); return;
    case VK_UP: scroll_by(-S(40)); return;
    case VK_DOWN: scroll_by(S(40)); return;
    case VK_PRIOR: scroll_by(-(view - S(40))); return;
    case VK_NEXT: scroll_by(view - S(40)); return;
    }
    if (A.kf.kind != H_SEARCH) { /* a control has the keyboard */
        const hot *h = find_hot(A.kf);
        switch (vk) {
        case VK_ESCAPE:
            A.kf = mk(H_SEARCH, -1, -1, 0);
            if (A.conf.ri >= 0) A.conf.ri = -1, layout();
            return;
        case VK_SPACE:
        case VK_RETURN:
            if (h) act(h);
            return;
        case VK_LEFT: focus_step(-1); return;
        case VK_RIGHT: focus_step(1); return;
        case VK_HOME: if (ctrl) A.scroll = 0; return;
        case VK_END: if (ctrl) scroll_by(A.content_h); return;
        }
        return;
    }
    switch (vk) {
    case VK_ESCAPE:
        if (A.q.len) {
            eb_set(&A.q, L"");
            query_changed();
        } else {
            send_msg(PANELP_CLOSE, NULL, 0);
        }
        return;
    case VK_HOME: if (ctrl) { A.scroll = 0; return; } break;
    case VK_END: if (ctrl) { scroll_by(A.content_h); return; } break;
    }
    if (eb_key(&A.q, vk, mods)) query_changed();
}

static void typed(wchar_t c, uint32_t mods)
{
    if (A.rec_ri >= 0) return;
    if (A.eat_char) {
        A.eat_char = 0;
        return;
    }
    if (A.f_open) {
        if (!A.f_saving && eb_char(&A.fe, c, mods)) A.f_msg[0] = 0, layout();
        return;
    }
    if (A.kf.kind != H_SEARCH) {
        if (c <= L' ' || (mods & PANELP_M_CTRL)) return; /* Space and Enter used the control */
        A.kf = mk(H_SEARCH, -1, -1, 0);                  /* typing anywhere searches */
    }
    if (eb_char(&A.q, c, mods)) query_changed();
}

static int in_rect(RECT r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

static const hot *hit(int x, int y)
{
    if (y < top_h() || in_rect(A.r_toast, x, y)) return NULL;
    int cy = y - top_h() + A.scroll;
    for (int i = 0; i < A.nhot; i++)
        if (in_rect(A.hots[i].r, x, cy)) return &A.hots[i];
    return NULL;
}

static int caret_at(const ebox *e, int left, int x)
{
    int k = 0;
    while (k < e->len && left + text_w(A.f_text, e->t, k + 1) <= x) k++;
    return k;
}

static void builder_input(const panelp_input *in)
{
    int x = in->x, y = in->y, cy = y - top_h() + A.scroll;
    switch (in->msg) {
    case WM_LBUTTONDOWN: {
        const hot *h = hit(x, y);
        builder_record_stop();
        if (!h)
            break;
        A.kf = h->id;
        if (h->id.kind == H_B_FIELD) {
            A.br.focus = h->id.opt;
            ebox *e = A.br.focus == BF_COUNT ? &A.br.query : &A.br.fields[A.br.focus];
            int left = A.br.focus == BF_COUNT ? pad() + S(8) : A.br.field_text[A.br.focus].left;
            e->caret = caret_at(e, left, x);
            if (!(in->mods & PANELP_M_SHIFT))
                e->sel = e->caret;
        } else if (h->id.kind == H_B_ACTION) {
            A.br.focus = -1;
            builder_action(h->id.rec, h->id.sub);
        }
        break;
    }
    case WM_LBUTTONDBLCLK:
        if (A.br.focus >= 0) {
            ebox *e = A.br.focus == BF_COUNT ? &A.br.query : &A.br.fields[A.br.focus];
            e->sel = 0;
            e->caret = e->len;
        }
        break;
    case WM_MOUSEWHEEL:
        scroll_by(-(short)HIWORD(in->wparam) * S(60) / WHEEL_DELTA);
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        builder_key(in->wparam, in->mods);
        break;
    case WM_CHAR:
        builder_typed((wchar_t)in->wparam, in->mods);
        break;
    case WM_MOUSEMOVE: {
        const hot *h = hit(x, y);
        hid id = h ? h->id : mk(-1, -1, -1, 0);
        A.hover = id;
        uint32_t cursor = h && h->id.kind == H_B_FIELD ? 32513 : h ? 32649 : 32512;
        if (cursor != A.cursor)
            send_msg(PANELP_CURSOR, &cursor, 4), A.cursor = cursor;
        break;
    }
    default:
        return;
    }
    (void)cy;
    A.caret_on = 1;
    paint();
}
static void builder_observe(void)
{
    static const wchar_t *field_names[] = {L"name",   L"title", L"program", L"arguments",
                                           L"action", L"types", L"keys",    L"filter"};
    for (int i = 0; i <= BF_COUNT; ++i) {
        wchar_t key[80];
        _snwprintf(key, 80, L"Command.Field.%ls", field_names[i]);
        RemovePropW(A.hwnd, key);
    }
    for (unsigned i = 0; i < A.br.n_observed; ++i)
        RemovePropW(A.hwnd, A.br.observed[i]);
    A.br.n_observed = 0;
    for (int i = 0; i < A.nhot; ++i) {
        const hot *h = A.hots + i;
        wchar_t key[80];
        if (h->id.kind == H_RELAY_EDIT)
            _snwprintf(key, 80, L"Command.Edit.%ls", A.s.recs[h->id.rec].id);
        else if (h->id.kind == H_B_ACTION && h->id.rec == BA_PICK && A.br.catalogue) {
            wchar_t title[256];
            MultiByteToWideChar(CP_UTF8, 0, A.br.catalogue->entries[h->id.sub].title, -1, title, 256);
            _snwprintf(key, 80, L"Command.Pick.%.60ls", title);
        } else if (h->id.kind == H_B_FIELD)
            _snwprintf(key, 80, L"Command.Field.%ls", field_names[h->id.opt]);
        else if (h->id.kind == H_B_ACTION)
            _snwprintf(key, 80, L"Command.Action.%d.%d", h->id.rec, h->id.sub);
        else
            continue;
        int x = (h->r.left + h->r.right) / 2, y = (h->r.top + h->r.bottom) / 2 + top_h() - A.scroll;
        SetPropW(A.hwnd, key, (HANDLE)(uintptr_t)((uint32_t)(uint16_t)x | ((uint32_t)(uint16_t)y << 16)));
        if (A.br.n_observed < 256)
            wcscpy(A.br.observed[A.br.n_observed++], key);
    }
    wchar_t caption[1000];
    if (A.br.open)
        _snwprintf(caption, 1000, L"Custom commands | %ls | ID %ls | %ls | %ls",
                   A.br.open == 2       ? L"Import"
                   : A.br.draft.editing ? L"Edit"
                                        : L"New",
                   A.br.fields[BF_NAME].t, A.br.message, A.br.program_status);
    else
        wcscpy(caption, L"Settings");
    caption[999] = 0;
    SetWindowTextW(A.hwnd, caption);
}

static void input(const panelp_input *in)
{
    int x = in->x, y = in->y;
    if(in->msg==WM_LBUTTONDOWN&&in_rect(A.r_commands,x,y)){
        if(A.br.open)builder_close();else builder_open();A.laid_w=0;paint();return;
    }
    if(in->msg==WM_LBUTTONDOWN&&can_edit()&&in_rect(A.r_edit,x,y)){run_fn("settings.edit");return;}
    if(A.br.open){builder_input(in);return;}
    switch (in->msg) {
    case WM_MOUSEMOVE: {
        int edit = can_edit() && in_rect(A.r_edit, x, y);
        const hot *h = hit(x, y);
        hid id = h ? h->id : mk(-1, -1, -1, 0);
        int beam = in_rect(A.r_field, x, y) || (h && h->id.kind == H_FIELD);
        uint32_t cur = beam ? 32513 /* IDC_IBEAM */ : edit || h ? 32649 /* IDC_HAND */ : 32512;
        if (cur != A.cursor) send_msg(PANELP_CURSOR, &cur, 4), A.cursor = cur;
        if (edit != A.hover_edit || !hid_eq(id, A.hover)) {
            A.hover_edit = edit;
            A.hover = id;
            break;
        }
        return;
    }
    case WM_LBUTTONDOWN: {
        const hot *ht = hit(x, y);
        /* Committing a field or cancelling recording rebuilds A.hots. Keep
         * this click's logical action and region, not a mutable layout slot.
         * Its word points into the unchanged snapshot for this input call. */
        hot clicked = ht ? *ht : (hot){0};
        int have_hot = ht != NULL;
        int on_toast = in_rect(A.r_toast, x, y);
        int on_search = in_rect(A.r_field, x, y);
        int on_edit = can_edit() && in_rect(A.r_edit, x, y);
        int in_field = ht && ht->id.kind == H_FIELD && A.f_open &&
                       field_is(&A.s.recs[ht->id.rec], &A.s.recs[ht->id.rec].opts[ht->id.opt]);
        if (A.rec_ri >= 0 && !(ht && ht->id.kind == H_KEY_ADD && ht->id.rec == A.rec_ri)) record_stop(); /* clicking elsewhere cancels */
        if (A.f_open && !in_field) field_commit(); /* clicking elsewhere saves it */
        if (on_toast) {
            A.notice[0] = 0;
            KillTimer(A.hwnd, TIMER_NOTICE);
        } else if (in_field) {
            if (!A.f_saving) {
                A.fe.caret = caret_at(&A.fe, A.f_at.left, x);
                if (!(in->mods & PANELP_M_SHIFT)) A.fe.sel = A.fe.caret;
            }
        } else if (on_search) {
            if (A.f_open && !A.f_saving) field_close();
            if (!A.f_open) {
                A.kf = mk(H_SEARCH, -1, -1, 0);
                A.q.caret = caret_at(&A.q, A.r_field.left + S(8), x);
                if (!(in->mods & PANELP_M_SHIFT)) A.q.sel = A.q.caret;
            }
        } else if (on_edit) {
            run_fn("settings.edit");
        } else if (have_hot && !(clicked.id.kind == H_FIELD && A.f_open)) {
            A.kf = clicked.id;
            act(&clicked);
        }
        break;
    }
    case WM_LBUTTONDBLCLK:
        if (A.f_open && !A.f_saving && in_rect(A.f_at, x, y)) A.fe.sel = 0, A.fe.caret = A.fe.len;
        else if (in_rect(A.r_field, x, y) && !A.f_open) A.q.sel = 0, A.q.caret = A.q.len;
        break;
    case WM_MOUSEWHEEL: scroll_by(-(short)HIWORD(in->wparam) * S(60) / WHEEL_DELTA); break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: key(in->wparam, in->mods); break;
    case WM_CHAR: typed((wchar_t)in->wparam, in->mods); break;
    default: return;
    }
    A.caret_on = 1; /* the caret shows right after anything typed or clicked */
    paint();
}

/* ---- host messages ------------------------------------------------------------------------ */

static void on_host(msg *m)
{
    switch (m->head.type) {
    case PANELP_PING: send_msg(PANELP_PONG, m->data, 4); break;
    case PANELP_FRAME_ACK:
        A.waiting = 0;
        if (A.pending) present();
        break;
    case PANELP_INPUT:
        if (m->head.size >= sizeof(panelp_input)) {
            const panelp_input *in = (const panelp_input *)m->data;
            if (in->msg == WM_KEYDOWN || in->msg == WM_SYSKEYDOWN) ++A.input_keys;
            else if (in->msg == WM_CHAR) ++A.input_chars;
            else if (in->msg == WM_LBUTTONDOWN) ++A.input_clicks;
            input(in);
        }
        break;
    case PANELP_FOCUS:
        A.focused = m->head.size >= 4 && *(uint32_t *)m->data;
        if (A.focused) SetTimer(A.hwnd, TIMER_CARET, GetCaretBlinkTime(), NULL);
        else KillTimer(A.hwnd, TIMER_CARET);
        if (!A.focused) {
            builder_record_stop();
            field_commit(); /* leaving the panel saves what was typed */
            if (A.rec_ri >= 0) A.rec_ri = -1, layout(); /* the explorer stopped sending every key */
        }
        A.caret_on = A.focused;
        paint();
        break;
    case PANELP_RESIZE:
        if (m->head.size >= sizeof(panelp_size)) {
            panelp_size s;
            memcpy(&s, m->data, sizeof s);
            if (s.dpi && (int)s.dpi != A.dpi) {
                A.dpi = (int)s.dpi;
                make_fonts();
            }
            A.seq = s.seq;
            if (pixels_map((HANDLE)(uintptr_t)s.section, (int)s.w, (int)s.h)) paint();
        }
        break;
    case PANELP_RELAY_OPEN:if(!A.br.open)builder_open();else builder_context_request();layout();paint();break;
    case PANELP_RELAY_CONTEXT:builder_context(m->data,m->head.size);layout();paint();break;
    case PANELP_RELAY_BROWSED:
        if(m->head.size>=6&&!(m->head.size%2)){uint32_t request;memcpy(&request,m->data,4);
            const wchar_t *path=(wchar_t*)(m->data+4);size_t chars=(m->head.size-4)/2;
            if(request==(uint32_t)A.br.browse_ticket&&path[chars-1]==0&&path[0]){
                if(wcslen(path)<512){eb_set(&A.br.fields[BF_PROGRAM],path);builder_changed();}else wcscpy(A.br.message,L"Program path is too long for this form");
                layout();paint();}}
        break;
    case PANELP_SETTINGS:
        take_snapshot((const char *)m->data, m->head.size);
        layout();
        paint();
        if (!GetPropW(A.hwnd, L"Settings.Ready")) {
            SetPropW(A.hwnd, L"Settings.Ready", (HANDLE)(uintptr_t)1);
            panel_startup_trace("settings.snapshot.ready");
        }
        break;
    case PANELP_SAVED:
        saved((const char *)m->data, m->head.size);
        paint();
        break;
    case PANELP_QUIT: PostQuitMessage(0); break;
    }
}

static void paste_tick(void)
{
    ebox *current = A.br.open               ? (A.br.focus == BF_COUNT ? &A.br.query
                                               : A.br.focus >= 0      ? &A.br.fields[A.br.focus]
                                                                      : NULL)
                    : A.f_open              ? &A.fe
                    : A.kf.kind == H_SEARCH ? &A.q
                                            : NULL;
    ebox *target = A.paste.target;
    if (!target || A.paste.generation != A.editor_generation || target != current || target->caret != A.paste.caret || target->sel != A.paste.sel ||
        whash(target->t, 0) != A.paste.hash || GetTickCount64() >= A.paste.until) {
        int expired = GetTickCount64() >= A.paste.until;
        A.paste.target = NULL;
        KillTimer(A.hwnd, TIMER_PASTE);
        if (A.br.open && (!wcsncmp(A.br.message, L"Waiting for", 11) ||
                          !wcsncmp(A.br.message, L"Wait for the pending", 20))) {
            wcscpy(A.br.message, expired ? L"Clipboard still busy; paste was not inserted."
                                         : L"Paste cancelled because the field changed.");
            A.laid_w = 0;
            layout();
            paint();
        }
        return;
    }
    int rc = paste_read(target);
    if (rc < 0)
        return;
    A.paste.target = NULL;
    KillTimer(A.hwnd, TIMER_PASTE);
    if (!rc && A.br.open && !wcsncmp(A.br.message, L"Waiting for", 11))
        wcscpy(A.br.message, L"The clipboard contains no plain text to paste.");
    if (rc > 0) {
        if (A.br.open)
            builder_changed();
        else if (A.f_open)
            A.f_msg[0] = 0;
        else
            query_changed();
    }
    A.laid_w = 0;
    layout();
    paint();
}

static LRESULT CALLBACK hidden_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_BUILDER_CATALOGUE) {
        A.br.catalogue_busy = 0;
        free(A.br.catalogue);
        A.br.catalogue = (relay_catalog *)l;
        if (A.br.catalogue_dirty && A.br.open == 2) {
            free(A.br.catalogue);
            A.br.catalogue = NULL;
            A.br.catalogue_dirty = 0;
            builder_catalogue_start();
        }
        A.laid_w = 0;
        layout();
        paint();
        return 0;
    }
    if (m == WM_BUILDER_RESOLVED) {
        builder_resolve *r = (builder_resolve *)l;
        A.br.resolve_busy = 0;
        if (r->ticket == A.br.ticket && A.br.open)
            wcscpy(A.br.program_status, r->status);
        free(r);
        A.laid_w = 0;
        layout();
        paint();
        return 0;
    }
    if (m == WM_TIMER && w == TIMER_PASTE) {
        paste_tick();
        return 0;
    }
    if (m == WM_TIMER) {
        if (w == TIMER_CARET)
            A.caret_on = !A.caret_on;
        else
            KillTimer(h, w);
        if (w == TIMER_NOTICE)
            A.notice[0] = 0;
        if (w == TIMER_WAIT && A.wait_req) { /* no answer: let the next change go */
            A.completed = A.wait_req;
            A.save_status = 5;
            A.wait_req = 0;
            A.queued.used = 0;
            if (A.br.saving) {
                A.br.saving = 0;
                wcscpy(
                    A.br.message,
                    L"No answer from the explorer: the draft is kept; check saved settings before retrying");
            }
            if (A.f_saving)
                A.f_saving = 0, _snwprintf(A.f_msg, 200, L"No answer from the explorer: not saved");
            layout();
        }
        paint(); /* TIMER_MARK: the change marks fade */
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    panel_startup_trace("settings.entry");
    (void)prev, (void)cmd, (void)show;
    A.in = GetStdHandle(STD_INPUT_HANDLE);
    A.out = GetStdHandle(STD_OUTPUT_HANDLE);
    A.q.max = 127;
    A.fe.max = 255;
    A.hover = mk(-1, -1, -1, 0);
    A.kf = mk(H_SEARCH, -1, -1, 0);
    A.rec_ri = A.msg_ri = A.conf.ri = -1;
    panelp_head h;
    panelp_hello hi;
    if (!read_all(&h, sizeof h) || h.type != PANELP_HELLO || h.size != sizeof hi || !read_all(&hi, sizeof hi) ||
        hi.magic != PANELP_MAGIC || hi.version != PANELP_VERSION)
        return 1;
    panel_startup_trace("settings.hello.read");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    A.dpi = hi.dpi ? (int)hi.dpi : 96;
    A.seq = hi.seq;
    make_fonts();
    panel_startup_trace("settings.fonts.ready");

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = hidden_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"FpSettingsHidden";
    RegisterClassW(&wc);
    A.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!A.hwnd) return 1;
    panel_startup_trace("settings.window.created");
    pixels_map((HANDLE)(uintptr_t)hi.section, (int)hi.w, (int)hi.h);
    HANDLE t = CreateThread(NULL, 64 * 1024, reader, A.hwnd, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (t) CloseHandle(t);
    panelp_ready r = { GetCurrentProcessId(), 0, L"Settings" };
    send_msg(PANELP_READY, &r, sizeof r);
    panel_startup_trace("settings.ready.sent");
    paint();
    panel_startup_trace("settings.first_frame.sent");
    observe_state();

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_HOST) {
            on_host((msg *)m.lParam);
            free((void *)m.lParam);
            observe_state();
            continue;
        }
        if (m.message == WM_HOST_GONE) break;
        DispatchMessageW(&m);
        observe_state();
    }
    return 0;
}
