/* Session file: see session.h. */
#include "session.h"
#include "pathwire.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const sort_names[] = { "name", "type", "size", "modified", "created" };
#define NSORT (int)(sizeof sort_names / sizeof sort_names[0])

void session_defaults(session *s)
{
    memset(s, 0, sizeof *s);
    s->tree = 1;
    s->dual = 1;
    s->tree_w = 230;
    s->ratio = 0.5;
}

void session_free(session *s)
{
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < s->ntabs[p]; i++) free(s->tabs[p][i].path);
    for (int i = 0; i < s->npins; i++) free(s->pins[i]);
    for (int i = 0; i < s->nrecent; i++) free(s->recent[i]);
    session_defaults(s);
}

static wchar_t *widen(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 1) return NULL;
    wchar_t *w = malloc((size_t)n * sizeof(wchar_t));
    if (w && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n)) {
        free(w);
        return NULL;
    }
    return w;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
    return s;
}

static wchar_t *decode_path(const char *s, int exact)
{
    if (!exact) return widen(s); /* pre-v1 UTF-8 sessions */
    size_t n = strlen(s);
    if (n < PATHWIRE_PREFIX_LEN + 4 || (n - PATHWIRE_PREFIX_LEN) / 4 >= SIZE_MAX / sizeof(wchar_t)) return NULL;
    wchar_t *w = malloc(((n - PATHWIRE_PREFIX_LEN) / 4 + 1) * sizeof(wchar_t));
    if (w && !pathwire_decode(s, n, w, (n - PATHWIRE_PREFIX_LEN) / 4 + 1)) { free(w); w = NULL; }
    return w;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void session_parse(const char *text, size_t len, session *s)
{
    session_defaults(s);
    if (!text || !len) return;
    char *buf = malloc(len + 1);
    if (!buf) return;
    memcpy(buf, text, len);
    buf[len] = 0;
    char *p = buf;
    if ((uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) p += 3;

    int pane = -1; /* 0, 1 in [pane1] / [pane2]; 2 in [pins]; 3 in [recent]; -1 elsewhere */
    int window = 0;
    while (p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        char *l = p;
        size_t ll = strlen(l);
        if (ll && l[ll - 1] == '\r') l[--ll] = 0;
        while (*l == ' ' || *l == '\t') l++;
        p = nl ? nl + 1 : NULL;
        if (!*l || *l == ';' || *l == '#') continue;
        if (*l == '[') {
            window = !strcmp(l, "[window]");
            pane = !strcmp(l, "[pane1]") ? 0 : !strcmp(l, "[pane2]") ? 1 : !strcmp(l, "[pins]") ? 2 : !strcmp(l, "[recent]") ? 3 : -1;
            continue;
        }
        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(l), *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        if (window) {
            if (!strcmp(k, "rect")) {
                long x, y, r, b;
                if (sscanf(v, "%ld %ld %ld %ld", &x, &y, &r, &b) == 4 && r - x >= 200 && b - y >= 150) {
                    SetRect(&s->rect, x, y, r, b);
                    s->has_rect = 1;
                }
            } else if (!strcmp(k, "maximized")) s->maximized = atoi(v) != 0;
            else if (!strcmp(k, "tree")) s->tree = atoi(v) != 0;
            else if (!strcmp(k, "tree_width")) s->tree_w = clampi(atoi(v), 120, 1200);
            else if (!strcmp(k, "dual")) s->dual = atoi(v) != 0;
            else if (!strcmp(k, "hidden")) s->hidden = atoi(v) != 0;
            else if (!strcmp(k, "active")) s->active = atoi(v) == 1;
            else if (!strcmp(k, "ratio")) {
                double r = atof(v);
                s->ratio = r < 0.15 ? 0.15 : r > 0.85 ? 0.85 : r;
            }
        } else if (pane == 0 || pane == 1) {
            if (!strcmp(k, "current")) {
                s->cur[pane] = atoi(v);
            } else if ((!strcmp(k, "tab") || !strcmp(k, "tab16")) && s->ntabs[pane] < SES_TABS_MAX) {
                /* tab = <sort> <0|1> <path> */
                char key[16];
                int desc, at = 0;
                if (sscanf(v, "%15s %d %n", key, &desc, &at) < 2 || !at) continue;
                ses_tab *t = &s->tabs[pane][s->ntabs[pane]];
                t->path = decode_path(v + at, !strcmp(k, "tab16"));
                if (!t->path) continue;
                t->sort = 0;
                for (int i = 0; i < NSORT; i++)
                    if (!strcmp(key, sort_names[i])) t->sort = (uint8_t)i;
                t->desc = desc != 0;
                s->ntabs[pane]++;
            }
        } else if (pane == 2 && (!strcmp(k, "pin") || !strcmp(k, "pin16")) && s->npins < SES_PINS_MAX) {
            wchar_t *w = decode_path(v, !strcmp(k, "pin16"));
            if (w) s->pins[s->npins++] = w;
        } else if (pane == 3 && (!strcmp(k, "folder") || !strcmp(k, "folder16")) && s->nrecent < SES_RECENT_MAX) {
            wchar_t *w = decode_path(v, !strcmp(k, "folder16"));
            if (w) s->recent[s->nrecent++] = w;
        }
    }
    for (int i = 0; i < 2; i++)
        if (s->cur[i] < 0 || s->cur[i] >= s->ntabs[i]) s->cur[i] = 0;
    free(buf);
}

typedef struct out {
    char  *p;
    size_t cap, len;
    int    full;
} out;

static void put(out *o, const char *fmt, ...)
{
    if (o->full) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->p + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->cap - o->len) { o->full = 1; return; }
    o->len += (size_t)n;
}

static void put_path(out *o, const wchar_t *w)
{
    if (o->full) return;
    size_t n = pathwire_encode(w, o->p + o->len, o->cap - o->len);
    if (!n) { o->full = 1; return; }
    o->len += n;
}

size_t session_format(const session *s, char *buf, size_t cap)
{
    out o = { buf, cap, 0, 0 };
    if (!cap) return 0;
    buf[0] = 0;
    put(&o, "; Folderwing session, written on exit\n[window]\n");
    if (s->has_rect)
        put(&o, "rect = %ld %ld %ld %ld\n", s->rect.left, s->rect.top, s->rect.right, s->rect.bottom);
    put(&o, "maximized = %d\ntree = %d\ntree_width = %d\ndual = %d\nratio = %.3f\nhidden = %d\nactive = %d\n",
        s->maximized, s->tree, s->tree_w, s->dual, s->ratio, s->hidden, s->active);
    for (int p = 0; p < 2; p++) {
        put(&o, "[pane%d]\ncurrent = %d\n", p + 1, s->cur[p]);
        for (int i = 0; i < s->ntabs[p]; i++) {
            const ses_tab *t = &s->tabs[p][i];
            if (!t->path) continue;
            put(&o, "tab16 = %s %d ", sort_names[t->sort < NSORT ? t->sort : 0], t->desc);
            put_path(&o, t->path);
            put(&o, "\n");
        }
    }
    if (s->npins) put(&o, "[pins]\n");
    for (int i = 0; i < s->npins; i++) {
        put(&o, "pin16 = ");
        put_path(&o, s->pins[i]);
        put(&o, "\n");
    }
    if (s->nrecent) put(&o, "[recent]\n");
    for (int i = 0; i < s->nrecent; i++) {
        put(&o, "folder16 = ");
        put_path(&o, s->recent[i]);
        put(&o, "\n");
    }
    if (o.full) {
        buf[0] = 0;
        return 0;
    }
    return o.len;
}
