/* Operations model and wire format. Owned by the explorer; a panel may
 * disappear or restart without losing jobs or results. No resources at idle. */
#pragma once
#include "fpe.h"
#include <windows.h>
#define OPS_MAX 64
#define OPS_DETAIL_CHARS 65536u
#define OPS_PENDING 6u

typedef struct ops_head {
    uint64_t token, job, bytes_done, bytes_total, started, ended;
    uint32_t state, op, policy, count, items_done, items_total, errors, skipped, last_error, cancelling;
    uint32_t details_count, omitted, chars[4]; /* source, destination, current, diagnostics (NUL included) */
} ops_head;
typedef struct ops_entry {
    ops_head h;
    wchar_t *source, *dest, *current, *details;
} ops_entry;
typedef struct ops_model {
    ops_entry entries[OPS_MAX];
    uint32_t n;
    uint64_t next_token;
} ops_model;

typedef struct ops_action { uint64_t token; uint32_t action, policy; } ops_action;
enum { OPS_START, OPS_CANCEL, OPS_CLEAR };

int ops_finished(uint32_t state);
ops_entry *ops_find(ops_model *m, uint64_t token);
ops_entry *ops_add(ops_model *m, fpe_op op, const wchar_t *source, uint32_t count, const wchar_t *dest);
void ops_update(ops_entry *e, const fpe_job_progress *p);
void ops_diagnostic(ops_entry *e, const fpe_job_event *event);
void ops_clear(ops_model *m); /* completed results only: pending/active jobs survive */
void ops_free_entry(ops_entry *e);
void ops_free(ops_model *m);
void *ops_encode(const ops_entry *e, uint32_t *size);
int ops_decode(const void *data, uint32_t size, ops_entry *out);
const wchar_t *ops_state(const ops_entry *e);
const wchar_t *ops_verb(uint32_t op);
