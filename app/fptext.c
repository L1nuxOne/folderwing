/* Text decoding, layout, search and clipboard preparation in a restricted
 * helper. Each reply publishes an immutable snapshot in its owned bank. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "textutil.h"
typedef struct msg {
    struct msg *next;
    textp_head head;
    uint8_t data[];
} msg;
static struct {
    HANDLE in, out, wake;
    CRITICAL_SECTION lock;
    msg *first, *last;
    unsigned count;
    volatile LONG doc, find_gen, eof;
    uint8_t *banks;
} Q;
static struct {
    text_data data;
    wchar_t *folded;
    uint32_t doc;
    uint64_t bytes, read_bytes;
} D;
static int read_all(void *p, DWORD n)
{
    while (n) {
        DWORD g;
        if (!ReadFile(Q.in, p, n, &g, NULL) || !g)
            return -1;
        p = (uint8_t *)p + g;
        n -= g;
    }
    return 0;
}
static int write_all(const void *p, DWORD n)
{
    while (n) {
        DWORD g;
        if (!WriteFile(Q.out, p, n, &g, NULL) || !g)
            return -1;
        p = (const uint8_t *)p + g;
        n -= g;
    }
    return 0;
}
static void reply(uint32_t type, uint32_t doc, uint32_t ticket, const void *p, uint32_t n)
{
    textp_head h = {type, n, doc, ticket};
    if (write_all(&h, sizeof h) || write_all(p, n))
        InterlockedExchange(&Q.eof, 1);
}
static void close_text(void)
{
    text_data_free(&D.data);
    free(D.folded);
    memset(&D, 0, sizeof D);
}
static DWORD WINAPI reader(void *arg)
{
    for (;;) {
        textp_head h;
        if (read_all(&h, sizeof h) || h.size > sizeof(textp_open) + TEXTP_PATH_MAX * 2)
            break;
        int valid = (h.type == TEXTP_OPEN && h.size >= sizeof(textp_open) + 2 && !(h.size % 2)) ||
                    (h.type == TEXTP_LAYOUT && h.size == sizeof(textp_layout)) ||
                    (h.type == TEXTP_FIND && h.size >= sizeof(textp_find) + 2 &&
                     h.size <= sizeof(textp_find) + TEXTP_QUERY_MAX * 2 && !(h.size % 2)) ||
                    (h.type == TEXTP_COPY && h.size == sizeof(textp_copy)) ||
                    ((h.type == TEXTP_CLOSE || h.type == TEXTP_QUIT) && !h.size);
        if (!valid)
            break;
        msg *m = malloc(sizeof *m + h.size);
        if (!m)
            break;
        m->next = NULL;
        m->head = h;
        if (read_all(m->data, h.size)) {
            free(m);
            break;
        }
        if ((h.type == TEXTP_OPEN || h.type == TEXTP_FIND) && *(uint16_t *)(m->data + h.size - 2)) {
            free(m);
            break;
        }
        if (h.type == TEXTP_OPEN || h.type == TEXTP_CLOSE)
            InterlockedExchange(&Q.doc, (LONG)h.doc);
        if (h.type == TEXTP_FIND) {
            textp_find f;
            memcpy(&f, m->data, sizeof f);
            if (f.chars >= TEXTP_QUERY_MAX || h.size != sizeof f + (f.chars + 1) * 2) {
                free(m);
                break;
            }
            InterlockedExchange(&Q.find_gen, (LONG)f.gen);
        }
        EnterCriticalSection(&Q.lock);
        /* Requests owning banks must always get an acknowledgement. Search
         * has no bank ownership and can be replaced before it starts. */
        if (h.type == TEXTP_FIND) {
            msg **at = &Q.first;
            Q.last = NULL;
            while (*at) {
                msg *k = *at;
                if (k->head.type == TEXTP_FIND) {
                    *at = k->next;
                    free(k);
                    --Q.count;
                } else {
                    Q.last = k;
                    at = &k->next;
                }
            }
        }
        if (Q.count >= 8) {
            LeaveCriticalSection(&Q.lock);
            free(m);
            break;
        }
        if (Q.last)
            Q.last->next = m;
        else
            Q.first = m;
        Q.last = m;
        ++Q.count;
        LeaveCriticalSection(&Q.lock);
        SetEvent(Q.wake);
    }
    InterlockedExchange(&Q.eof, 1);
    SetEvent(Q.wake);
    return 0;
}
static msg *pop(void)
{
    EnterCriticalSection(&Q.lock);
    msg *m = Q.first;
    if (m) {
        Q.first = m->next;
        if (!Q.first)
            Q.last = NULL;
        --Q.count;
    }
    LeaveCriticalSection(&Q.lock);
    return m;
}
static HRESULT open_text(const wchar_t *path, textp_open *r, uint32_t doc)
{
    close_text();
    D.doc = doc;
    if (r->limit_mb < 1 || r->limit_mb > 8 || r->encoding > TEXT_1252 || r->language > TEXT_MARKDOWN)
        return E_INVALIDARG;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32(GetLastError());
    LARGE_INTEGER size;
    HRESULT hr = S_OK;
    uint8_t *bytes = NULL;
    if (!GetFileSizeEx(f, &size)) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto out;
    }
    D.bytes = size.QuadPart;
    uint32_t limit = r->limit_mb * 1024 * 1024, n = D.bytes > limit ? limit : (uint32_t)D.bytes;
    bytes = malloc(n ? n : 1);
    if (!bytes) {
        hr = E_OUTOFMEMORY;
        goto out;
    }
    DWORD off = 0;
    while (off < n) {
        if ((uint32_t)Q.doc != doc || Q.eof) {
            hr = E_ABORT;
            goto out;
        }
        DWORD got, want = n - off > 65536 ? 65536 : n - off;
        if (!ReadFile(f, bytes + off, want, &got, NULL) || !got) {
            hr = HRESULT_FROM_WIN32(ERROR_READ_FAULT);
            goto out;
        }
        off += got;
    }
    D.read_bytes = n;
    int decoded = text_decode(bytes, n, D.bytes > n, r->encoding, &D.data);
    if (decoded) {
        hr = decoded == -2 ? HRESULT_FROM_WIN32(ERROR_INVALID_DATA) : E_OUTOFMEMORY;
        goto out;
    }
    if ((uint32_t)Q.doc != doc) {
        hr = E_ABORT;
        goto out;
    }
    text_highlight(&D.data, r->language);
    D.folded = malloc(((size_t)D.data.chars + 1) * 2);
    if (!D.folded) {
        hr = E_OUTOFMEMORY;
        goto out;
    }
    memcpy(D.folded, D.data.text, ((size_t)D.data.chars + 1) * 2);
    CharLowerBuffW(D.folded, D.data.chars);
