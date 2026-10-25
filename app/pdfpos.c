/* Where each PDF was left: see pdfpos.h. */
#include "pdfpos.h"
#include "fpe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    wchar_t *file;
    uint32_t max;
    int      loaded, dirty;
    pdfpos  *v; /* newest first */
    uint32_t n, cap;
} P;

static void entry_free(pdfpos *e) { free(e->path); }

static void list_free(pdfpos *v, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) entry_free(&v[i]);
    free(v);
}

static int reserve(pdfpos **v, uint32_t *cap, uint32_t need)
{
    if (need <= *cap) return 1;
    uint32_t c = *cap ? *cap * 2 : 64;
    while (c < need) c *= 2;
    pdfpos *p = realloc(*v, c * sizeof *p);
    if (!p) return 0;
    *v = p;
    *cap = c;
    return 1;
}

static int index_of(const pdfpos *v, uint32_t n, const wchar_t *doc)
{
    for (uint32_t i = 0; i < n; i++)
        if (fpe_path_equal(v[i].path, doc)) return (int)i;
    return -1;
}

/* Reads the file into a fresh list; a missing or unreadable file is an empty list. */
static void read_file(const wchar_t *file, pdfpos **out, uint32_t *n_out, uint32_t max)
{
    *out = NULL;
    *n_out = 0;
    uint32_t n = 0, cap = 0;
    pdfpos *v = NULL;
    HANDLE f = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size;
    char *text = NULL;
    DWORD got = 0;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (16 << 20) &&
        (text = malloc((size_t)size.QuadPart + 1)) && ReadFile(f, text, (DWORD)size.QuadPart, &got, NULL))
        text[got] = 0;
    else
        got = 0;
    CloseHandle(f);
    int versioned = 0;
    for (char *line = text, *next; got && line && *line && n < max; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') line[--len] = 0;
        if (line[0] == '#') {
            if (!strcmp(line, "# size\tmtime\tpage\ty\tzoom\tx\tpath")) versioned = 1;
            continue;
        }
        if (!len) continue;
        char *p = line, *end;
        pdfpos e = { 0 };
        e.size = _strtoui64(p, &end, 10);
        if (end == p || *end != '\t') continue;
        p = end + 1;
        if (versioned) {
            e.mtime = _strtoi64(p, &end, 10);
            if (end == p || *end != '\t') continue;
            p = end + 1;
        }
        e.page = (uint32_t)strtoul(p, &end, 10);
        if (end == p || *end != '\t') continue;
        p = end + 1;
        e.y = strtof(p, &end);
        if (end == p || *end != '\t') continue;
        p = end + 1;
        e.zoom = strtof(p, &end);
        if (end == p || *end != '\t') continue;
        p = end + 1;
        e.x = strtof(p, &end);
        if (end == p || *end != '\t' || !end[1]) continue;
        p = end + 1;
        int wl = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, -1, NULL, 0);
        if (wl <= 1 || wl > 32768 || !(e.y >= 0 && e.y < 1e6f) || !(e.zoom >= 0 && e.zoom < 100) ||
            !(e.x > -1e6f && e.x < 1e6f))
            continue;
        e.path = malloc((size_t)wl * sizeof *e.path);
        if (!e.path) continue;
        MultiByteToWideChar(CP_UTF8, 0, p, -1, e.path, wl);
        if (index_of(v, n, e.path) >= 0 || !reserve(&v, &cap, n + 1)) {
            entry_free(&e);
            continue;
        }
        v[n++] = e;
    }
    free(text);
    *out = v;
    *n_out = n;
}

static void load(void)
{
    if (P.loaded) return;
    P.loaded = 1;
    if (P.file && P.max) read_file(P.file, &P.v, &P.n, P.max);
    P.cap = P.n;
}

void pdfpos_open(const wchar_t *file, uint32_t max)
{
    pdfpos_close();
    P.file = file && file[0] ? _wcsdup(file) : NULL;
    P.max = P.file ? max : 0;
}

const pdfpos *pdfpos_get(const wchar_t *doc, uint64_t size, int64_t mtime)
{
    if (!P.max || !doc) return NULL;
    load();
    int i = index_of(P.v, P.n, doc);
    return i >= 0 && P.v[i].size == size && P.v[i].mtime == mtime ? &P.v[i] : NULL;
}

int pdfpos_has(const wchar_t *doc)
{
    if (!P.max || !doc) return 0;
    load();
    return index_of(P.v, P.n, doc) >= 0;
}

