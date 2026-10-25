/* The bounded line protocol shared by OpenPilot and folderwing-shell.exe. A failed
 * append leaves the prior buffer intact; callers must discard the request. */
#ifndef OPENPILOT_SHELLWIRE_H
#define OPENPILOT_SHELLWIRE_H

#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SHELL_LINE_MAX 8191u
#define SHELL_REQUEST_MAX (1024u * 1024u)
#define SHELL_ITEMS_MAX 4096u

#ifndef SHELLWIRE_REALLOC
#define SHELLWIRE_REALLOC realloc
#endif

typedef struct shellwire_text {
    char *p;
    size_t len, cap;
} shellwire_text;

static inline int shellwire_add(shellwire_text *t, const char *key, const wchar_t *val)
{
    if (!t || !key || !val) return -1;
    size_t key_len = strlen(key);
    if (!key_len || key_len >= SHELL_LINE_MAX) return -1;
    for (const wchar_t *p = val; *p; p++) if (*p == L'\r' || *p == L'\n') return -1;
    int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, val, -1, NULL, 0, NULL, NULL);
    if (bytes < 1) return -1;
    size_t line_len = key_len + 1 + (size_t)bytes - 1;
    if (line_len > SHELL_LINE_MAX || t->len > SHELL_REQUEST_MAX - line_len - 1) return -1;
    size_t need = t->len + line_len + 2; /* newline and terminator */
    if (need > t->cap) {
        size_t cap = need > (SHELL_REQUEST_MAX + 1) / 2 ? SHELL_REQUEST_MAX + 1 : need * 2;
        char *p = SHELLWIRE_REALLOC(t->p, cap);
        if (!p) return -1;
        t->p = p;
        t->cap = cap;
    }
    char *line = t->p + t->len;
    memcpy(line, key, key_len);
    line[key_len] = ' ';
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, val, -1, line + key_len + 1,
                             bytes, NULL, NULL)) return -1;
    t->len += line_len;
    t->p[t->len++] = '\n';
    t->p[t->len] = 0;
    return 0;
}

#endif
