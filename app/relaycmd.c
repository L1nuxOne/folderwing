/* relaycmd.c - see relaycmd.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "relaycmd.h"

#include <string.h>

typedef struct buf {
    wchar_t *p;
    size_t   cap, len;
    int      over;
} buf;

static void put(buf *b, const wchar_t *s, size_t n)
{
    if (b->len + n >= b->cap) {
        b->over = 1;
        return;
    }
    memcpy(b->p + b->len, s, n * sizeof *s);
    b->len += n;
    b->p[b->len] = 0;
}

/* Windows argument quoting: a final backslash must be doubled before a
 * closing quote. A placeholder already in quotes only needs that escape
 * when the template closes its quote immediately after the placeholder. */
static void put_quoted(buf *b, const wchar_t *s, int quote)
{
    if (quote > 0) put(b, L"\"", 1);
    const wchar_t *p=s;
    while(*p){
        unsigned slashes=0;while(*p==L'\\'){++slashes;++p;}
        unsigned emit=slashes;
        if(*p==L'\"')emit=slashes*2+1;
        else if(!*p&&quote)emit=slashes*2;
        for(unsigned i=0;i<emit;++i)put(b,L"\\",1);
        if(*p)put(b,p++,1);
    }
    if(quote>0)put(b,L"\"",1);
}

static int name_char(wchar_t c)
{
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'_' || c == L'(' ||
           c == L')';
}

int relay_expand(const wchar_t *cmd, const wchar_t *const *paths, uint32_t n, int item, const wchar_t *dir,
                 wchar_t *out, size_t cap)
{
    buf b = { out, cap, 0, 0 };
    if (cap) out[0] = 0;
    int quoted = 0;
    const wchar_t *one = n ? paths[item >= 0 && (uint32_t)item < n ? item : 0] : dir;
    for (const wchar_t *p = cmd; *p && !b.over; p++) {
        if (*p == L'"') quoted = !quoted;
        if (*p != L'%') {
            put(&b, p, 1);
            continue;
        }
        wchar_t c = p[1];
        if (c == L'%') {
            put(&b, L"%", 1);
            p++;
            continue;
        }
        /* %NAME%: only when the variable exists, so %1%2 style text survives */
        const wchar_t *e = p + 1;
        while (name_char(*e)) e++;
        if (*e == L'%' && e > p + 1 && e - p < 64) {
            wchar_t name[64], val[2048];
            wmemcpy(name, p + 1, (size_t)(e - p - 1));
            name[e - p - 1] = 0;
            DWORD got = GetEnvironmentVariableW(name, val, 2048);
            if (got && got < 2048) {
                put(&b, val, got);
                p = e;
                continue;
            }
            /* An unset named variable stays whole, even if its first letter
             * is also a folder/item placeholder. Numeric %1%2 remains legacy. */
            if((name[0]>=L'A'&&name[0]<=L'Z')||(name[0]>=L'a'&&name[0]<=L'z')||name[0]==L'_'){
                put(&b,p,(size_t)(e-p+1));p=e;continue;
            }
        }
        switch (c) {
        case L'1': case L'L': case L'l': case L'V': case L'v':
            if (one) put_quoted(&b, one, !quoted ? 1 : p[2]==L'"' ? -1 : 0);
            p++;
            break;
        case L'*':
            for (uint32_t i = 0; i < n; i++) {
                if (i) put(&b, L" ", 1);
                put_quoted(&b, paths[i], !quoted ? 1 : p[2]==L'"' ? -1 : 0);
            }
            p++;
            break;
        case L'D': case L'd': case L'W': case L'w':
            if (dir) put_quoted(&b, dir, !quoted ? 1 : p[2]==L'"' ? -1 : 0);
            p++;
            break;
        default:
            put(&b, p, 1);
        }
    }
    return b.over ? -1 : (int)b.len;
}

int relay_takes_all(const wchar_t *cmd)
{
    for (const wchar_t *p = cmd; *p; p++) {
        if (p[0] == L'%' && p[1] == L'%') p++;
        else if (p[0] == L'%' && p[1] == L'*') return 1;
    }
    return 0;
}

void relay_split(const wchar_t *cmd, wchar_t *program, size_t pcap, const wchar_t **rest)
{
    while (*cmd == L' ' || *cmd == L'\t') cmd++;
    const wchar_t *s = cmd, *e;
    if (*s == L'"') {
        s++;
        e = wcschr(s, L'"');
        if (!e) e = s + wcslen(s);
    } else {
        e = s;
        while (*e && *e != L' ' && *e != L'\t') e++;
    }
    size_t n = (size_t)(e - s) < pcap - 1 ? (size_t)(e - s) : pcap - 1;
    wmemcpy(program, s, n);
    program[n] = 0;
    if (*e == L'"') e++;
    while (*e == L' ' || *e == L'\t') e++;
    *rest = e;
}

int relay_type_ok(const wchar_t *path, const char *types)
{
    if (!types || !types[0]) return 1;
    const wchar_t *slash = wcsrchr(path, L'\\'), *dot = wcsrchr(path, L'.');
    if (!dot || (slash && dot < slash)) return 0;
    dot++;
    for (const char *p = types; *p;) {
        while (*p == ' ' || *p == ',' || *p == ';') p++;
        if (*p == '.') p++;
        const char *e = p;
        while (*e && *e != ' ' && *e != ',' && *e != ';') e++;
        if (e > p) {
            wchar_t w[64];
            int n = MultiByteToWideChar(CP_UTF8, 0, p, (int)(e - p), w, 63);
            w[n > 0 ? n : 0] = 0;
            if (n > 0 && !_wcsicmp(w, dot)) return 1;
        }
        p = e;
    }
    return 0;
}

void relay_norm(const wchar_t *in, wchar_t *out, size_t cap)
{
    size_t n = 0;
    for (const wchar_t *p = in; *p && *p != L'\t' && n + 1 < cap; p++) {
        if (*p == L'&') {
            if (p[1] != L'&') continue;
            p++;
        }
        out[n++] = *p;
    }
    out[n] = 0;
    for (;;) {
        while (n && (out[n - 1] == L' ' || out[n - 1] == L'\u00A0')) n--;
        if (n >= 3 && !wcsncmp(out + n - 3, L"...", 3)) n -= 3;
        else if (n && out[n - 1] == L'\u2026') n--;
        else break;
    }
    out[n] = 0;
    size_t lead = 0;
    while (out[lead] == L' ') lead++;
    if (lead) memmove(out, out + lead, (n - lead + 1) * sizeof *out);
    CharLowerW(out);
}

const wchar_t *relay_segment(const wchar_t *path, wchar_t *seg, size_t cap)
{
    const wchar_t *gt = wcsstr(path, L" > ");
    size_t n = gt ? (size_t)(gt - path) : wcslen(path);
    wchar_t part[512];
    if (n > 511) n = 511;
    wmemcpy(part, path, n);
    part[n] = 0;
    relay_norm(part, seg, cap);
    return gt ? gt + 3 : NULL;
}