void pdfpos_set(const wchar_t *doc, uint64_t size, int64_t mtime, uint32_t page, float y, float zoom, float x)
{
    if (!P.max || !doc || !doc[0]) return;
    load();
    int i = index_of(P.v, P.n, doc);
    pdfpos e;
    if (i >= 0) {
        e = P.v[i];
        memmove(P.v + 1, P.v, (size_t)i * sizeof *P.v);
    } else {
        memset(&e, 0, sizeof e);
        if (!(e.path = _wcsdup(doc)) || !reserve(&P.v, &P.cap, P.n + 1)) {
            free(e.path);
            return;
        }
        memmove(P.v + 1, P.v, (size_t)P.n * sizeof *P.v);
        P.n++;
    }
    e.size = size, e.mtime = mtime, e.page = page, e.y = y, e.zoom = zoom, e.x = x, e.touched = 1;
    P.v[0] = e;
    while (P.n > P.max) entry_free(&P.v[--P.n]);
    P.dirty = 1;
}

static int write_file(const wchar_t *file, const pdfpos *v, uint32_t n)
{
    size_t cap = 256 + (size_t)n * 128, len = 0;
    for (uint32_t i = 0; i < n; i++) cap += wcslen(v[i].path) * 3;
    char *buf = malloc(cap);
    if (!buf) return -1;
    len += (size_t)snprintf(buf, cap, "# Folderwing: where each PDF was left, newest first\r\n"
                                      "# size\tmtime\tpage\ty\tzoom\tx\tpath\r\n");
    for (uint32_t i = 0; i < n; i++) {
        len += (size_t)snprintf(buf + len, cap - len, "%llu\t%lld\t%u\t%.1f\t%.3f\t%.1f\t", (unsigned long long)v[i].size,
                                (long long)v[i].mtime, v[i].page, v[i].y, v[i].zoom, v[i].x);
        int w = WideCharToMultiByte(CP_UTF8, 0, v[i].path, -1, buf + len, (int)(cap - len), NULL, NULL);
        if (w <= 0) {
            free(buf);
            return -1;
        }
        len += (size_t)w - 1;
        len += (size_t)snprintf(buf + len, cap - len, "\r\n");
    }
    wchar_t tmp[MAX_PATH + 64];
    _snwprintf(tmp, MAX_PATH + 64, L"%ls.%lu.tmp", file, GetCurrentProcessId());
    tmp[MAX_PATH + 63] = 0;
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    int ok = 0;
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w;
        ok = WriteFile(f, buf, (DWORD)len, &w, NULL) && w == len;
        CloseHandle(f);
        ok = ok && MoveFileExW(tmp, file, MOVEFILE_REPLACE_EXISTING);
        if (!ok) DeleteFileW(tmp);
    }
    free(buf);
    return ok ? 0 : -1;
}

int pdfpos_save(void)
{
    if (!P.max || !P.dirty || !P.file) return 0;
    /* the file as another window may have left it, under what this run changed */
    pdfpos *disk;
    uint32_t nd;
    read_file(P.file, &disk, &nd, P.max);
    pdfpos *out = NULL;
    uint32_t n = 0, cap = 0;
    for (int pass = 0; pass < 2; pass++) {
        const pdfpos *src = pass ? disk : P.v;
        uint32_t ns = pass ? nd : P.n;
        for (uint32_t i = 0; i < ns && n < P.max; i++) {
            if (!pass && !src[i].touched) continue;
            if (index_of(out, n, src[i].path) >= 0 || !reserve(&out, &cap, n + 1)) continue;
            out[n] = src[i];
            out[n].path = _wcsdup(src[i].path);
            if (!out[n].path) continue;
            out[n++].touched = 0;
        }
    }
    /* touched entries first, newest first, then the file's in its order */
    int rc = write_file(P.file, out, n);
    list_free(disk, nd);
    if (!rc) {
        /* what the file now says is what we know; nothing left to write */
        list_free(P.v, P.n);
        P.v = out;
        P.n = P.cap = n;
        P.dirty = 0;
    } else {
        list_free(out, n);
    }
    return rc;
}

void pdfpos_close(void)
{
    pdfpos_save();
    list_free(P.v, P.n);
    free(P.file);
    memset(&P, 0, sizeof P);
}

uint32_t pdfpos_count(void)
{
    load();
    return P.n;
}
