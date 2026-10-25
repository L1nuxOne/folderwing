#include "operations.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdio.h>

static wchar_t *dup(const wchar_t *s)
{
    if (!s) s = L"";
    size_t n = (wcslen(s) + 1) * sizeof *s;
    wchar_t *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
int ops_finished(uint32_t s) { return s == FPE_JOB_DONE || s == FPE_JOB_CANCELLED || s == FPE_JOB_FAILED; }
void ops_free_entry(ops_entry *e)
{
    free(e->source); free(e->dest); free(e->current); free(e->details);
    memset(e, 0, sizeof *e);
}
void ops_free(ops_model *m)
{
    for (uint32_t i = 0; i < m->n; i++) ops_free_entry(m->entries + i);
    memset(m, 0, sizeof *m);
}
ops_entry *ops_find(ops_model *m, uint64_t token)
{
    for (uint32_t i = 0; i < m->n; i++) if (m->entries[i].h.token == token) return m->entries + i;
    return NULL;
}
static void remove_at(ops_model *m, uint32_t i)
{
    ops_free_entry(m->entries + i);
    memmove(m->entries + i, m->entries + i + 1, (--m->n - i) * sizeof *m->entries);
    memset(m->entries + m->n, 0, sizeof *m->entries);
}
void ops_clear(ops_model *m)
{
    for (uint32_t i = 0; i < m->n;) {
        if (ops_finished(m->entries[i].h.state)) remove_at(m, i);
        else i++;
    }
}
ops_entry *ops_add(ops_model *m, fpe_op op, const wchar_t *source, uint32_t count, const wchar_t *dest)
{
    if (m->n == OPS_MAX) {
        uint32_t i = 0;
        while (i < m->n && !ops_finished(m->entries[i].h.state)) i++;
        if (i == m->n) return NULL; /* never evict work */
        remove_at(m, i);
    }
    ops_entry e = { 0 };
    e.source = dup(source); e.dest = dup(dest); e.current = dup(L""); e.details = dup(L"");
    if (!e.source || !e.dest || !e.current || !e.details) { ops_free_entry(&e); return NULL; }
    e.h.token = ++m->next_token;
    e.h.op = op; e.h.count = count; e.h.state = OPS_PENDING;
    m->entries[m->n++] = e;
    return m->entries + m->n - 1;
}
void ops_update(ops_entry *e, const fpe_job_progress *p)
{
    if (!e || ops_finished(e->h.state)) return;
    e->h.job = p->job; e->h.state = p->state;
    e->h.bytes_done = p->bytes_done; e->h.bytes_total = p->bytes_total;
    e->h.items_done = p->items_done; e->h.items_total = p->items_total;
    e->h.errors = p->errors; e->h.skipped = p->skipped; e->h.last_error = p->last_error;
    if (!e->h.started) e->h.started = GetTickCount64();
    if (p->current) {
        wchar_t *s = dup(p->current);
        if (s) { free(e->current); e->current = s; }
    }
    if (ops_finished(p->state)) e->h.ended = GetTickCount64();
}
void ops_diagnostic(ops_entry *e, const fpe_job_event *v)
{
    if (!e || ops_finished(e->h.state)) return;
    wchar_t why[512] = L"";
    if (v->kind == FPE_JOB_SKIPPED && v->error == ERROR_NOT_SUPPORTED)
        wcscpy(why, L"Directory link was left untouched");
    else if (v->kind == FPE_JOB_SKIPPED && v->error == ERROR_FILE_EXISTS)
        wcscpy(why, L"Destination already exists");
    else if (!FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                            NULL, v->error, 0, why, 512, NULL))
        _snwprintf(why, 512, L"Windows error %u", v->error);
    size_t wn = wcslen(why);
    while (wn && (why[wn-1] == L'\r' || why[wn-1] == L'\n' || why[wn-1] == L' ')) why[--wn] = 0;
    const wchar_t *path = v->path ? v->path : L"(operation)";
    size_t old = wcslen(e->details), cap = old + wcslen(path) + wn + 80;
    if (e->h.details_count >= 64 || cap > OPS_DETAIL_CHARS) { e->h.omitted++; return; }
    wchar_t *s = realloc(e->details, cap * sizeof *s);
    if (!s) { e->h.omitted++; return; }
    e->details = s;
    _snwprintf(s + old, cap - old, L"%ls: %ls\r\n%ls (code %u)\r\n\r\n",
               v->kind == FPE_JOB_SKIPPED ? L"Skipped" : L"Failed", path, why, v->error);
    e->h.details_count++;
}
void *ops_encode(const ops_entry *e, uint32_t *size)
{
    ops_head h = e->h;
    const wchar_t *s[] = { e->source, e->dest, e->current, e->details };
    size_t n = sizeof h;
    for (int i = 0; i < 4; i++) { h.chars[i] = (uint32_t)wcslen(s[i] ? s[i] : L"") + 1; n += h.chars[i] * sizeof(wchar_t); }
    if (n > 4u * 1024 * 1024) return NULL;
    uint8_t *buf = malloc(n), *p;
    if (!buf) return NULL;
    memcpy(buf, &h, sizeof h); p = buf + sizeof h;
    for (int i = 0; i < 4; i++) { size_t z = h.chars[i] * sizeof(wchar_t); memcpy(p, s[i] ? s[i] : L"", z); p += z; }
    *size = (uint32_t)n;
    return buf;
}
int ops_decode(const void *data, uint32_t size, ops_entry *out)
{
    if (size < sizeof(ops_head)) return 0;
    ops_entry e = { 0 };
    memcpy(&e.h, data, sizeof e.h);
    if (!e.h.token || e.h.op > FPE_OP_RECYCLE || e.h.policy > FPE_CONFLICT_FAIL ||
        e.h.state > OPS_PENDING) return 0;
    const uint8_t *p = (const uint8_t *)data + sizeof e.h;
    size_t left = size - sizeof e.h;
    wchar_t **s[] = { &e.source, &e.dest, &e.current, &e.details };
    for (int i = 0; i < 4; i++) {
        size_t z = (size_t)e.h.chars[i] * sizeof(wchar_t);
        if (!z || z > left || e.h.chars[i] > (i == 3 ? OPS_DETAIL_CHARS : 32769u)) goto bad;
        *s[i] = malloc(z);
        if (!*s[i]) goto bad;
        memcpy(*s[i], p, z);
        if ((*s[i])[e.h.chars[i]-1] || wcslen(*s[i]) + 1 != e.h.chars[i]) goto bad;
        p += z; left -= z;
    }
    if (left) goto bad;
    *out = e; return 1;
bad:
    ops_free_entry(&e); return 0;
}
const wchar_t *ops_verb(uint32_t op)
{
    static const wchar_t *const v[] = { L"Copy", L"Move", L"Delete", L"Recycle" };
    return op <= FPE_OP_RECYCLE ? v[op] : L"Operation";
}
const wchar_t *ops_state(const ops_entry *e)
{
    if (e->h.cancelling && !ops_finished(e->h.state)) return e->h.op == FPE_OP_RECYCLE ? L"Stopping after current item" : L"Cancelling";
    switch (e->h.state) {
    case OPS_PENDING: return L"Ready to start";
    case FPE_JOB_QUEUED: return L"Queued";
    case FPE_JOB_SCANNING: return L"Counting items";
    case FPE_JOB_RUNNING: return L"Running";
    case FPE_JOB_CANCELLED: return L"Cancelled";
    case FPE_JOB_FAILED: return L"Could not start";
    default: return e->h.errors ? L"Finished with errors" : e->h.skipped ? L"Finished with skips" : L"Completed";
    }
}
