#include "textutil.h"
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
static int eq(const wchar_t *a, const wchar_t *b)
{
    while (*a && *b && towlower(*a) == towlower(*b))
        ++a, ++b;
    return !*a && !*b;
}
int text_file_language(const wchar_t *name)
{
    if (!name)
        return -1;
    const wchar_t *ext = wcsrchr(name, L'.');
    ext = ext ? ext + 1 : L"";
    static const struct {
        const wchar_t *ext;
        int lang;
    } types[] = {{L"txt", TEXT_PLAIN},   {L"text", TEXT_PLAIN},
                 {L"log", TEXT_PLAIN},   {L"csv", TEXT_PLAIN},
                 {L"tsv", TEXT_PLAIN},   {L"c", TEXT_C},
                 {L"h", TEXT_C},         {L"cc", TEXT_C},
                 {L"cpp", TEXT_C},       {L"cxx", TEXT_C},
                 {L"hpp", TEXT_C},       {L"cs", TEXT_C},
                 {L"java", TEXT_C},      {L"js", TEXT_C},
                 {L"jsx", TEXT_C},       {L"ts", TEXT_C},
                 {L"tsx", TEXT_C},       {L"go", TEXT_C},
                 {L"rs", TEXT_C},        {L"swift", TEXT_C},
                 {L"kt", TEXT_C},        {L"py", TEXT_PYTHON},
                 {L"pyw", TEXT_PYTHON},  {L"rb", TEXT_PYTHON},
                 {L"sh", TEXT_SCRIPT},   {L"bash", TEXT_SCRIPT},
                 {L"zsh", TEXT_SCRIPT},  {L"ps1", TEXT_SCRIPT},
                 {L"psm1", TEXT_SCRIPT}, {L"bat", TEXT_SCRIPT},
                 {L"cmd", TEXT_SCRIPT},  {L"json", TEXT_JSON},
                 {L"jsonc", TEXT_JSON},  {L"jsonl", TEXT_JSON},
                 {L"ini", TEXT_CONFIG},  {L"cfg", TEXT_CONFIG},
                 {L"conf", TEXT_CONFIG}, {L"config", TEXT_CONFIG},
                 {L"toml", TEXT_CONFIG}, {L"yaml", TEXT_CONFIG},
                 {L"yml", TEXT_CONFIG},  {L"env", TEXT_CONFIG},
                 {L"xml", TEXT_XML},     {L"html", TEXT_XML},
                 {L"htm", TEXT_XML},     {L"svg", TEXT_XML},
                 {L"xaml", TEXT_XML},    {L"css", TEXT_C},
                 {L"scss", TEXT_C},      {L"sql", TEXT_SQL},
                 {L"md", TEXT_MARKDOWN}, {L"markdown", TEXT_MARKDOWN},
                 {L"rst", TEXT_PLAIN}};
    for (size_t i = 0; i < sizeof types / sizeof *types; ++i)
        if (eq(ext, types[i].ext))
            return types[i].lang;
    if (eq(name, L"Makefile") || eq(name, L"Dockerfile") || eq(name, L".gitignore") || eq(name, L".bashrc") ||
        eq(name, L".zshrc") || eq(name, L".gitconfig"))
        return TEXT_SCRIPT;
    if (eq(name, L"README") || eq(name, L"LICENSE") || eq(name, L"LICENCE") || eq(name, L"COPYING") ||
        eq(name, L"AUTHORS") || eq(name, L"CHANGELOG"))
        return TEXT_PLAIN;
    return -1;
}
const wchar_t *text_language_name(uint32_t n)
{
    static const wchar_t *const names[] = {L"Text",          L"Code",   L"Python/Ruby", L"Script",  L"JSON",
                                           L"Configuration", L"Markup", L"SQL",         L"Markdown"};
    return n < sizeof names / sizeof *names ? names[n] : L"Text";
}
const wchar_t *text_encoding_name(uint32_t n)
{
    static const wchar_t *const names[] = {L"Auto",      L"UTF-8",     L"UTF-16 LE",   L"UTF-16 BE",
                                           L"UTF-32 LE", L"UTF-32 BE", L"Windows-1252"};
    return n < sizeof names / sizeof *names ? names[n] : L"Unknown";
}
static int utf8(const uint8_t *p, size_t n, uint32_t *cp, size_t *used)
{
    if (!n)
        return 0;
    unsigned c = p[0], len;
    uint32_t v, min;
    if (c < 128) {
        *cp = c;
        *used = 1;
        return 1;
    }
    if (c >= 0xC2 && c <= 0xDF) {
        len = 2;
        v = c & 31;
        min = 0x80;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3;
        v = c & 15;
        min = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
        v = c & 7;
        min = 0x10000;
    } else
        return -1;
    if (n < len) {
        for (size_t i = 1; i < n; ++i)
            if ((p[i] & 0xC0) != 0x80)
                return -1;
        return 0;
    }
    for (unsigned i = 1; i < len; ++i) {
        if ((p[i] & 0xC0) != 0x80)
            return -1;
        v = (v << 6) | (p[i] & 63);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF))
        return -1;
    *cp = v;
    *used = len;
    return 1;
}
static uint32_t word16(const uint8_t *p, int be)
{
    return be ? ((uint32_t)p[0] << 8) | p[1] : p[0] | ((uint32_t)p[1] << 8);
}
static uint32_t word32(const uint8_t *p, int be)
{
    return be ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]
              : p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
