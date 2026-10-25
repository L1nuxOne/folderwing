/* Exact path transport. Windows UTF-16 code units (including raw-byte low
 * surrogates used by WSL) are data, not Unicode text to normalize. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
#include <string.h>

#define PATHWIRE_PREFIX "u16v1:"
#define PATHWIRE_PREFIX_LEN 6

static inline size_t pathwire_encode(const wchar_t *path, char *out, size_t cap)
{
    static const char hex[] = "0123456789abcdef";
    if (!path || !out) return 0;
    size_t n = wcslen(path);
    if (!n || n > (SIZE_MAX - PATHWIRE_PREFIX_LEN - 1) / 4 ||
        cap < PATHWIRE_PREFIX_LEN + n * 4 + 1) return 0;
    memcpy(out, PATHWIRE_PREFIX, PATHWIRE_PREFIX_LEN);
    for (size_t i = 0; i < n; i++) {
        unsigned v = (unsigned)(uint16_t)path[i];
        for (int j = 0; j < 4; j++) out[PATHWIRE_PREFIX_LEN + i * 4 + j] = hex[(v >> (12 - j * 4)) & 15];
    }
    size_t len = PATHWIRE_PREFIX_LEN + n * 4;
    out[len] = 0;
    return len;
}

static inline int pathwire_decode(const char *data, size_t len, wchar_t *out, size_t cap)
{
    if (!data || !out || len < PATHWIRE_PREFIX_LEN + 4 ||
        memcmp(data, PATHWIRE_PREFIX, PATHWIRE_PREFIX_LEN) ||
        (len - PATHWIRE_PREFIX_LEN) % 4 ||
        (len - PATHWIRE_PREFIX_LEN) / 4 >= cap) return 0;
    size_t n = (len - PATHWIRE_PREFIX_LEN) / 4;
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        for (int j = 0; j < 4; j++) {
            unsigned char c = (unsigned char)data[PATHWIRE_PREFIX_LEN + i * 4 + j];
            int d = c >= '0' && c <= '9' ? c - '0' :
                    c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                    c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return 0;
            v = (v << 4) | (unsigned)d;
        }
        if (!v) return 0;
        out[i] = (wchar_t)v;
    }
    out[n] = 0;
    return 1;
}
