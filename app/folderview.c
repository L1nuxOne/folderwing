#include "folderview.h"
#include "fpe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

static const char header[] = "# OpenPilot folder views v1";
static int valid(fv_value v)
{
    return v.mask && !(v.mask & ~FV_ALL) && v.mode <= 1 &&
           (v.size == 96 || v.size == 160 || v.size == 256) && v.sort <= 4 && v.descending <= 1;
}
/* Input paths already come from the engine. Only spelling aliases and trailing
 * separators are changed here, without resolving Linux . / .. or symlinks. */
static wchar_t *key(const wchar_t *path)
{
    if (!path) return NULL;
    size_t n = wcslen(path);
    if (!n || n >= FV_PATH_MAX) return NULL;
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, NULL, 0, NULL, NULL)) return NULL;
    for (size_t i = 0; i < n; ++i)
        if (path[i] < 32 || path[i] == 127) return NULL;
    wchar_t *p = malloc((n + 20) * sizeof *p);
    if (!p) return NULL;
    if (!_wcsnicmp(path, L"\\\\?\\UNC\\", 8)) {
        wcscpy(p, L"\\\\"); wcscat(p, path + 8);
    } else if (!_wcsnicmp(path, L"\\\\?\\", 4)) wcscpy(p, path + 4);
    else wcscpy(p, path);
    for (wchar_t *q = p; *q; ++q) if (*q == L'/') *q = L'\\';
    if (!_wcsnicmp(p, L"\\\\wsl$\\", 7)) {
        size_t rest = wcslen(p + 7) + 1;
        memmove(p + 16, p + 7, rest * sizeof *p);
        memcpy(p, L"\\\\wsl.localhost\\", 16 * sizeof *p);
    }
    n = wcslen(p);
    while (n > 3 && p[n - 1] == L'\\') p[--n] = 0;
    if (n >= FV_PATH_MAX || !((n >= 3 && p[1] == L':' && p[2] == L'\\') ||
                               (n > 2 && p[0] == L'\\' && p[1] == L'\\'))) {
        free(p); return NULL;
    }
    return p;
}
int fv_same_path(const wchar_t *a, const wchar_t *b)
{
    wchar_t *ka = key(a), *kb = key(b);
    int same = ka && kb && fpe_path_equal(ka, kb);
    free(ka); free(kb); return same;
}
static int find(const folderviews *s, const wchar_t *p)
{
    for (uint32_t i = 0; i < s->count; ++i)
        if (fpe_path_equal(s->entries[i].path, p)) return (int)i;
    return -1;
}
static int removed(const folderviews *s, const wchar_t *p)
{
    for (uint32_t i = 0; i < s->nremoved; ++i)
        if (fpe_path_equal(s->removed[i], p)) return 1;
    return 0;
}
void fv_free(folderviews *s)
{
    for (uint32_t i = 0; i < s->count; ++i) free(s->entries[i].path);
    for (uint32_t i = 0; i < s->nremoved; ++i) free(s->removed[i]);
    memset(s, 0, sizeof *s);
}
int fv_get(const folderviews *s, const wchar_t *path, fv_value *out)
{
    wchar_t *p = key(path);
    int i = p ? find(s, p) : -1;
    free(p);
    if (i < 0) return 0;
    if (out) *out = s->entries[i].value;
    return 1;
}
static fv_value merge(fv_value old, fv_value v)
{
    if (v.mask & FV_SORT) old.sort = v.sort, old.descending = v.descending;
    if (v.mask & FV_MODE) old.mode = v.mode;
    if (v.mask & FV_SIZE) old.size = v.size;
    old.mask |= v.mask;
    return old;
}
int fv_set(folderviews *s, const wchar_t *path, fv_value v)
{
    if (!valid(v)) return -1;
    wchar_t *p = key(path);
    if (!p) return -1;
    int i = find(s, p);
    fv_entry e = {p, {0, 0, 160, 0, 0}, 0};
    if (i >= 0) {
        free(p); e = s->entries[i];
        fv_value next = merge(e.value, v);
        if (!memcmp(&next, &e.value, sizeof next)) return 0;
        memmove(s->entries + 1, s->entries, (size_t)i * sizeof e);
    } else {
        if (s->count == FV_MAX) free(s->entries[--s->count].path);
        memmove(s->entries + 1, s->entries, (size_t)s->count * sizeof e);
        ++s->count;
    }
    e.value = merge(e.value, v);
    e.changed |= v.mask;
    s->entries[0] = e;
    return 1;
}
int fv_reset(folderviews *s, const wchar_t *path)
{
    wchar_t *p = key(path);
    if (!p) return -1;
    int i = find(s, p);
    if (!removed(s, p)) {
        if (s->nremoved == FV_MAX) { free(p); return -1; }
        s->removed[s->nremoved++] = p;
    } else free(p);
    if (i >= 0) {
        free(s->entries[i].path);
        memmove(s->entries + i, s->entries + i + 1, (s->count - (uint32_t)i - 1) * sizeof s->entries[0]);
        --s->count;
    }
    return 1;
}
int fv_parse(folderviews *s, const char *text, size_t len)
{
    if (len > FV_FILE_MAX || (len && (!text || memchr(text, 0, len)))) return -1;
    folderviews next = {0};
    char *buf = malloc(len + 1);
    if (!buf) return -1;
    if (len) memcpy(buf, text, len);
    buf[len] = 0;
    char *line = buf;
    if (len >= 3 && !memcmp(line, "\xEF\xBB\xBF", 3)) line += 3;
    char *nl = strchr(line, '\n');
    if (nl) *nl++ = 0;
    size_t first = strlen(line);
    if (first && line[first - 1] == '\r') line[--first] = 0;
    if (len && strcmp(line, header)) { free(buf); return -1; }
    for (line = nl; line && next.count < FV_MAX; line = nl) {
        nl = strchr(line, '\n'); if (nl) *nl++ = 0;
        size_t n = strlen(line); if (n && line[n - 1] == '\r') line[--n] = 0;
        if (!n || line[0] == '#') continue;
        fv_value v = {0}; uint32_t *fields[] = {&v.mask, &v.mode, &v.size, &v.sort, &v.descending};
        char *p = line; int ok = 1;
        for (int j = 0; j < 5; ++j) {
            char *end; unsigned long x = strtoul(p, &end, 10);
            if (end == p || *end != '\t' || *p < '0' || *p > '9' || x > 256) { ok = 0; break; }
            *fields[j] = (uint32_t)x; p = end + 1;
        }
        if (!ok || !valid(v)) continue;
        wchar_t path[FV_PATH_MAX];
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, -1, path, FV_PATH_MAX)) continue;
        wchar_t *k = key(path);
        if (!k) continue;
        if (find(&next, k) >= 0) { free(k); continue; }
        next.entries[next.count++] = (fv_entry){k, v, 0};
    }
    free(buf); fv_free(s); *s = next;
    return 0;
}
size_t fv_format(const folderviews *s, char *text, size_t cap)
{
    if (!cap) return 0;
    text[0] = 0;
    int h = snprintf(text, cap, "%s\r\n", header);
    if (h < 0 || (size_t)h >= cap) { text[0] = 0; return 0; }
    size_t len = (size_t)h;
    for (uint32_t i = 0; i < s->count; ++i) {
        fv_value v = s->entries[i].value;
        int n = snprintf(text + len, cap - len, "%u\t%u\t%u\t%u\t%u\t", v.mask, v.mode, v.size, v.sort, v.descending);
        if (n < 0 || (size_t)n >= cap - len) goto full;
        len += (size_t)n;
        n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s->entries[i].path, -1, text + len,
                                 (int)(cap - len), NULL, NULL);
        if (n <= 0 || (size_t)n + 2 > cap - len) goto full;
        len += (size_t)n - 1;
        memcpy(text + len, "\r\n", 3); len += 2;
    }
    return len;
