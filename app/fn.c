/* Function registry: see fn.h. */
#include "fn.h"
#include "brand.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fpe.h"

enum { ST_IDLE, ST_RUNNING, ST_FAILED, ST_STARTING, ST_STOPPING };
static const char *const state_names[] = { "idle", "running", "failed", "starting", "stopping" };
static const char *const life_names[] = { "hot", "warm", "cold" };

#define NEEDS_MAX 6

typedef struct slot {
    const fn_def *d;
    uint8_t  on, menu_on, pal_on, state, nkeys, nneeds, busy, stop_called;
    fn_binding   keys[FN_KEYS_MAX];
    int16_t  needs[NEEDS_MAX]; /* slot indexes, -1 for a name nobody registered */
    uint32_t runs, starts, holds, violations, depth;
    uint32_t period;          /* fn_timer */
    uint64_t due, last_used;
    int64_t  t0;
    fpe_zone zone, zstart;    /* "fn.<id>" and "fn.<id>/start" */
    char     zname[48], zsname[56];
    char     cerr[200];       /* fn_config_error, cleared before each configure */
    /* A family member (fn_family) owns its definition. Slots never move
     * (the profiler links their zones), so a removed member is only marked
     * gone; its slot serves the next new section. */
    uint8_t  fam, gone, seen;
    struct member *own;
} slot;

/* What a family member owns, allocated for members only. */
typedef struct member {
    fn_def  def;
    char    id[48];
    wchar_t title[80];
} member;

#define FAMILY_MAX 4

typedef struct kv {
    const char *sec, *key, *val; /* val NULL: an option value that doesn't fit, so the default holds */
} kv;

static struct {
    fn_host  host;
    slot     s[FN_MAX];
    uint32_t n;
    uint8_t  order[FN_MAX]; /* start order, so shutdown runs backwards */
    uint32_t norder;
    int      resolved;
    char    *text; /* settings, split in place */
    kv      *kv;
    uint32_t nkv;
    char     errors[1024];
    size_t   elen;
    char     all_errors[2048]; /* errors + each function's cerr, built by fn_settings_errors */
    uint32_t revision;
    uint8_t  hot_done; /* fn_start_hot ran: 1 for the first-frame part, 2 for the deferred one */
    const fn_def *family[FAMILY_MAX];
    uint32_t nfamily;
    const fn_def *self; /* whose callback runs (fn_self) */
} R;

/* Calls a callback of d with fn_self() telling it whose it is. */
#define AS_SELF(d, call)                \
    do {                                \
        const fn_def *self_was_ = R.self; \
        R.self = (d);                   \
        call;                           \
        R.self = self_was_;             \
    } while (0)

/* ---- helpers --------------------------------------------------------------- */

static uint64_t now_ms(void) { return R.host.now_ms ? R.host.now_ms() : GetTickCount64(); }

