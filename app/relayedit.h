/* Drafts are validated without changing the registry; the host commits their
 * complete settings text only after its revision/file-content checks pass. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>
#define RELAY_DRAFT_MAGIC 0x524C5944u
#define RELAY_LINE_MAX 8192
#define RELAY_CATALOG_MAX 192
#define RELAY_CONTEXT_MAX 128
#define RELAY_CONTEXT_CHARS 32768

enum { RELAY_RUN = 1, RELAY_ITEM, RELAY_VERB, RELAY_SENDTO };
enum { RELAY_FILES = 1, RELAY_FOLDERS = 2, RELAY_BACKGROUND = 4 };
typedef struct relay_draft {
    uint32_t magic, request, revision, editing, kind, scopes, menu, palette, window, share_key, count_one,
        fallback;
    char name[33], title[320], program[2048], arguments[4096], action[512], handler[40], types[256],
        keys[192];
} relay_draft;
void relay_draft_init(relay_draft *d);
int relay_draft_validate(const relay_draft *d, char *why, size_t cap);
/* UTF-16 template: program quoted once, then arguments. */
int relay_draft_line(const relay_draft *d, wchar_t *line, size_t cap);
/* Registered relay family required; new identities cannot silently replace
 * existing ones. Does not mutate live registry/settings or start anything. */
int relay_draft_apply(const relay_draft *d, const char *text, size_t len, char **out, size_t *out_len,
                      char *why, size_t cap);

typedef struct relay_catalog_entry {
    uint32_t kind, scopes;
    char title[320], action[512], handler[40], types[256], source[512], command[RELAY_LINE_MAX];
} relay_catalog_entry;
typedef struct relay_catalog {
    unsigned count, omitted;
    relay_catalog_entry entries[RELAY_CATALOG_MAX];
} relay_catalog;
/* Pure identity insertion: bounded, deduplicated by action+handler+types+scope. */
int relay_catalog_add(relay_catalog *c, const relay_catalog_entry *e);
int relay_catalog_draft(const relay_catalog_entry *e, relay_draft *d);
/* Read-only registry + Send to discovery. Call on a worker. No extension is loaded. */
void relay_catalog_scan(relay_catalog *c, const wchar_t *file);