full:
    text[0] = 0; return 0;
}
DWORD fv_load(folderviews *s, const wchar_t *file)
{
    HANDLE h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) { fv_free(s); return 0; }
        return e;
    }
    LARGE_INTEGER size; DWORD err = 0, got = 0; char *buf = NULL;
    if (!GetFileSizeEx(h, &size)) err = GetLastError();
    else if (size.QuadPart < 0 || size.QuadPart > FV_FILE_MAX) err = ERROR_FILE_TOO_LARGE;
    else if (!(buf = malloc((size_t)size.QuadPart + 1))) err = ERROR_NOT_ENOUGH_MEMORY;
    else if (!ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL)) err = GetLastError();
    else if (got != size.QuadPart) err = ERROR_READ_FAULT;
    else if (fv_parse(s, buf, got)) err = ERROR_INVALID_DATA;
    free(buf); CloseHandle(h); return err;
}
DWORD fv_save(folderviews *s, const wchar_t *file)
{
    int dirty = s->nremoved != 0;
    for (uint32_t i = 0; i < s->count; ++i) dirty |= s->entries[i].changed != 0;
    if (!dirty) return 0;
    wchar_t full[32768], name[80];
    DWORD need = GetFullPathNameW(file, 32768, full, NULL);
    if (!need || need >= 32768) return ERROR_FILENAME_EXCED_RANGE;
    uint64_t hash = 14695981039346656037ull;
    for (wchar_t *p = full; *p; ++p) { hash ^= (uint16_t)towlower(*p); hash *= 1099511628211ull; }
    _snwprintf(name, 80, L"Local\\OpenPilotFolderViews-%016llx", (unsigned long long)hash);
    HANDLE mutex = CreateMutexW(NULL, FALSE, name);
    if (!mutex) return GetLastError();
    DWORD wait = WaitForSingleObject(mutex, 2000);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) { CloseHandle(mutex); return ERROR_TIMEOUT; }
    folderviews disk = {0}, out = {0};
    DWORD err = fv_load(&disk, file);
    if (err) goto done;
    /* Edits first, then untouched disk entries. Resets are applied before a
     * subsequent partial edit, so reset+sort does not revive an old tile mode. */
    for (uint32_t i = 0; i < s->count; ++i) {
        const fv_entry *e = &s->entries[i];
        if (!e->changed) continue;
        int at = removed(s, e->path) ? -1 : find(&disk, e->path);
        fv_value base = at >= 0 ? disk.entries[at].value : (fv_value){0, 0, 160, 0, 0};
        fv_value edit = e->value; edit.mask = e->changed;
        fv_value v = merge(base, edit);
        wchar_t *p = _wcsdup(e->path);
        if (!p) { err = ERROR_NOT_ENOUGH_MEMORY; goto done; }
        out.entries[out.count++] = (fv_entry){p, v, 0};
    }
    for (uint32_t i = 0; i < disk.count && out.count < FV_MAX; ++i) {
        const fv_entry *e = &disk.entries[i];
        if (removed(s, e->path) || find(&out, e->path) >= 0) continue;
        wchar_t *p = _wcsdup(e->path);
        if (!p) { err = ERROR_NOT_ENOUGH_MEMORY; goto done; }
        out.entries[out.count++] = (fv_entry){p, e->value, 0};
    }
    size_t cap = 128 + (size_t)out.count * 80;
    for (uint32_t i = 0; i < out.count; ++i) cap += wcslen(out.entries[i].path) * 3;
    char *buf = malloc(cap);
    if (!buf) { err = ERROR_NOT_ENOUGH_MEMORY; goto done; }
    size_t len = fv_format(&out, buf, cap);
    if (!len) { free(buf); err = ERROR_INVALID_DATA; goto done; }
    size_t chars = wcslen(file) + 80;
    wchar_t *tmp = malloc(chars * sizeof *tmp);
    if (!tmp) { free(buf); err = ERROR_NOT_ENOUGH_MEMORY; goto done; }
    _snwprintf(tmp, chars, L"%ls.%lu.%llu.tmp", file, GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) err = GetLastError();
    else {
        DWORD written;
        if (!WriteFile(f, buf, (DWORD)len, &written, NULL)) err = GetLastError();
        else if (written != len) err = ERROR_WRITE_FAULT;
        else if (!FlushFileBuffers(f)) err = GetLastError();
        CloseHandle(f);
        if (!err && !MoveFileExW(tmp, file, MOVEFILE_REPLACE_EXISTING)) err = GetLastError();
        if (err) DeleteFileW(tmp);
    }
    free(tmp); free(buf);
    if (!err) { fv_free(s); *s = out; memset(&out, 0, sizeof out); }
done:
    fv_free(&out); fv_free(&disk); ReleaseMutex(mutex); CloseHandle(mutex);
    return err;
}