int text_decode(const uint8_t *p, size_t n, int prefix, uint32_t enc, text_data *out)
{
    memset(out, 0, sizeof *out);
    size_t at = 0;
    if (enc > TEXT_1252)
        return -2;
    if (n >= 4 && !memcmp(p, "\xFF\xFE\0\0", 4)) {
        if (!enc)
            enc = TEXT_UTF32LE;
        if (enc == TEXT_UTF32LE)
            at = 4;
    } else if (n >= 4 && !memcmp(p, "\0\0\xFE\xFF", 4)) {
        if (!enc)
            enc = TEXT_UTF32BE;
        if (enc == TEXT_UTF32BE)
            at = 4;
    } else if (n >= 3 && !memcmp(p, "\xEF\xBB\xBF", 3)) {
        if (!enc)
            enc = TEXT_UTF8;
        if (enc == TEXT_UTF8)
            at = 3;
    } else if (n >= 2 && p[0] == 255 && p[1] == 254) {
        if (!enc)
            enc = TEXT_UTF16LE;
        if (enc == TEXT_UTF16LE)
            at = 2;
    } else if (n >= 2 && p[0] == 254 && p[1] == 255) {
        if (!enc)
            enc = TEXT_UTF16BE;
        if (enc == TEXT_UTF16BE)
            at = 2;
    }
    if (!enc) {
        /* Conservative BOM-less UTF-16 detection: ASCII and newline patterns. */
        size_t sample = n < 4096 ? n : 4096, even = 0, odd = 0;
        for (size_t i = 0; i < sample; ++i)
            if (!p[i]) {
                if (i & 1)
                    ++odd;
                else
                    ++even;
            }
        if (sample >= 8 && odd * 3 > sample && even == 0)
            enc = TEXT_UTF16LE;
        else if (sample >= 8 && even * 3 > sample && odd == 0)
            enc = TEXT_UTF16BE;
        else {
            enc = TEXT_UTF8;
            size_t i = 0, u;
            uint32_t cp;
            while (i < n) {
                int ok = utf8(p + i, n - i, &cp, &u);
                if (ok == 0 && prefix)
                    break;
                if (ok != 1) {
                    enc = TEXT_1252;
                    out->flags |= TEXTP_GUESSED;
                    break;
                }
                i += u;
            }
        }
    }
    out->encoding = enc;
    if (prefix)
        out->flags |= TEXTP_PARTIAL;
    out->text = malloc(((size_t)TEXTP_MAX_CHARS + 1) * sizeof(wchar_t));
    if (!out->text)
        return -1;
    uint32_t count = 0, controls = 0;
    int cr = 0;
    static const uint16_t cp1252[32] = {0x20AC, 0x81,   0x201A, 0x192,  0x201E, 0x2026, 0x2020, 0x2021,
                                        0x2C6,  0x2030, 0x160,  0x2039, 0x152,  0x8D,   0x17D,  0x8F,
                                        0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                        0x2DC,  0x2122, 0x161,  0x203A, 0x153,  0x9D,   0x17E,  0x178};
    while (at < n && count < TEXTP_MAX_CHARS) {
        uint32_t cp = 0;
        size_t u = 1;
        int valid = 1;
        if (enc == TEXT_UTF8) {
            valid = utf8(p + at, n - at, &cp, &u);
            if (valid == 0 && prefix)
                break;
        } else if (enc == TEXT_UTF16LE || enc == TEXT_UTF16BE) {
            int be = enc == TEXT_UTF16BE;
            u = 2;
            if (n - at < 2) {
                if (prefix)
                    break;
                valid = -1;
                u = n - at;
            } else {
                cp = word16(p + at, be);
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (n - at < 4) {
                        if (prefix)
                            break;
                        valid = -1;
                    } else {
                        uint32_t lo = word16(p + at + 2, be);
                        if (lo < 0xDC00 || lo > 0xDFFF)
                            valid = -1;
                        else {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            u = 4;
                        }
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF)
                    valid = -1;
            }
        } else if (enc == TEXT_UTF32LE || enc == TEXT_UTF32BE) {
            u = 4;
            if (n - at < 4) {
                if (prefix)
                    break;
                valid = -1;
                u = n - at;
            } else {
                cp = word32(p + at, enc == TEXT_UTF32BE);
                if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                    valid = -1;
            }
        } else {
            cp = p[at];
            if (cp >= 128 && cp < 160)
                cp = cp1252[cp - 128];
        }
        if (valid != 1) {
            cp = 0xFFFD;
            out->flags |= TEXTP_REPLACED;
        }
        at += u;
        if (!cp) {
            text_data_free(out);
            return -2;
        }
        if (cp == '\n' && cr) {
            cr = 0;
            continue;
        }
        cr = cp == '\r';
        if (cr)
            cp = '\n';
        if (cp < 32 && cp != '\n' && cp != '\t') {
            ++controls;
            cp = 0x2400 + cp;
        }
        if (cp <= 0xFFFF)
            out->text[count++] = (wchar_t)cp;
        else {
            if (count + 2 > TEXTP_MAX_CHARS) {
                out->flags |= TEXTP_PARTIAL;
                break;
            }
            cp -= 0x10000;
            out->text[count++] = (wchar_t)(0xD800 + (cp >> 10));
            out->text[count++] = (wchar_t)(0xDC00 + (cp & 1023));
        }
    }
    if (at < n)
        out->flags |= TEXTP_PARTIAL;
    out->text[count] = 0;
    out->chars = count;
    if (controls > 8 && controls * 50 > count) {
        text_data_free(out);
        return -2;
    }
    out->style = calloc((size_t)count + 1, 1);
    if (!out->style) {
        text_data_free(out);
        return -1;
    }
    return 0;
}
void text_data_free(text_data *d)
{
    free(d->text);
    free(d->style);
    memset(d, 0, sizeof *d);
}
static int word(wchar_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
static void mark(text_data *d, uint32_t a, uint32_t b, uint8_t style)
{
    if (b > d->chars)
        b = d->chars;
    if (b > a)
        memset(d->style + a, style, b - a);
}
void text_highlight(text_data *d, uint32_t language)
{
    d->language = language;
    memset(d->style, 0, d->chars);
    if (language == TEXT_PLAIN)
        return;
    const wchar_t *s = d->text;
    uint32_t n = d->chars;
    static const wchar_t keywords[] =
        L"|if|else|elif|for|while|do|switch|case|break|continue|return|try|catch|finally|throw|throws|class|"
        L"struct|enum|interface|extends|implements|public|private|protected|static|const|let|var|function|"
        L"async|await|import|export|from|as|in|is|new|delete|void|int|char|float|double|bool|unsigned|long|"
        L"short|typedef|sizeof|null|true|false|True|False|None|def|lambda|with|yield|pass|raise|and|or|not|"
        L"match|fn|pub|use|impl|mut|self|Self|package|func|go|defer|select|chan|end|then|fi|done|echo|param|"
        L"BEGIN|END|";
    int line_start = 1;
    for (uint32_t i = 0; i < n;) {
        uint32_t a = i;
        wchar_t c = s[i];
        if (c == '\n') {
            line_start = 1;
            ++i;
            continue;
        }
        if (c == ' ' || c == '\t') {
            ++i;
            continue;
        }
        if ((language == TEXT_XML && i + 3 < n && !wcsncmp(s + i, L"<!--", 4)) ||
            (i + 1 < n && s[i] == '/' && s[i + 1] == '*' &&
             (language == TEXT_C || language == TEXT_JSON || language == TEXT_SQL))) {
            int xml = language == TEXT_XML;
            i += xml ? 4 : 2;
            while (i < n) {
                if (xml && i + 2 < n && !wcsncmp(s + i, L"-->", 3)) {
                    i += 3;
                    break;
                }
                if (!xml && i + 1 < n && s[i] == '*' && s[i + 1] == '/') {
                    i += 2;
                    break;
                }
                ++i;
            }
            mark(d, a, i, TEXT_COMMENT);
            line_start = 0;
            continue;
        }
        if ((i + 1 < n && c == '/' && s[i + 1] == '/' && (language == TEXT_C || language == TEXT_JSON)) ||
            (i + 1 < n && c == '-' && s[i + 1] == '-' && language == TEXT_SQL) ||
            (c == '#' && (language == TEXT_PYTHON || language == TEXT_SCRIPT || language == TEXT_CONFIG)) ||
            (c == ';' && language == TEXT_CONFIG)) {
            while (i < n && s[i] != '\n')
                ++i;
            mark(d, a, i, TEXT_COMMENT);
            line_start = 0;
            continue;
        }
        if (c == '\'' || c == '"' || (c == '`' && (language == TEXT_C || language == TEXT_SCRIPT))) {
            int triple = language == TEXT_PYTHON && i + 2 < n && s[i + 1] == c && s[i + 2] == c;
            i += triple ? 3 : 1;
            while (i < n) {
                if (s[i] == '\\' && i + 1 < n) {
                    i += 2;
                    continue;
                }
                if (s[i] == c) {
                    if (!triple) {
                        ++i;
                        break;
                    }
                    if (i + 2 < n && s[i + 1] == c && s[i + 2] == c) {
                        i += 3;
                        break;
                    }
                }
                if (!triple && s[i] == '\n' && language != TEXT_C)
                    break;
                ++i;
            }
            uint32_t next = i;
            while (next < n && (s[next] == ' ' || s[next] == '\t'))
                ++next;
            mark(d, a, i, language == TEXT_JSON && next < n && s[next] == ':' ? TEXT_LABEL : TEXT_STRING);
            line_start = 0;
            continue;
        }
        if (c >= '0' && c <= '9') {
            ++i;
            while (i < n && (word(s[i]) || s[i] == '.'))
                ++i;
            mark(d, a, i, TEXT_NUMBER);
            line_start = 0;
            continue;
        }
        if (language == TEXT_MARKDOWN && line_start && c == '#') {
            while (i < n && s[i] != '\n')
                ++i;
            mark(d, a, i, TEXT_LABEL);
            line_start = 0;
            continue;
        }
        if (language == TEXT_XML && c == '<') {
            ++i;
            while (i < n &&
                   (word(s[i]) || s[i] == '/' || s[i] == '!' || s[i] == '?' || s[i] == '-' || s[i] == ':'))
                ++i;
            mark(d, a, i, TEXT_KEYWORD);
            line_start = 0;
            continue;
        }
        if (word(c)) {
            while (i < n && word(s[i]))
                ++i;
            uint32_t len = i - a;
            wchar_t token[96];
            if (len < 90) {
                token[0] = '|';
                memcpy(token + 1, s + a, len * 2);
                token[len + 1] = '|';
                token[len + 2] = 0;
                if (wcsstr(keywords, token))
                    mark(d, a, i, TEXT_KEYWORD);
                else if (language == TEXT_SQL) {
                    static const wchar_t sql[] =
                        L"|select|from|where|join|left|right|inner|outer|on|and|or|insert|into|values|update|"
                        L"set|delete|create|table|index|drop|alter|group|by|order|limit|having|distinct|"
                        L"union|all|null|as|";
                    for (uint32_t j = 1; j <= len; ++j)
                        token[j] = towlower(token[j]);
                    if (wcsstr(sql, token))
                        mark(d, a, i, TEXT_KEYWORD);
                }
            }
            uint32_t next = i;
            while (next < n && (s[next] == ' ' || s[next] == '\t'))
                ++next;
            if (language == TEXT_CONFIG && next < n && (s[next] == '=' || s[next] == ':'))
                mark(d, a, i, TEXT_LABEL);
            line_start = 0;
            continue;
        }
        if (language == TEXT_C && line_start && c == '#') {
            while (i < n && s[i] != '\n')
                ++i;
            mark(d, a, i, TEXT_LABEL);
            line_start = 0;
            continue;
        }
        ++i;
        line_start = 0;
    }
}
uint32_t text_cell(const wchar_t *s, uint32_t at, uint32_t end, uint32_t column, uint32_t *units)
{
    *units = 1;
    uint32_t cp = s[at];
    if (cp >= 0xD800 && cp <= 0xDBFF && at + 1 < end && s[at + 1] >= 0xDC00 && s[at + 1] <= 0xDFFF) {
        *units = 2;
        cp = 0x10000 + ((cp - 0xD800) << 10) + (s[at + 1] - 0xDC00);
    }
    if (cp == '\t')
        return 4 - column % 4;
    if ((cp >= 0x300 && cp <= 0x36F) || (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0x1AB0 && cp <= 0x1AFF))
        return 0;
    return (cp >= 0x1100 &&
            ((cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7AF) ||
             (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE10 && cp <= 0xFE6F) ||
             (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0x1F300 && cp <= 0x1FAFF) || (cp >= 0x20000)))
               ? 2
               : 1;
}
void text_columns(const wchar_t *s, uint32_t n, uint32_t *columns)
{
    uint32_t col = 0;
    for (uint32_t i = 0; i < n;) {
        columns[i] = col;
        if (s[i] == '\n') {
            columns[++i] = 0;
            col = 0;
            continue;
        }
        uint32_t units, w = text_cell(s, i, n, col, &units);
        if (units == 2)
            columns[i + 1] = col;
        col += w;
        i += units;
        columns[i] = col;
    }
    columns[n] = col;
}
uint32_t text_layout(const wchar_t *s, uint32_t n, uint32_t cols, textp_row *rows, uint32_t cap,
                     uint32_t *lines, uint32_t *maxcols)
{
    uint32_t count = 0, start = 0, line = 1, col = 0, max = 0, physical_col = 0, break_at = 0, break_phys = 0;
    for (uint32_t i = 0; i < n;) {
        if (s[i] == '\n') {
            if (count >= cap)
                return 0;
            rows[count++] = (textp_row){start, i, line++};
            ++i;
            start = i;
            col = 0;
            break_at = 0;
            if (physical_col > max)
                max = physical_col;
            physical_col = 0;
            continue;
        }
        uint32_t units, w = text_cell(s, i, n, physical_col, &units);
        if (cols && col + w > cols && i > start) {
            if (count >= cap)
                return 0;
            uint32_t end = break_at > start ? break_at : i;
            rows[count++] = (textp_row){start, end, line};
            start = end;
            col = 0;
            if (break_at) {
                i = break_at;
                physical_col = break_phys;
            }
            break_at = 0;
            continue;
        }
        col += w;
        physical_col += w;
        i += units;
        if (s[i - units] == ' ' || s[i - units] == '\t') {
            break_at = i;
            break_phys = physical_col;
        }
    }
    if (count >= cap)
        return 0;
    rows[count++] = (textp_row){start, n, line};
    if (physical_col > max)
        max = physical_col;
    *lines = line;
    *maxcols = max;
    return count;
}
uint32_t text_matches(const wchar_t *s, uint32_t n, const wchar_t *q, uint32_t len, uint32_t *hits,
                      uint32_t cap, uint32_t *more)
{
    *more = 0;
    if (!len || len >= TEXTP_QUERY_MAX)
        return 0;
    uint32_t fail[TEXTP_QUERY_MAX] = {0}, j = 0, count = 0;
    for (uint32_t i = 1; i < len; ++i) {
        while (j && q[i] != q[j])
            j = fail[j - 1];
        if (q[i] == q[j])
            ++j;
        fail[i] = j;
    }
    j = 0;
    for (uint32_t i = 0; i < n; ++i) {
        while (j && s[i] != q[j])
            j = fail[j - 1];
        if (s[i] == q[j])
            ++j;
        if (j == len) {
            if (count == cap) {
                *more = 1;
                break;
            }
            hits[count++] = i + 1 - len;
            j = fail[j - 1];
        }
    }
    return count;
}