static void error(const char *fmt, ...)
{
    if (R.elen + 2 >= sizeof R.errors) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(R.errors + R.elen, sizeof R.errors - R.elen - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    R.elen += (size_t)n < sizeof R.errors - R.elen - 1 ? (size_t)n : sizeof R.errors - R.elen - 2;
    R.errors[R.elen++] = '\n';
    R.errors[R.elen] = 0;
}

static void tell(int err, const wchar_t *fmt, ...)
{
    if (!R.host.status) return;
    wchar_t msg[300];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(msg, 300, fmt, ap);
    va_end(ap);
    msg[299] = 0;
    R.host.status(err, msg);
}

static int index_of(const fn_def *d)
{
    for (uint32_t i = 0; i < R.n; i++)
        if (R.s[i].d == d) return (int)i;
    return -1;
}

static slot *slot_of(const fn_def *d)
{
    int i = d ? index_of(d) : -1;
    return i < 0 ? NULL : &R.s[i];
}

static int find_index(const char *id, size_t len)
{
    for (uint32_t i = 0; i < R.n; i++)
        if (!R.s[i].gone && strlen(R.s[i].d->id) == len && !strncmp(R.s[i].d->id, id, len)) return (int)i;
    return -1;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
    return s;
}

/* Is `word` one of the space-separated words in `list`? */
static int has_word(const char *list, const char *word)
{
    size_t wl = strlen(word);
    for (const char *p = list; p && *p;) {
        while (*p == ' ') p++;
        const char *e = p;
        while (*e && *e != ' ') e++;
        if ((size_t)(e - p) == wl && !strncmp(p, word, wl)) return 1;
        p = e;
    }
    return 0;
}

/* ---- keys ------------------------------------------------------------------ */

int fn_key_text(const fn_def *d, wchar_t *buf, size_t cap)
{
    slot *s = slot_of(d);
    if (cap) buf[0] = 0;
    if (!s || !s->nkeys) return 0;
    fn_format_key(s->keys[0], buf, cap);
    return 1;
}

/* "F7, Ctrl+Shift+N" or "none". */
static int parse_bindings(slot *s, const char *list, const char *where)
{
    s->nkeys = 0;
    if (!list) return 0;
    char buf[160];
    snprintf(buf, sizeof buf, "%s", list);
    char *t = trim(buf);
    if (!*t || !_stricmp(t, "none")) return 0;
    int bad = 0;
    for (char *p = t; p;) {
        char *comma = strchr(p, ',');
        if (comma) *comma = 0;
        char *one = trim(p);
        fn_binding k;
        if (!*one) {
        } else if (fn_parse_key(one, &k)) {
            error("%s: unknown key \"%s\" for %s", where, one, s->d->id);
            bad = 1;
        } else if (s->nkeys < FN_KEYS_MAX) {
            s->keys[s->nkeys++] = k;
        } else {
            error("%s: %s has more than %d keys", where, s->d->id, FN_KEYS_MAX);
        }
        p = comma ? comma + 1 : NULL;
    }
    return bad ? -1 : 0;
}

/* ---- registration + settings ----------------------------------------------------- */

void fn_init(const fn_host *host)
{
    if (host) R.host = *host;
}

static void slot_names(slot *s)
{
    snprintf(s->zname, sizeof s->zname, "fn.%s", s->d->id);
    snprintf(s->zsname, sizeof s->zsname, "fn.%s/start", s->d->id);
    /* a zone already known to the profiler keeps its list link */
    if (!s->zone.state) s->zone.name = s->zname;
    if (!s->zstart.state) s->zstart.name = s->zsname;
}

static void slot_defaults(slot *s)
{
    s->on = !(s->d->flags & FN_OPTIONAL);
    s->menu_on = 1;
    s->pal_on = 1;
    parse_bindings(s, s->d->keys, "default");
}

int fn_register(const fn_def *defs, uint32_t n)
{
    int rc = 0;
    for (uint32_t i = 0; i < n; i++) {
        const fn_def *d = &defs[i];
        if (!d->id || find_index(d->id, strlen(d->id)) >= 0) {
            error("function \"%s\" registered twice", d->id ? d->id : "(null)");
            rc = -1;
            continue;
        }
        if (R.n >= FN_MAX) {
            error("more than %d functions", FN_MAX);
            return -1;
        }
        slot *s = &R.s[R.n++];
        R.revision++;
        s->d = d;
        slot_names(s);
        slot_defaults(s);
    }
    R.resolved = 0;
    return rc;
}

int fn_family(const fn_def *tmpl)
{
    /* Dynamic sections can retire their definition during reload. Async
     * services need a stable definition through their final completion. */
    if (!tmpl || !tmpl->id || tmpl->stop_async || R.nfamily >= FAMILY_MAX) return -1;
    R.revision++;
    R.family[R.nfamily++] = tmpl;
    return 0;
}

const fn_def *fn_self(void) { return R.self; }

static const fn_def *family_of(const char *sec)
{
    for (uint32_t f = 0; f < R.nfamily; f++) {
        size_t n = strlen(R.family[f]->id);
        if (!strncmp(sec, R.family[f]->id, n)) return R.family[f];
    }
    return NULL;
}

static int family_name_ok(const char *name)
{
    size_t n = strlen(name);
    if (!n || n > 32) return 0;
    for (const char *p = name; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
            return 0;
    return 1;
}

/* The member for section `id` of family t: the slot it had, else a gone
 * one, else a new one. NULL when the registry is full. */
static slot *family_slot(const fn_def *t, const char *id)
{
    slot *s = NULL;
    for (uint32_t i = 0; i < R.n && !s; i++)
        if (R.s[i].fam && !strcmp(R.s[i].own->id, id)) s = &R.s[i];
    for (uint32_t i = 0; i < R.n && !s; i++)
        if (R.s[i].fam && R.s[i].gone) s = &R.s[i];
    if (!s) {
        if (R.n >= FN_MAX) return NULL;
        member *m = calloc(1, sizeof *m);
        if (!m) return NULL;
        s = &R.s[R.n++];
        s->own = m;
    }
    if (!s->fam || strcmp(s->own->id, id)) { /* someone else's counters go */
        fpe_zone z = s->zone, zs = s->zstart;
        member *m = s->own;
        memset(s, 0, sizeof *s);
        s->own = m;
        z.calls = z.ticks = z.max_ticks = 0;
        zs.calls = zs.ticks = zs.max_ticks = 0;
        s->zone = z;
        s->zstart = zs;
        s->fam = 1;
        snprintf(s->own->id, sizeof s->own->id, "%s", id);
    }
    s->own->def = *t;
    s->own->def.id = s->own->id;
    s->own->def.title = s->own->title;
    s->d = &s->own->def;
    s->gone = 0;
    s->seen = 1;
    slot_names(s);
    slot_defaults(s);
    return s;
}

/* A member's title: its "title" option, else the name after the prefix. */
static void family_title(slot *s)
{
    const char *t = fn_option(s->d, "title");
    if (!t[0]) t = strchr(s->own->id, '.') ? strchr(s->own->id, '.') + 1 : s->own->id;
    if (!MultiByteToWideChar(CP_UTF8, 0, t, -1, s->own->title, 80)) s->own->title[0] = 0;
    s->own->title[79] = 0;
}

static void resolve(void)
{
    if (R.resolved) return;
    R.resolved = 1;
    for (uint32_t i = 0; i < R.n; i++) {
        slot *s = &R.s[i];
        s->nneeds = 0;
        if (s->gone) continue;
        for (const char *p = s->d->needs; p && *p;) {
            while (*p == ' ') p++;
            const char *e = p;
            while (*e && *e != ' ') e++;
            if (e > p && s->nneeds < NEEDS_MAX) {
                int k = find_index(p, (size_t)(e - p));
                if (k < 0) error("%s needs \"%.*s\", which does not exist", s->d->id, (int)(e - p), p);
                s->needs[s->nneeds++] = (int16_t)k;
            }
            p = e;
        }
    }
}

static int parse_bool(const char *v, uint8_t *out)
{
    static const char *yes[] = { "on", "true", "yes", "1" }, *no[] = { "off", "false", "no", "0" };
    for (int i = 0; i < 4; i++) {
        if (!_stricmp(v, yes[i])) { *out = 1; return 0; }
        if (!_stricmp(v, no[i])) { *out = 0; return 0; }
    }
    return -1;
}

static const fn_opt *find_opt(const fn_def *d, const char *name)
{
    for (const fn_opt *o = d->opts; o && o->name; o++)
        if (!_stricmp(o->name, name)) return o;
    return NULL;
}

/* Does v fit o->values? Writes what would ("use ...") to why when it doesn't. */
static int value_fits(const fn_opt *o, const char *v, char *why, size_t cap)
{
    const char *spec = o->values;
    if (!spec) return 1;
    if (spec[0] == '+') { /* some of the words */
        char list[160];
        snprintf(list, sizeof list, "%s", spec + 1);
        for (const char *p = v; *p;) {
            while (*p == ' ' || *p == ',') p++;
            const char *e = p;
            while (*e && *e != ' ' && *e != ',') e++;
            if (e == p) break;
            char w[40];
            snprintf(w, sizeof w, "%.*s", (int)(e - p < 39 ? e - p : 39), p);
            int ok = 0;
            for (const char *q = list; *q;) {
                while (*q == ' ') q++;
                const char *qe = q;
                while (*qe && *qe != ' ') qe++;
                if (qe > q && strlen(w) == (size_t)(qe - q) && !_strnicmp(q, w, strlen(w))) ok = 1;
                q = qe;
            }
            if (!ok) {
                snprintf(why, cap, "\"%s\" is not one of: %s", w, list);
                return 0;
            }
            p = e;
        }
        return 1;
    }
    if (spec[0] == '#') {
        long lo = 0, hi = 0;
        sscanf(spec + 1, "%ld..%ld", &lo, &hi);
        char *end;
        long n = strtol(v, &end, 10);
        if (*v && !*end && n >= lo && n <= hi) return 1;
        snprintf(why, cap, "use a whole number from %ld to %ld", lo, hi);
        return 0;
    }
    for (const char *p = spec; *p;) {
        while (*p == ' ') p++;
        const char *e = p;
        while (*e && *e != ' ') e++;
        if (e > p && strlen(v) == (size_t)(e - p) && !_strnicmp(p, v, (size_t)(e - p))) return 1;
        p = e;
    }
    /* "use a, b or c" */
    size_t n = (size_t)snprintf(why, cap, "use ");
    int words = 0, total = 0;
    for (const char *p = spec; *p; p++)
        if (*p != ' ' && (p == spec || p[-1] == ' ')) total++;
    for (const char *p = spec; *p && n < cap;) {
        while (*p == ' ') p++;
        const char *e = p;
        while (*e && *e != ' ') e++;
        if (e == p) break;
        words++;
        n += (size_t)snprintf(why + n, cap - n, "%s%.*s", words == 1 ? "" : words == total ? " or " : ", ", (int)(e - p), p);
        p = e;
    }
    return 0;
}

static void stop_at(int i);

/* Members whose section is gone go too (stopped first), before new ones take their slots. */
static void families_prune(void)
{
    for (uint32_t i = 0; i < R.n; i++) {
        slot *s = &R.s[i];
        if (!s->fam || s->gone || s->seen) continue;
        if (s->state == ST_RUNNING) stop_at((int)i);
        s->gone = 1;
        s->state = ST_IDLE;
        s->cerr[0] = 0;
        s->nkeys = 0;
    }
    R.resolved = 0;
}

static void families_title(void)
{
    for (uint32_t i = 0; i < R.n; i++)
        if (R.s[i].fam && !R.s[i].gone) family_title(&R.s[i]);
}

int fn_settings(const char *text, size_t len)
{
    /* everything that can fail happens before the live settings are touched */
    char *buf = NULL;
    kv *kvs = NULL;
    if (text && len) {
        buf = malloc(len + 1);
        uint32_t lines = 1;
        for (size_t i = 0; i < len; i++) lines += text[i] == '\n';
        kvs = calloc(lines, sizeof *kvs);
        if (!buf || !kvs) {
            free(buf);
            free(kvs);
            return -2;
        }
        memcpy(buf, text, len);
        buf[len] = 0;
    }
    /* Visible menus and palette rows become stale before a family slot or
     * option can be replaced, including while a stop callback pumps UI. */
    R.revision++;
    free(R.text);
    free(R.kv);
    R.text = buf;
    R.kv = kvs;
    R.nkv = 0;
    R.elen = 0;
    R.errors[0] = 0;
    for (uint32_t i = 0; i < R.n; i++) {
        slot_defaults(&R.s[i]);
        R.s[i].seen = 0;
    }
    R.resolved = 0;
    if (!buf) {
        families_prune();
        return 0;
    }
    char *p = R.text;
    if ((uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) p += 3;

    const char *sec = "", *fresh[FN_MAX];
    uint32_t nfresh = 0;
    int line_no = 0;
    while (p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        line_no++;
        char *l = trim(p);
        p = nl ? nl + 1 : NULL;
        if (!*l || *l == ';' || *l == '#') continue;
        if (*l == '[') {
            char *e = strchr(l, ']');
            if (!e) { error("line %d: missing ]", line_no); continue; }
            *e = 0;
            sec = trim(l + 1);
            if (!strcmp(sec, "functions") || !strcmp(sec, "keys") || !strcmp(sec, "menu") || !strcmp(sec, "palette"))
                continue;
            int at = find_index(sec, strlen(sec));
            const fn_def *fam = family_of(sec);
            if (at >= 0 && R.s[at].fam) {
                R.s[at].seen = 1; /* repeated: its options add up */
            } else if (at < 0 && fam && !family_name_ok(sec + strlen(fam->id))) {
                error("line %d: [%s]: a name after %s is letters, digits, - and _ (at most 32)", line_no, sec, fam->id);
            } else if (at < 0 && fam) {
                int again = 0;
                for (uint32_t k = 0; k < nfresh; k++) again |= !strcmp(fresh[k], sec);
                if (!again && nfresh < FN_MAX) fresh[nfresh++] = sec;
            } else if (at < 0) {
                error("line %d: unknown section [%s]", line_no, sec);
            }
            continue;
        }
        char *eq = strchr(l, '=');
        if (!eq) { error("line %d: expected name = value", line_no); continue; }
        *eq = 0;
        R.kv[R.nkv].sec = sec;
        R.kv[R.nkv].key = trim(l);
        R.kv[R.nkv].val = trim(eq + 1);
        R.nkv++;
    }
    families_prune();
    for (uint32_t k = 0; k < nfresh; k++)
        if (!family_slot(family_of(fresh[k]), fresh[k])) error("[%s]: more than %d functions", fresh[k], FN_MAX);

    int rc = 0;
    for (uint32_t i = 0; i < R.nkv; i++) {
        const kv *e = &R.kv[i];
        int fi = find_index(e->key, strlen(e->key));
        if (!strcmp(e->sec, "functions") || !strcmp(e->sec, "menu") || !strcmp(e->sec, "palette")) {
            if (fi < 0) { error("[%s] unknown function %s", e->sec, e->key); rc = -1; continue; }
            uint8_t *flag = e->sec[0] == 'm' ? &R.s[fi].menu_on : e->sec[0] == 'p' ? &R.s[fi].pal_on : &R.s[fi].on;
            if (parse_bool(e->val, flag)) {
                error("[%s] %s = %s: use on or off", e->sec, e->key, e->val);
                rc = -1;
            }
        } else if (!strcmp(e->sec, "keys")) {
            if (fi < 0) { error("[keys] unknown function %s", e->key); rc = -1; continue; }
            if (parse_bindings(&R.s[fi], e->val, "[keys]")) rc = -1;
        } else {
            int si = find_index(e->sec, strlen(e->sec));
            if (si < 0) continue; /* reported with the section */
            const fn_opt *o = find_opt(R.s[si].d, e->key);
            char why[160];
            if (!o) {
                error("[%s] has no option %s", e->sec, e->key);
                rc = -1;
            } else if (!value_fits(o, e->val, why, sizeof why)) {
                error("[%s] %s = %s: %s", e->sec, e->key, e->val, why);
                R.kv[i].val = NULL;
                rc = -1;
            }
        }
    }
    families_title();
    R.resolved = 0;
    return rc;
}

int fn_settings_file(const wchar_t *path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return fn_settings(NULL, 0);
    LARGE_INTEGER size;
    char *buf = NULL;
    DWORD got = 0;
    if (GetFileSizeEx(f, &size) && size.QuadPart < (1 << 20) && (buf = malloc((size_t)size.QuadPart + 1)))
        ReadFile(f, buf, (DWORD)size.QuadPart, &got, NULL);
    CloseHandle(f);
    int rc = fn_settings(buf, got);
    free(buf);
    return rc;
}

/* ---- editing the settings text ----------------------------------------------- */

enum { LN_BLANK, LN_SECTION, LN_COMMENT, LN_KV };

typedef struct ini_line {
    size_t a, b, next; /* [a, b) without the line end; next: the following line */
    int    kind;
    size_t name, name_len; /* section name, or the key (of a comment too, when it looks like "key = ...") */
    size_t eq;             /* LN_KV: the '=' */
} ini_line;

static size_t skip_ws(const char *t, size_t i, size_t end)
{
    while (i < end && (t[i] == ' ' || t[i] == '\t')) i++;
    return i;
}

static void ini_classify(const char *t, ini_line *l)
{
    size_t i = skip_ws(t, l->a, l->b), e = l->b;
    while (e > i && (t[e - 1] == ' ' || t[e - 1] == '\t')) e--;
    l->name = l->name_len = 0;
    if (i == e) {
        l->kind = LN_BLANK;
    } else if (t[i] == '[') {
        l->kind = LN_SECTION;
        size_t s = skip_ws(t, i + 1, e), close = s;
        while (close < e && t[close] != ']') close++;
        size_t n = close;
        while (n > s && (t[n - 1] == ' ' || t[n - 1] == '\t')) n--;
        l->name = s, l->name_len = n - s;
    } else {
        int comment = t[i] == ';' || t[i] == '#';
        l->kind = comment ? LN_COMMENT : LN_KV;
        size_t k = comment ? skip_ws(t, i + 1, e) : i, eq = k;
        while (eq < e && t[eq] != '=') eq++;
        if (eq == e) {
            if (!comment) l->kind = LN_BLANK; /* not name = value: the parser reports it; leave it alone */
            return;
        }
        size_t n = eq;
        while (n > k && (t[n - 1] == ' ' || t[n - 1] == '\t')) n--;
        for (size_t c = k; c < n; c++)
            if (t[c] == ' ' || t[c] == '\t') return; /* "; some words = x" is prose */
        l->name = k, l->name_len = n - k, l->eq = eq;
    }
}

static int name_is(const char *t, const ini_line *l, const char *s, int icase)
{
    size_t n = strlen(s);
    return l->name_len == n && (icase ? !_strnicmp(t + l->name, s, n) : !strncmp(t + l->name, s, n));
}

/* "f7 ,ctrl+shift+n" -> "F7, Ctrl+Shift+N"; "" or none -> "none". Repeats are dropped. */
static int bindings_text(const char *in, char *out, size_t cap, char *why, size_t wcap)
{
    fn_binding k[FN_KEYS_MAX];
    int n = 0;
    char buf[256];
    out[0] = 0;
    snprintf(buf, sizeof buf, "%s", in ? in : "");
    char *t = trim(buf);
    for (char *p = _stricmp(t, "none") ? t : NULL; p && *p;) {
        char *comma = strchr(p, ',');
        if (comma) *comma = 0;
        char *one = trim(p);
        fn_binding b;
        if (*one) {
            if (fn_parse_key(one, &b)) {
                if (why) snprintf(why, wcap, "\"%s\" is not a key name", one);
                return -1;
            }
            int dup = 0;
            for (int i = 0; i < n; i++) dup |= k[i].vk == b.vk && k[i].mods == b.mods;
            if (!dup && n == FN_KEYS_MAX) {
                if (why) snprintf(why, wcap, "at most %d keys", FN_KEYS_MAX);
                return -1;
            }
            if (!dup) k[n++] = b;
        }
        p = comma ? comma + 1 : NULL;
    }
    size_t len = 0;
    for (int i = 0; i < n; i++) {
        wchar_t w[48];
        fn_format_key(k[i], w, 48);
        int m = snprintf(out + len, cap - len, "%s%ls", i ? ", " : "", w);
        if (m < 0 || (size_t)m >= cap - len) break;
        len += (size_t)m;
    }
    if (!n) snprintf(out, cap, "none");
    return 0;
}

static int settings_edit_as(const char *text, size_t len, const char *sec, const char *key, const char *val, char **out,
                     size_t *out_len, char *why, size_t cap, const fn_def *proposed)
{
    *out = NULL;
    *out_len = 0;
    if (cap) why[0] = 0;
    if (!text) len = 0;
    int functions = !strcmp(sec, "functions"), keys = !strcmp(sec, "keys");
    int listed = functions || keys || !strcmp(sec, "menu") || !strcmp(sec, "palette"); /* key is a function */
    int fi = find_index(listed ? key : sec, strlen(listed ? key : sec));
    if (fi < 0 && !proposed) {
        snprintf(why, cap, "no function %s", listed ? key : sec);
        return -1;
    }
    const fn_def *d = fi < 0 ? proposed : R.s[fi].d;
    char norm[8192];
    size_t limit = !strncmp(d->id,"relay.",6) ? sizeof norm : 256;
    if (val) {
        if (strlen(val) >= limit) {
            snprintf(why, cap, "the value is too long");
            return -1;
        }
        for (const char *c = val; *c; c++)
            if ((uint8_t)*c < 32) {
                snprintf(why, cap, "the value is on more than one line");
                return -1;
            }
        snprintf(norm, sizeof norm, "%s", val);
        char *v = trim(norm);
        memmove(norm, v, strlen(v) + 1);
        val = norm;
    }
    char list[256];
    if (keys) {
        if (!d->run) {
            snprintf(why, cap, "%s has nothing a key could run", key);
            return -1;
        }
        if (val && bindings_text(val, list, sizeof list, why, cap)) return -1;
        char def[256];
        bindings_text(d->keys, def, sizeof def, NULL, 0);
        if (val) val = strcmp(list, def) ? list : NULL;
    } else if (listed) {
        if (sec[0] == 'm' && !d->menus) {
            snprintf(why, cap, "%s is in no context menu", key);
            return -1;
        }
        if (sec[0] == 'p' && (!d->run || (d->flags & FN_HIDDEN))) {
            snprintf(why, cap, "%s is never in the palette", key);
            return -1;
        }
        uint8_t on = 0;
        if (val && parse_bool(val, &on)) {
            snprintf(why, cap, "[%s] %s = %s: use on or off", sec, key, val);
            return -1;
        }
        if (val) val = on ? "on" : "off";
        if (val && on == (functions ? !(d->flags & FN_OPTIONAL) : 1)) val = NULL;
    } else {
        const fn_opt *o = find_opt(d, key);
        if (!o) {
            snprintf(why, cap, "[%s] has no option %s", sec, key);
            return -1;
        }
        key = o->name;
        char fit[160];
        if (val && !value_fits(o, val, fit, sizeof fit)) {
            snprintf(why, cap, "%s = %s: %s", key, val, fit);
            return -1;
        }
        const char *def = o->def ? o->def : "";
        if (val) {
            int is_def = !o->values ? !strcmp(val, def)
                         : o->values[0] == '#' ? strtol(val, NULL, 10) == strtol(def, NULL, 10)
                                               : !_stricmp(val, def);
            if (is_def) val = NULL;
        }
    }
    int icase = !listed;

    /* where things are */
    const char *eol = "\r\n";
    if (len && !memchr(text, '\r', len) && memchr(text, '\n', len)) eol = "\n";
    size_t start = len >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF ? 3 : 0;
    int in_sec = 0, found = 0;
    ini_line last = { 0 }, after_comment = { 0 }, sec_tail = { 0 };
    int have_comment = 0, have_sec = 0;
    for (size_t p = start; p < len;) {
        ini_line l = { p, p, p };
        while (l.b < len && text[l.b] != '\n') l.b++;
        l.next = l.b < len ? l.b + 1 : len;
        if (l.b > l.a && text[l.b - 1] == '\r') l.b--;
        ini_classify(text, &l);
        if (l.kind == LN_SECTION) {
            in_sec = name_is(text, &l, sec, 0);
            if (in_sec) sec_tail = l, have_sec = 1;
        } else if (in_sec && l.kind != LN_BLANK) {
            sec_tail = l;
            if (l.kind == LN_KV && name_is(text, &l, key, icase)) last = l, found = 1;
            if (l.kind == LN_COMMENT && !have_comment && name_is(text, &l, key, icase)) after_comment = l, have_comment = 1;
        }
        p = l.next;
    }

    size_t extra = strlen(sec) + strlen(key) + (val ? strlen(val) : 0) + 32;
    char *o = malloc(len + extra);
    if (!o) return -2;
    size_t n = 0;
#define PUT(s, k) (memcpy(o + n, (s), (k)), n += (k))
#define PUTS(s) PUT((s), strlen(s))
    if (!val) { /* every line for it goes: an earlier one would count otherwise */
        in_sec = 0;
        for (size_t p = 0; p < len;) {
            ini_line l = { p, p, p };
            while (l.b < len && text[l.b] != '\n') l.b++;
            l.next = l.b < len ? l.b + 1 : len;
            if (l.b > l.a && text[l.b - 1] == '\r') l.b--;
            if (p < start) l.a = start; /* the BOM stays */
            ini_classify(text, &l);
            if (l.kind == LN_SECTION) in_sec = name_is(text, &l, sec, 0);
            if (in_sec && l.kind == LN_KV && name_is(text, &l, key, icase)) PUT(text + p, l.a - p);
            else PUT(text + p, l.next - p);
            p = l.next;
        }
    } else if (found) {
        PUT(text, last.eq + 1);
        PUTS(" ");
        PUTS(val);
        PUT(text + last.b, len - last.b);
    } else if (have_comment || have_sec) {
        const ini_line *at = have_comment ? &after_comment : &sec_tail;
        PUT(text, at->next);
        if (at->next == at->b) PUTS(eol); /* the file ended without one */
        PUTS(key);
        PUTS(" = ");
        PUTS(val);
        PUTS(eol);
        PUT(text + at->next, len - at->next);
    } else {
        PUT(text, len);
        if (len > start) {
            if (text[len - 1] != '\n') PUTS(eol);
            if (!(len - start >= 2 && text[len - 2] == '\n') && !(len - start >= 3 && text[len - 3] == '\n' && text[len - 2] == '\r'))
                PUTS(eol);
        }
        PUTS("[");
        PUTS(sec);
        PUTS("]");
        PUTS(eol);
        PUTS(key);
        PUTS(" = ");
        PUTS(val);
        PUTS(eol);
    }
#undef PUT
#undef PUTS
    *out = o;
    *out_len = n;
    return 0;
}

int fn_settings_edit(const char *text, size_t len, const char *sec, const char *key, const char *val,
                     char **out, size_t *out_len, char *why, size_t cap)
{
    return settings_edit_as(text, len, sec, key, val, out, out_len, why, cap, NULL);
}
int fn_settings_family_edit(const char *text, size_t len, const char *id, const char *sec, const char *key,
                            const char *val, char **out, size_t *out_len, char *why, size_t cap)
{
    *out = NULL;
    *out_len = 0;
    const fn_def *family = NULL;
    for (uint32_t i = 0; i < R.nfamily; ++i) {
        const fn_def *f = R.family[i];
        size_t n = strlen(f->id);
        if (!strncmp(id, f->id, n) && family_name_ok(id + n)) {
            family = f;
            break;
        }
    }
    if (!family) {
        snprintf(why, cap, "Invalid command name or unavailable family");
        return -1;
    }
    int listed =
        !strcmp(sec, "functions") || !strcmp(sec, "keys") || !strcmp(sec, "menu") || !strcmp(sec, "palette");
    if (listed ? strcmp(key, id) != 0 : strcmp(sec, id) != 0) {
        snprintf(why, cap, "This edit must belong to %s", id);
        return -1;
    }
    if (find_index(id, strlen(id)) < 0 && R.n >= FN_MAX) {
        int free_slot = 0;
        for (uint32_t i = 0; i < R.n; ++i)
            if (R.s[i].fam && R.s[i].gone)
                free_slot = 1;
        if (!free_slot) {
            snprintf(why, cap, "The command registry is full");
            return -1;
        }
    }
    fn_def proposed = *family;
    proposed.id = id;
    return settings_edit_as(text, len, sec, key, val, out, out_len, why, cap, &proposed);
}
uint32_t fn_bindings(const fn_def *d, fn_binding *out, uint32_t cap)
{
    slot *s = slot_of(d);
    if (!s)
        return 0;
    uint32_t n = s->nkeys;
    if (n > cap)
        n = cap;
    if (n && out)
        memcpy(out, s->keys, n * sizeof *out);
    return s->nkeys;
}

const char *fn_settings_errors(void)
{
    size_t n = (size_t)snprintf(R.all_errors, sizeof R.all_errors, "%s", R.errors);
    for (uint32_t i = 0; i < R.n && n < sizeof R.all_errors - 1; i++)
        if (R.s[i].cerr[0]) n += (size_t)snprintf(R.all_errors + n, sizeof R.all_errors - n, "%s\n", R.s[i].cerr);
    return R.all_errors;
}

void fn_config_error(const fn_def *d, const char *fmt, ...)
{
    slot *s = slot_of(d);
    if (!s || s->cerr[0]) return; /* the first problem says enough */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->cerr, sizeof s->cerr, fmt, ap);
    va_end(ap);
}

static void configure_at(int i)
{
    R.s[i].cerr[0] = 0;
    if (R.s[i].d->configure) AS_SELF(R.s[i].d, R.s[i].d->configure());
}

void fn_configure_all(void)
{
    resolve();
    for (uint32_t i = 0; i < R.n; i++)
        if (!R.s[i].gone) configure_at((int)i);
}

uint32_t fn_revision(void) { return R.revision; }

void fn_reset(void)
{
    if(!fn_shutdown())return; /* pending completions still name their owners */
    for (uint32_t i = 0; i < FN_MAX; i++) {
        free(R.s[i].own);
        fpe_zone z = R.s[i].zone, zs = R.s[i].zstart;
        memset(&R.s[i], 0, sizeof R.s[i]);
        R.s[i].zone = z;
        R.s[i].zstart = zs;
    }
    free(R.text);
    free(R.kv);
    fn_host h = R.host;
    memset(&R, 0, sizeof R);
    R.host = h;
}

/* ---- queries --------------------------------------------------------------------- */

const fn_def *fn_find(const char *id)
{
    int i = id ? find_index(id, strlen(id)) : -1;
    return i < 0 ? NULL : R.s[i].d;
}

uint32_t fn_count(void) { return R.n; }
const fn_def *fn_at(uint32_t i) { return i < R.n ? R.s[i].d : NULL; }

static int active_at(int i, int depth)
{
    if (i < 0 || depth > 8) return 0;
    slot *s = &R.s[i];
    if (!s->on || s->gone) return 0;
    for (uint8_t k = 0; k < s->nneeds; k++)
        if (!active_at(s->needs[k], depth + 1)) return 0;
    return 1;
}

int fn_active(const fn_def *d)
{
    resolve();
    return active_at(index_of(d), 0);
}

int fn_running(const fn_def *d)
{
    slot *s = slot_of(d);
    return s && s->state == ST_RUNNING;
}

static int applies_at(int i, const fn_ctx *c)
{
    const fn_def *d = R.s[i].d;
    if (!active_at(i, 0) || R.s[i].state == ST_FAILED) return 0;
    if (c && !(d->where & c->where)) return 0;
    if (c && (c->state & d->requires) != d->requires) return 0;
    if (!d->applies) return 1;
    int ok = 0;
    AS_SELF(d, ok = d->applies(c));
    return ok;
}

int fn_applies(const fn_def *d, const fn_ctx *c)
{
    resolve();
    int i = index_of(d);
    return i >= 0 && applies_at(i, c);
}

int fn_in_palette(const fn_def *d)
{
    resolve();
    int i = index_of(d);
    return i >= 0 && d->run && !(d->flags & FN_HIDDEN) && R.s[i].pal_on && active_at(i, 0) &&
           R.s[i].state != ST_FAILED;
}

const char *fn_option(const fn_def *d, const char *name)
{
    const fn_opt *o = find_opt(d, name);
    for (uint32_t i = R.nkv; i-- > 0;) /* the last one wins */
        if (!strcmp(R.kv[i].sec, d->id) && !_stricmp(R.kv[i].key, name)) {
            if (R.kv[i].val) return R.kv[i].val;
            break; /* it didn't fit: the default */
        }
    return o && o->def ? o->def : "";
}

wchar_t **fn_paths(fn_ctx *c)
{
    if (!c->paths && c->n && c->gather) c->n = c->gather(c);
    return c->paths;
}

/* ---- timers ---------------------------------------------------------------------- */

/* One host timer serves every function: armed for the earliest deadline and
 * cancelled when nothing is due, so an idle app has no timer at all. */
static void rearm(void)
{
    uint64_t now = now_ms(), next = UINT64_MAX;
    for (uint32_t i = 0; i < R.n; i++) {
        slot *s = &R.s[i];
        if (s->state != ST_RUNNING) continue;
        if (s->period && s->due < next) next = s->due;
        if (s->d->life == FN_WARM && s->d->idle_ms && !s->holds && !s->depth) {
            uint64_t idle = s->last_used + s->d->idle_ms;
            if (idle < next) next = idle;
        }
    }
    if (!R.host.arm) return;
    if (next == UINT64_MAX) R.host.arm(0);
    else R.host.arm(next <= now ? 1 : (uint32_t)(next - now < 0x7FFFFFFF ? next - now : 0x7FFFFFFF));
}

int fn_timer(const fn_def *d, uint32_t period_ms)
{
    slot *s = slot_of(d);
    if (!s) return -1;
    if (period_ms && (d->life == FN_COLD || s->state != ST_RUNNING || !d->tick)) {
        /* a cold function must not leave anything behind it */
        s->violations++;
        error("%s asked for a timer but is %s", d->id, d->life == FN_COLD ? "cold" : "not running");
        return -1;
    }
    s->period = period_ms;
    s->due = now_ms() + period_ms;
    rearm();
    return 0;
}

void fn_tick(void)
{
    uint64_t now = now_ms();
    for (uint32_t i = 0; i < R.n; i++) {
        slot *s = &R.s[i];
        if (s->state != ST_RUNNING) continue;
        if (s->d->life == FN_WARM && s->d->idle_ms && !s->holds && !s->depth &&
            now >= s->last_used + s->d->idle_ms) {
            fn_stop(s->d);
            continue;
        }
        if (s->period && now >= s->due) {
            s->due = now + s->period;
            s->d->tick();
        }
    }
    rearm();
}

/* ---- lifetime -------------------------------------------------------------------- */

static void order_remove(int i)
{
    for (uint32_t k=0;k<R.norder;k++) if(R.order[k]==i) {
        memmove(R.order+k,R.order+k+1,R.norder-k-1);R.norder--;break;
    }
}
static void stopped_at(int i)
{
    slot *s=R.s+i;s->state=ST_IDLE;s->period=0;s->stop_called=0;order_remove(i);
}
static int start_at(int i, int depth)
{
    if (i < 0 || depth > 8) return -1;
    slot *s = &R.s[i];
    if (s->state == ST_RUNNING) return 0;
    if (s->state == ST_STARTING || s->state == ST_STOPPING) return FN_PENDING;
    if (s->state == ST_FAILED || !active_at(i, 0) || s->busy) return -1;
    s->busy = 1;
    int rc = 0;
    for (uint8_t k = 0; k < s->nneeds && !rc; k++) rc = start_at(s->needs[k], depth + 1);
    if (!rc && s->d->start) {
        int64_t t = fpe_prof_begin();
        AS_SELF(s->d, rc = s->d->start());
        fpe_prof_end(&s->zstart, t);
        if(rc==FN_PENDING) {
            /* Async starts must provide cancellation and own their resources
             * until stop_complete, even if stopped before start_complete. */
            s->state=ST_STARTING;
            if(R.norder<FN_MAX)R.order[R.norder++]=(uint8_t)i;
        } else if(rc) {
            s->state=ST_FAILED;tell(1,L"%hs could not start",s->d->id);
        }
    }
    s->busy = 0;
    if (rc) return rc==FN_PENDING?FN_PENDING:-1;
    s->state = ST_RUNNING;
    s->starts++;
    s->last_used = now_ms();
    if (R.norder < FN_MAX) R.order[R.norder++] = (uint8_t)i;
    return 0;
}
int fn_start_complete(const fn_def *d,int result)
{
    int i=index_of(d);if(i<0||R.s[i].state!=ST_STARTING)return 0;
    slot *s=R.s+i;
    if(result){s->state=ST_FAILED;order_remove(i);tell(1,L"%hs could not start",d->id);}
    else {s->state=ST_RUNNING;s->starts++;s->last_used=now_ms();}
    rearm();return 1;
}
int fn_start(const fn_def *d)
{
    resolve();
    int rc = start_at(index_of(d), 0);
    rearm();
    return rc;
}
static void stop_at(int i)
{
    slot *s = &R.s[i];
    if ((s->state!=ST_RUNNING&&s->state!=ST_STARTING&&s->state!=ST_STOPPING)||s->busy)return;
    s->busy=1;s->state=ST_STOPPING;s->period=0;
    int pending=0;
    for(uint32_t j=0;j<R.n;j++)for(uint8_t k=0;k<R.s[j].nneeds;k++)if(R.s[j].needs[k]==i){
        stop_at((int)j);if(R.s[j].state==ST_STOPPING)pending=1;
    }
    if(!pending&&!s->stop_called){
        int rc=0;s->stop_called=1;
        if(s->d->stop_async)AS_SELF(s->d,rc=s->d->stop_async());
        else if(s->d->stop)AS_SELF(s->d,s->d->stop());
        if(rc!=FN_PENDING)stopped_at(i);
    }
    s->busy=0;
}
int fn_stop_complete(const fn_def *d)
{
    int i=index_of(d);if(i<0||R.s[i].state!=ST_STOPPING||!R.s[i].stop_called)return 0;
    stopped_at(i);
    /* Dependencies remain alive until every stopping child has released its
     * resources. A child completion may now finish several parents. */
    for(uint32_t j=0;j<R.n;j++)if(R.s[j].state==ST_STOPPING)stop_at((int)j);
    rearm();return 1;
}
int fn_fail(const fn_def *d)
{
    int i=index_of(d);if(i<0)return 0;
    stop_at(i);if(R.s[i].state==ST_STOPPING)return 0;
    R.s[i].state=ST_FAILED;tell(1,L"%hs failed",d->id);rearm();return 1;
}

void fn_stop(const fn_def *d)
{
    int i = index_of(d);
    if (i < 0) return;
    stop_at(i);
    rearm();
}

void fn_start_hot(int deferred)
{
    resolve();
    R.hot_done |= deferred ? 2 : 1;
    for (uint32_t i = 0; i < R.n; i++) {
        const fn_def *d = R.s[i].d;
        if (d->life == FN_HOT && !(d->flags & FN_DEFERRED) == !deferred && active_at((int)i, 0) &&
            (!d->wanted || d->wanted()))
            start_at((int)i, 0);
    }
    rearm();
}

static int pending_start(int i,int depth)
{
    if(depth>8)return i;
    slot *s=R.s+i;
    for(uint8_t n=0;n<s->nneeds;n++){
        int child=s->needs[n];
        if(R.s[child].state!=ST_RUNNING&&R.s[child].state!=ST_FAILED)return pending_start(child,depth+1);
    }
    return i;
}
int fn_start_hot_step(int deferred,uint32_t *cursor)
{
    if(!cursor)return 0;
    resolve();R.hot_done|=deferred?2:1;
    while(*cursor<R.n){
        uint32_t i=*cursor;const fn_def *d=R.s[i].d;
        if(d->life==FN_HOT&&!(d->flags&FN_DEFERRED)==!deferred&&active_at((int)i,0)&&
           (!d->wanted||d->wanted())&&R.s[i].state!=ST_RUNNING&&R.s[i].state!=ST_FAILED){
            int selected=pending_start((int)i,0);int rc=start_at(selected,0);
            if((selected==(int)i&&rc!=FN_PENDING)||rc<0)(*cursor)++;
            rearm();return rc==FN_PENDING?FN_PENDING:1;
        }
        (*cursor)++;
    }
    rearm();return 0;
}

int fn_shutdown(void)
{
    /* A pending stop retains its order entry. Never spin or block the UI. */
    uint8_t order[FN_MAX];uint32_t n=R.norder;memcpy(order,R.order,n);
    while(n)stop_at(order[--n]);
    if(R.host.arm)R.host.arm(0);
    return R.norder==0;
}

/* What a function's settings come to, as a number: equal before and after a reload = unchanged. */
static uint64_t settings_hash(int i)
{
    const slot *s = &R.s[i];
    uint64_t h = 1469598103934665603ull;
#define MIX(byte) (h = (h ^ (uint8_t)(byte)) * 1099511628211ull)
    MIX(active_at(i, 0));
    for (const char *c = s->d->id; *c; c++) MIX(*c);
    MIX(s->on);
    MIX(s->menu_on);
    MIX(s->pal_on);
    for (uint8_t k = 0; k < s->nkeys; k++) MIX(s->keys[k].vk), MIX(s->keys[k].mods);
    MIX(0xFF);
    for (const fn_opt *o = s->d->opts; o && o->name; o++) {
        for (const char *v = fn_option(s->d, o->name); *v; v++) MIX(*v);
        MIX(0);
    }
#undef MIX
    return h;
}

/* Options and activation only: what configure reads. */
static uint64_t configure_hash(int i)
{
    uint64_t h = (uint64_t)active_at(i, 0) * 0x9E3779B97F4A7C15ull;
    for (const char *c = R.s[i].d->id; *c; c++) h = (h ^ (uint8_t)*c) * 1099511628211ull;
    for (const fn_opt *o = R.s[i].d->opts; o && o->name; o++)
        for (const char *v = fn_option(R.s[i].d, o->name);; v++) {
            h = (h ^ (uint8_t)*v) * 1099511628211ull;
            if (!*v) break;
        }
    return h;
}

int fn_reload(const char *text, size_t len)
{
    resolve();
    uint64_t before[FN_MAX], conf[FN_MAX];
    uint32_t had = R.n;
    for (uint32_t i = 0; i < R.n; i++) {
        before[i] = R.s[i].gone ? 0 : settings_hash((int)i);
        conf[i] = R.s[i].gone ? 0 : configure_hash((int)i);
    }
    if (fn_settings(text, len) == -2) return -1; /* out of memory: nothing changed */
    resolve();
    int changed = 0;
    for (uint32_t i = 0; i < R.n; i++) {
        int fresh = i >= had, gone = R.s[i].gone;
        int differs = fresh || (gone ? before[i] != 0 : settings_hash((int)i) != before[i]);
        changed += differs;
        if (gone) continue;
        /* a function that failed to start gets another chance with new settings */
        if (differs && R.s[i].state == ST_FAILED) R.s[i].state = ST_IDLE;
        if ((fresh || configure_hash((int)i) != conf[i]) && !(R.s[i].d->flags & FN_RESTART)) configure_at((int)i);
    }
    /* switched off (or something they need is): stopped, dependents first */
    for (uint32_t i = R.n; i-- > 0;)
        if ((R.s[i].state == ST_RUNNING || R.s[i].state == ST_STARTING) && !active_at((int)i, 0)) stop_at((int)i);
    /* hot ones that are now on, or wanted, start as they would have at startup;
     * ones no longer wanted stop unless they are in use */
    for (uint32_t i = 0; i < R.n; i++) {
        slot *s = &R.s[i];
        const fn_def *d = s->d;
        if (d->life != FN_HOT || !(R.hot_done & (d->flags & FN_DEFERRED ? 2 : 1))) continue;
        int want = active_at((int)i, 0) && (!d->wanted || d->wanted());
        if (want && s->state == ST_IDLE) start_at((int)i, 0);
        else if (!want && (s->state == ST_RUNNING || s->state == ST_STARTING) && !s->holds && !s->depth) stop_at((int)i);
        else if(!want&&s->state==ST_FAILED)s->state=ST_IDLE;
    }
    rearm();
    return changed;
}

static void touch(int i, uint64_t now, int depth)
{
    if (i < 0 || depth > 8) return;
    R.s[i].last_used = now;
    for (uint8_t k = 0; k < R.s[i].nneeds; k++) touch(R.s[i].needs[k], now, depth + 1);
}

/* A cold function stops once nothing of it is left running. */
static void settle(int i)
{
    slot *s = &R.s[i];
    if (s->depth || s->holds) return;
    if (s->d->life == FN_COLD) stop_at(i);
    rearm();
}

int fn_hold(const fn_def *d)
{
    resolve();
    int i = index_of(d);
    if (i < 0) return -1;
    if (R.s[i].state != ST_RUNNING && start_at(i, 0)) return -1;
    R.s[i].holds++;
    return 0;
}

void fn_release(const fn_def *d)
{
    int i = index_of(d);
    if (i < 0) return;
    if (!R.s[i].holds) {
        R.s[i].violations++;
        error("%s released more than it held", d->id);
        return;
    }
    R.s[i].holds--;
    /* held means in use: the idle time of a warm function starts now */
    R.s[i].last_used = now_ms();
    settle(i);
}

/* ---- invoking ---------------------------------------------------------------------- */

static int begin_at(int i)
{
    slot *s = &R.s[i];
    if (start_at(i, 0)) {
        rearm();
        return -1;
    }
    s->runs++;
    touch(i, now_ms(), 0);
    if (!s->depth++) s->t0 = fpe_prof_begin();
    return 0;
}

static void end_at(int i)
{
    slot *s = &R.s[i];
    if (!s->depth) return;
    if (!--s->depth) fpe_prof_end(&s->zone, s->t0);
    s->last_used = now_ms();
    settle(i);
}

int fn_begin(const fn_def *d)
{
    resolve();
    int i = index_of(d);
    if (i < 0 || !active_at(i, 0)) return -1;
    return begin_at(i);
}

void fn_end(const fn_def *d)
{
    int i = index_of(d);
    if (i >= 0) end_at(i);
}

static int run_at(int i, fn_ctx *c)
{
    if (!applies_at(i, c) || !R.s[i].d->run) return 0;
    if (begin_at(i)) return -1;
    AS_SELF(R.s[i].d, R.s[i].d->run(c));
    end_at(i);
    return 1;
}

int fn_run(const fn_def *d, fn_ctx *c)
{
    resolve();
    int i = index_of(d);
    return i < 0 ? 0 : run_at(i, c);
}

int fn_run_id(const char *id, fn_ctx *c) { return fn_run(fn_find(id), c); }

int fn_run_id_at_revision(const char *id, uint32_t revision, fn_ctx *c)
{
    return revision == R.revision ? fn_run_id(id, c) : 0;
}

int fn_key(uint8_t vk, uint8_t mods, fn_ctx *c)
{
    resolve();
    for (uint32_t i = 0; i < R.n; i++) {
        slot *s = &R.s[i];
        for (uint8_t k = 0; k < s->nkeys; k++) {
            if (s->keys[k].vk != vk || s->keys[k].mods != mods) continue;
            int r = run_at((int)i, c);
            if (r) return r;
            break;
        }
    }
    return 0;
}

int fn_verb(const char *verb, fn_ctx *c)
{
    resolve();
    for (uint32_t i = 0; i < R.n; i++)
        if (R.s[i].d->verbs && has_word(R.s[i].d->verbs, verb)) {
            int r = run_at((int)i, c);
            if (r) return r;
        }
    return 0;
}

size_t fn_verbs(const fn_ctx *c, char *buf, size_t cap)
{
    resolve();
    size_t len = 0;
    if (cap) buf[0] = 0;
    for (uint32_t i = 0; i < R.n; i++) {
        const fn_def *d = R.s[i].d;
        if (!d->verbs || !d->run || !applies_at((int)i, c)) continue;
        int n = snprintf(buf + len, cap - len, "%s%s", len ? " " : "", d->verbs);
        if (n < 0 || (size_t)n >= cap - len) break;
        len += (size_t)n;
    }
    return len;
}

uint32_t fn_menu(HMENU m, uint32_t menu, const fn_ctx *c)
{
    resolve();
    uint32_t added = 0;
    int last_group = -1, max_group = 0;
    for (uint32_t i = 0; i < R.n; i++)
        if (R.s[i].d->group > max_group) max_group = R.s[i].d->group;
    for (int g = 0; g <= max_group; g++) {
        for (uint32_t i = 0; i < R.n; i++) {
            slot *s = &R.s[i];
            const fn_def *d = s->d;
            if (d->group != g || !(d->menus & menu & ~FN_MENU_OURS) || !s->menu_on || !d->run) continue;
            if ((d->menus & FN_MENU_OURS) && !(menu & FN_MENU_OURS)) continue;
            if (!applies_at((int)i, c)) continue;
            if (last_group >= 0 && last_group != g) AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            last_group = g;
            wchar_t text[220], key[48];
            if (d->label) AS_SELF(d, d->label(c, text, 160));
            else _snwprintf(text, 160, L"%ls", d->title);
            text[159] = 0;
            if (fn_key_text(d, key, 48)) wcscat(wcscat(text, L"\t"), key);
            AppendMenuW(m, MF_STRING, FN_MENU_ID0 + i, text);
            added++;
        }
    }
    return added;
}

int fn_menu_run(UINT id, fn_ctx *c)
{
    if (c && c->from == FN_FROM_MENU && c->menu_revision != R.revision) return 0;
    if (id < FN_MENU_ID0 || id >= FN_MENU_ID0 + R.n) return 0;
    return run_at((int)(id - FN_MENU_ID0), c);
}

/* ---- reports ------------------------------------------------------------------------- */

typedef struct out {
    char  *p;
    size_t cap, len;
} out;

static void put(out *o, const char *fmt, ...)
{
    if (o->len + 1 >= o->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->p + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n > 0) o->len += (size_t)n < o->cap - o->len ? (size_t)n : o->cap - o->len - 1;
}

static void keys_text(const slot *s, char *buf, size_t cap)
{
    size_t len = 0;
    buf[0] = 0;
    for (uint8_t k = 0; k < s->nkeys; k++) {
        wchar_t w[48];
        fn_format_key(s->keys[k], w, 48);
        int n = snprintf(buf + len, cap - len, "%s%ls", k ? ", " : "", w);
        if (n < 0 || (size_t)n >= cap - len) break;
        len += (size_t)n;
    }
    if (!len) snprintf(buf, cap, "-");
}

size_t fn_report(char *buf, size_t cap)
{
    resolve();
    out o = { buf, cap, 0 };
    if (cap) buf[0] = 0;
    uint32_t on = 0, running = 0, violations = 0;
    for (uint32_t i = 0; i < R.n; i++) {
        on += active_at((int)i, 0);
        running += R.s[i].state == ST_RUNNING;
        violations += R.s[i].violations;
    }
    put(&o, "functions          %u, %u on, %u running\n", R.n, on, running);
    for (uint32_t i = 0; i < R.n; i++) {
        const slot *s = &R.s[i];
        if (s->gone || (s->state == ST_IDLE && !s->runs && !s->violations)) continue;
        char extra[64] = "";
        int n = 0;
        if (s->holds) n += snprintf(extra + n, sizeof extra - n, "  holds %u", s->holds);
        if (s->period) n += snprintf(extra + n, sizeof extra - n, "  timer %ums", s->period);
        if (s->violations) snprintf(extra + n, sizeof extra - n, "  VIOLATIONS %u", s->violations);
        put(&o, "  %-22s %-4s %-7s %5u%s\n", s->d->id, life_names[s->d->life], state_names[s->state], s->runs, extra);
    }
    if (violations) put(&o, "  lifetime violations: %u\n", violations);
    const char *errs = fn_settings_errors();
    if (errs[0]) put(&o, "settings problems:\n%s", errs);
    return o.len;
}

size_t fn_report_csv(char *buf, size_t cap)
{
    resolve();
    out o = { buf, cap, 0 };
    if (cap) buf[0] = 0;
    uint32_t violations = 0;
    for (uint32_t i = 0; i < R.n; i++) {
        const slot *s = &R.s[i];
        if (s->gone) continue;
        const char *st = !active_at((int)i, 0) ? "off" : state_names[s->state];
        put(&o, "fn,%s,%u,,,%s,%u\n", s->d->id, s->runs, st, s->holds);
        violations += s->violations;
    }
    put(&o, "metric,fn_violations,,,,%u,\n", violations);
    return o.len;
}

size_t fn_list(char *buf, size_t cap)
{
    resolve();
    out o = { buf, cap, 0 };
    if (cap) buf[0] = 0;
    put(&o, "%-24s %-4s %-4s %-24s %s\n", "function", "life", "on", "keys", "what it does");
    for (uint32_t i = 0; i < R.n; i++) {
        const slot *s = &R.s[i];
        const fn_def *d = s->d;
        if (s->gone) continue;
        char keys[96];
        keys_text(s, keys, sizeof keys);
        const char *on = active_at((int)i, 0) ? "on" : s->on ? "off*" : "off";
        put(&o, "%-24s %-4s %-4s %-24s %s\n", d->id, life_names[d->life], on, keys, d->about ? d->about : "");
        if (d->needs) put(&o, "%-24s needs %s\n", "", d->needs);
        if (d->life == FN_HOT && d->wanted && !d->wanted()) put(&o, "%-24s not started: the current settings don't need it\n", "");
        if (d->menus && !s->menu_on) put(&o, "%-24s hidden from menus\n", "");
        if (d->run && !(d->flags & FN_HIDDEN) && !s->pal_on) put(&o, "%-24s hidden from the palette\n", "");
        for (const fn_opt *p = d->opts; p && p->name; p++)
            put(&o, "%-24s [%s] %s = %s   (%s)\n", "", d->id, p->name, fn_option(d, p->name), p->help ? p->help : "");
    }
    put(&o, "\noff* = switched on, but something it needs is off\n");
    const char *errs = fn_settings_errors();
    if (errs[0]) put(&o, "\nsettings problems:\n%s", errs);
    return o.len;
}

/* Writes what fits, counts everything: the caller learns the size it needs. */
typedef struct sink {
    char  *p;
    size_t cap, need;
} sink;

static void sink_raw(sink *k, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++, k->need++)
        if (k->need + 1 < k->cap) k->p[k->need] = s[i];
    if (k->cap) k->p[k->need + 1 < k->cap ? k->need : k->cap - 1] = 0;
}

static void sink_put(sink *k, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) sink_raw(k, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

/* A field: a tab first, and no tabs or line breaks inside. */
static void sink_field(sink *k, const char *s)
{
    sink_raw(k, "\t", 1);
    for (; s && *s; s++) {
        char c = *s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s;
        sink_raw(k, &c, 1);
    }
}

static void sink_wfield(sink *k, const wchar_t *s)
{
    char u[512];
    if (!s || !WideCharToMultiByte(CP_UTF8, 0, s, -1, u, sizeof u, NULL, NULL)) u[0] = 0;
    sink_field(k, u);
}

/* The value given for an option in the settings, or NULL. */
static const char *option_set(const fn_def *d, const char *name)
{
    for (uint32_t i = R.nkv; i-- > 0;)
        if (!strcmp(R.kv[i].sec, d->id) && !_stricmp(R.kv[i].key, name)) return R.kv[i].val;
    return NULL;
}

size_t fn_describe(char *buf, size_t cap)
{
    resolve();
    sink k = { buf, cap, 0 };
    if (cap) buf[0] = 0;
    for (uint32_t i = 0; i < R.n; i++) {
        const slot *s = &R.s[i];
        const fn_def *d = s->d;
        if (s->gone) continue;
        char keys[128], defkeys[256], flags[12];
        int nf = 0;
        keys_text(s, keys, sizeof keys);
        if (!strcmp(keys, "-")) keys[0] = 0;
        bindings_text(d->keys, defkeys, sizeof defkeys, NULL, 0);
        if (!strcmp(defkeys, "none")) defkeys[0] = 0;
        if (d->flags & FN_RESTART) flags[nf++] = 'r';
        if (d->menus && !s->menu_on) flags[nf++] = 'm';
        if (d->run && !(d->flags & FN_HIDDEN) && !s->pal_on) flags[nf++] = 'p';
        if (d->flags & FN_OPTIONAL) flags[nf++] = 'o';
        if (d->run) flags[nf++] = 'k';
        if (d->menus) flags[nf++] = 'M';
        if (d->run && !(d->flags & FN_HIDDEN)) flags[nf++] = 'P';
        if (s->fam) flags[nf++] = 'f';
        flags[nf] = 0;
        sink_raw(&k, "fn", 2);
        sink_field(&k, d->id);
        sink_wfield(&k, d->title ? d->title : L"");
        sink_field(&k, life_names[d->life]);
        sink_field(&k, active_at((int)i, 0) ? "on" : s->on ? "needs" : "off");
        sink_field(&k, keys);
        sink_field(&k, defkeys);
        sink_field(&k, flags);
        sink_field(&k, d->needs ? d->needs : "");
        sink_field(&k, d->about ? d->about : "");
        sink_raw(&k, "\n", 1);
        for (const fn_opt *o = d->opts; o && o->name; o++) {
            sink_raw(&k, "opt", 3);
            sink_field(&k, o->name);
            sink_field(&k, fn_option(d, o->name));
            sink_field(&k, o->def ? o->def : "");
            sink_field(&k, option_set(d, o->name) ? "1" : "0");
            sink_field(&k, o->values ? o->values : "");
            sink_field(&k, o->help ? o->help : "");
            sink_raw(&k, "\n", 1);
        }
    }
    for (const char *e = fn_settings_errors(); *e;) {
        const char *nl = strchr(e, '\n');
        size_t n = nl ? (size_t)(nl - e) : strlen(e);
        char line[300];
        snprintf(line, sizeof line, "%.*s", (int)n, e);
        sink_raw(&k, "error", 5);
        sink_field(&k, line);
        sink_raw(&k, "\n", 1);
        e += n + (nl != NULL);
    }
    return k.need;
}

static void template_opts(sink *k, const fn_def *d)
{
    for (const fn_opt *o = d->opts; o && o->name; o++) {
        sink_put(k, "; %s: %s", o->name, o->help ? o->help : "");
        if (o->values && o->values[0] == '#') {
            long lo = 0, hi = 0;
            sscanf(o->values + 1, "%ld..%ld", &lo, &hi);
            sink_put(k, " (%ld to %ld)", lo, hi);
        } else if (o->values && o->values[0] == '+') {
            sink_put(k, " (some of: %s)", o->values + 1);
        } else if (o->values) {
            sink_put(k, " (%s)", o->values);
        }
        sink_put(k, "\r\n;%s = %s\r\n", o->name, o->def ? o->def : "");
    }
}
size_t fn_template(char *buf, size_t cap)
{
    resolve();
    sink k = { buf, cap, 0 };
    if (cap) buf[0] = 0;
    sink_put(&k, "; Folderwing settings. Saved changes apply at once; Ctrl+, shows every function\r\n"
                 "; and option with what it does. Remove the ; in front of a line to use it.\r\n\r\n"
                 "[functions]\r\n; id = on | off\r\n");
    for (uint32_t i = 0; i < R.n; i++)
        if (!R.s[i].fam) sink_put(&k, ";%s = %s\r\n", R.s[i].d->id, R.s[i].d->flags & FN_OPTIONAL ? "off" : "on");
    sink_put(&k, "\r\n[keys]\r\n; id = F2, Ctrl+Shift+R | none\r\n");
    for (uint32_t i = 0; i < R.n; i++)
        if (R.s[i].d->run && !R.s[i].fam) sink_put(&k, ";%s = %s\r\n", R.s[i].d->id, R.s[i].d->keys ? R.s[i].d->keys : "none");
    sink_put(&k, "\r\n[menu]\r\n; id = off: not in context menus\r\n\r\n[palette]\r\n; id = off: not in the command palette\r\n");
    for (uint32_t i = 0; i < R.n; i++) {
        const fn_def *d = R.s[i].d;
        if (!d->opts || !d->opts->name || R.s[i].fam) continue;
        sink_put(&k, "\r\n[%s]\r\n", d->id);
        template_opts(&k, d);
    }
    for (uint32_t f = 0; f < R.nfamily; f++) {
        const fn_def *t = R.family[f];
        sink_put(&k, "\r\n; [%sNAME]: %s\r\n;[%sexample]\r\n", t->id, t->about ? t->about : "", t->id);
        template_opts(&k, t);
    }
    return k.need;
}