out:
    free(bytes);
    CloseHandle(f);
    return hr;
}
static textp_info snapshot(uint32_t bank, uint32_t cols, HRESULT err)
{
    textp_info info = {.err = (int32_t)err, .bank = bank, .bytes = D.bytes, .read_bytes = D.read_bytes};
    if (bank >= TEXTP_BANKS) {
        info.err = E_INVALIDARG;
        return info;
    }
    if (FAILED(err))
        return info;
    if (cols && cols < 4) {
        info.err = E_INVALIDARG;
        return info;
    }
    if (!D.data.text) {
        info.err = E_ABORT;
        return info;
    }
    info.chars = D.data.chars;
    info.encoding = D.data.encoding;
    info.flags = D.data.flags;
    info.language = D.data.language;
    info.columns = cols;
    uint8_t *p = Q.banks + (size_t)bank * TEXTP_BANK_BYTES;
    size_t start = textp_rows_offset(info.chars);
    uint32_t cap = (uint32_t)((TEXTP_BANK_BYTES - start) / sizeof(textp_row));
    info.rows = text_layout(D.data.text, info.chars, cols, (textp_row *)(p + start), cap, &info.lines,
                            &info.max_columns);
    if (!info.rows) {
        info.err = E_OUTOFMEMORY;
        return info;
    }
    text_columns(D.data.text, info.chars, (uint32_t *)(p + textp_columns_offset(info.chars)));
    memcpy(p, D.data.text, ((size_t)info.chars + 1) * 2);
    memcpy(p + textp_style_offset(info.chars), D.data.style, info.chars);
    return info;
}
static void find_text(msg *m)
{
    textp_find req;
    memcpy(&req, m->data, sizeof req);
    textp_found header = {.gen = req.gen, .chars = req.chars};
    uint8_t *payload = malloc(sizeof header + TEXTP_MATCH_MAX * 4);
    if (!payload)
        return;
    wchar_t query[TEXTP_QUERY_MAX];
    memcpy(query, m->data + sizeof req, (req.chars + 1) * 2);
    CharLowerBuffW(query, req.chars);
    if (m->head.doc != D.doc || (uint32_t)Q.doc != D.doc || !D.folded)
        header.err = E_ABORT;
    else
        header.count = text_matches(D.folded, D.data.chars, query, req.chars,
                                    (uint32_t *)(payload + sizeof header), TEXTP_MATCH_MAX, &header.more);
    if (req.gen == (uint32_t)Q.find_gen) {
        memcpy(payload, &header, sizeof header);
        reply(TEXTP_FOUND, m->head.doc, m->head.ticket, payload, sizeof header + header.count * 4);
    }
    free(payload);
}
static void copy_text(msg *m)
{
    textp_copy r;
    memcpy(&r, m->data, sizeof r);
    textp_copied result = {.bank = r.bank};
    if (m->head.doc != D.doc || (uint32_t)Q.doc != D.doc || !D.data.text)
        result.err = E_ABORT;
    else if (r.bank >= TEXTP_BANKS || r.start > r.end || r.end > D.data.chars)
        result.err = E_INVALIDARG;
    else {
        /* Never split a UTF-16 surrogate pair at selection boundaries. */
        if (r.start && D.data.text[r.start] >= 0xDC00 && D.data.text[r.start] <= 0xDFFF)
            --r.start;
        if (r.end < D.data.chars && D.data.text[r.end] >= 0xDC00 && D.data.text[r.end] <= 0xDFFF)
            ++r.end;
        wchar_t *dst = (wchar_t *)(Q.banks + (size_t)r.bank * TEXTP_BANK_BYTES);
        uint32_t n = 0;
        for (uint32_t i = r.start; i < r.end; ++i) {
            if (D.data.text[i] == '\n')
                dst[n++] = '\r';
            dst[n++] = D.data.text[i];
        }
        dst[n] = 0;
        result.bytes = (n + 1) * 2;
    }
    reply(TEXTP_COPIED, m->head.doc, m->head.ticket, &result, sizeof result);
}
int wmain(int argc, wchar_t **argv)
{
    if (argc != 2)
        return 2;
    Q.banks = MapViewOfFile((HANDLE)(uintptr_t)_wcstoui64(argv[1], NULL, 10), FILE_MAP_WRITE, 0, 0,
                            (size_t)TEXTP_BANKS * TEXTP_BANK_BYTES);
    if (!Q.banks)
        return 3;
    Q.in = GetStdHandle(STD_INPUT_HANDLE);
    Q.out = GetStdHandle(STD_OUTPUT_HANDLE);
    InitializeCriticalSection(&Q.lock);
    Q.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE thread = CreateThread(NULL, 0, reader, NULL, 0, NULL);
    if (!thread)
        return 4;
    while (!Q.eof) {
        msg *m = pop();
        if (!m) {
            WaitForSingleObject(Q.wake, INFINITE);
            continue;
        }
        int quit = m->head.type == TEXTP_QUIT;
        if (m->head.type == TEXTP_OPEN) {
            textp_open r;
            memcpy(&r, m->data, sizeof r);
            HRESULT err = m->head.doc == (uint32_t)Q.doc
                              ? open_text((wchar_t *)(m->data + sizeof r), &r, m->head.doc)
                              : E_ABORT;
            textp_info info = snapshot(r.bank, r.columns, err);
            if ((HRESULT)info.err == HRESULT_FROM_WIN32(ERROR_INVALID_DATA))
                info.flags |= TEXTP_BINARY;
            reply(TEXTP_READY, m->head.doc, m->head.ticket, &info, sizeof info);
        } else if (m->head.type == TEXTP_LAYOUT) {
            textp_layout r;
            memcpy(&r, m->data, sizeof r);
            textp_info info = snapshot(
                r.bank, r.columns, m->head.doc == D.doc && m->head.doc == (uint32_t)Q.doc ? S_OK : E_ABORT);
            reply(TEXTP_READY, m->head.doc, m->head.ticket, &info, sizeof info);
        } else if (m->head.type == TEXTP_FIND)
            find_text(m);
        else if (m->head.type == TEXTP_COPY)
            copy_text(m);
        else if (m->head.type == TEXTP_CLOSE)
            close_text();
        free(m);
        if (quit)
            break;
    }
    close_text();
    return 0;
}
