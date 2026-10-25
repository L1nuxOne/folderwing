#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "relayedit.h"
#include "relaycmd.h"
#ifndef RELAY_FORM_ONLY
#include "fn.h"
#endif
static int fail(char *why, size_t cap, const char *message)
{
    if (cap)
        snprintf(why, cap, "%s", message);
    return -1;
}
void relay_draft_init(relay_draft *d)
{
    memset(d, 0, sizeof *d);
    d->magic = RELAY_DRAFT_MAGIC;
    d->kind = RELAY_RUN;
    d->scopes = RELAY_FILES | RELAY_FOLDERS;
    d->menu = d->palette = 1;
}
static int string_ok(const char *s, size_t cap)
{
    if (!memchr(s, 0, cap) || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0))
        return 0;
    for (; *s; ++s)
        if ((unsigned char)*s < 32 || (unsigned char)*s == 127)
            return 0;
    return 1;
}
static int placeholders(const char *s)
{
    int quote = 0;
    for (const char *p = s; *p; ++p) {
        if (*p == '"')
            quote = !quote;
        if (*p != '%')
            continue;
        if (p[1] == '%') {
            ++p;
            continue;
        }
        const char *e = p + 1;
        while ((*e >= 'A' && *e <= 'Z') || (*e >= 'a' && *e <= 'z') || (*e >= '0' && *e <= '9') ||
               (strchr("_()", *e) && *e))
            ++e;
        if (*e == '%' && e > p + 1 &&
            (p[1] == '_' || (p[1] >= 'A' && p[1] <= 'Z') || (p[1] >= 'a' && p[1] <= 'z'))) {
            p = e;
            continue;
        }
        if (p[1] && strchr("1LlVvDdWw*", p[1])) {
            if (p[1] == '*' && quote)
                return -2;
            ++p;
            continue;
        }
        return -1;
    }
    return quote ? -3 : 0;
}
int relay_draft_validate(const relay_draft *d, char *why, size_t cap)
{
    if (cap)
        why[0] = 0;
    if (d->magic != RELAY_DRAFT_MAGIC || d->kind < RELAY_RUN || d->kind > RELAY_SENDTO || d->editing > 1 ||
        !d->scopes || (d->scopes & ~7u) || d->menu > 1 || d->palette > 1 || d->share_key > 1 ||
        d->count_one > 1 || d->fallback > 1 || d->window > 4)
        return fail(why, cap, "Invalid command draft");
#define VALID(field)                                                                                         \
    if (!string_ok(d->field, sizeof d->field))                                                               \
    return fail(why, cap, "Values must be valid UTF-8 on a single line")
    VALID(name);
    VALID(title);
    VALID(program);
    VALID(arguments);
    VALID(action);
    VALID(handler);
    VALID(types);
    VALID(keys);
#undef VALID
    size_t n = strlen(d->name);
    if (!n || n > 32)
        return fail(why, cap, "Give the command an ID (1-32 letters, numbers, - or _)");
    for (const char *p = d->name; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' ||
              *p == '_'))
            return fail(why, cap, "The command ID uses letters, numbers, - or _");
    if (!d->title[0])
        return fail(why, cap, "Give the command a menu title");
    wchar_t title[80];
    if (!MultiByteToWideChar(CP_UTF8, 0, d->title, -1, title, 80))
        return fail(why, cap, "Menu title is too long (79 characters)");
    if (d->kind == RELAY_RUN) {
        if (!d->program[0])
            return fail(why, cap, "Choose a program or script");
        if (strchr(d->program, '"'))
            return fail(why, cap, "Enter the program without quotes; OpenPilot adds them");
        int p = placeholders(d->arguments);
        if (p == -2)
            return fail(why, cap, "Remove quotes around %*: each selected file is quoted separately");
        if (p == -3)
            return fail(why, cap, "Close the quotation mark in Arguments");
        if (p)
            return fail(why, cap, "Unknown placeholder: use %1, %*, %D, %NAME% or %% for a literal %");
        if (d->handler[0] || d->action[0])
            return fail(why, cap, "A custom command cannot also be an Explorer action");
    } else {
        if (!d->action[0])
            return fail(why, cap,
                        d->kind == RELAY_ITEM ? "Enter the exact Explorer menu text (including any submenu)"
                                              : "Choose an Explorer action");
        if (d->program[0] || d->arguments[0])
            return fail(why, cap, "An Explorer action cannot also run a custom program");
        if (d->handler[0]) {
            wchar_t guid[40];
            CLSID id;
            if (d->kind != RELAY_ITEM && d->kind != RELAY_VERB)
                return fail(why, cap, "Only Explorer items/verbs can have a handler");
            if (!MultiByteToWideChar(CP_UTF8, 0, d->handler, -1, guid, 40) ||
                FAILED(IIDFromString(guid, &id)))
                return fail(why, cap, "The Explorer handler ID is invalid");
        }
    }
    for (const char *p = d->types; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              strchr(".,; _-", *p)))
            return fail(why, cap, "File types are extensions separated by spaces or commas, e.g. txt md");
    return 0;
}
int relay_draft_line(const relay_draft *d, wchar_t *line, size_t cap)
{
    wchar_t program[2048], arguments[4096];
    if (d->kind != RELAY_RUN || !MultiByteToWideChar(CP_UTF8, 0, d->program, -1, program, 2048) ||
        !MultiByteToWideChar(CP_UTF8, 0, d->arguments, -1, arguments, 4096))
        return -1;
    int n = _snwprintf(line, cap, L"\"%ls\"%ls%ls", program, arguments[0] ? L" " : L"", arguments);
    if (n < 0 || (size_t)n >= cap) {
        if (cap)
            line[cap - 1] = 0;
        return -1;
    }
    return n;
}
#ifndef RELAY_FORM_ONLY
int relay_draft_apply(const relay_draft *d, const char *text, size_t len, char **out, size_t *out_len,
                      char *why, size_t cap)
{
    *out = NULL;
    *out_len = 0;
    if (relay_draft_validate(d, why, cap))
        return -1;
    char id[48];
    snprintf(id, sizeof id, "relay.%s", d->name);
    const fn_def *existing = fn_find(id);
    if (d->editing && !existing)
        return fail(why, cap, "This command was removed; create a new command instead");
    if (!d->editing && existing)
        return fail(why, cap, "That command ID already exists; use Edit command or choose another ID");
    if (d->keys[0] && strcmp(d->keys, "none") && !d->share_key) {
        char list[192];
        snprintf(list, sizeof list, "%s", d->keys);
        for (char *p = list; p;) {
            char *comma = strchr(p, ',');
            if (comma)
                *comma++ = 0;
            fn_binding key;
            if (!fn_parse_key(p, &key))
                for (uint32_t i = 0; i < fn_count(); ++i) {
                    const fn_def *f = fn_at(i);
                    fn_binding keys[FN_KEYS_MAX];
                    if (!f || !f->run || !strcmp(f->id, id) || !fn_active(f))
                        continue;
                    uint32_t nk = fn_bindings(f, keys, FN_KEYS_MAX);
                    for (uint32_t k = 0; k < nk && k < FN_KEYS_MAX; ++k)
                        if (keys[k].vk == key.vk && keys[k].mods == key.mods) {
                            snprintf(why, cap,
                                     "Shortcut also belongs to %s; change it or explicitly share it", f->id);
                            return -1;
                        }
                }
            p = comma;
        }
    }
    char command[RELAY_LINE_MAX];
    command[0] = 0;
    if (d->kind == RELAY_RUN) {
        wchar_t wide[RELAY_LINE_MAX];
        if (relay_draft_line(d, wide, RELAY_LINE_MAX) < 0 ||
            !WideCharToMultiByte(CP_UTF8, 0, wide, -1, command, sizeof command, NULL, NULL))
            return fail(why, cap, "The command line is too long");
    }
    static const char *window[] = {"auto", "panel", "normal", "minimized", "hidden"};
    char scopes[40];
    snprintf(scopes, sizeof scopes, "%s%s%s", (d->scopes & RELAY_FILES) ? "files " : "",
             (d->scopes & RELAY_FOLDERS) ? "folders " : "",
             (d->scopes & RELAY_BACKGROUND) ? "background" : "");
    struct {
        const char *sec, *key, *value;
    } edits[] = {
        {id, "title", d->title},
        {id, "run", d->kind == RELAY_RUN ? command : NULL},
        {id, "item", d->kind == RELAY_ITEM ? d->action : NULL},
        {id, "verb", d->kind == RELAY_VERB ? d->action : NULL},
        {id, "sendto", d->kind == RELAY_SENDTO ? d->action : NULL},
        {id, "new", NULL},
        {id, "template", NULL},
        {id, "handler", d->handler[0] ? d->handler : NULL},
        {id, "fallback", d->handler[0] ? (d->fallback ? "on" : "off") : NULL},
        {id, "on", scopes},
        {id, "types", d->types},
        {id, "count", d->count_one ? "one" : "any"},
        {id, "window", d->kind == RELAY_RUN ? window[d->window] : NULL},
        {"keys", id, d->keys[0] ? d->keys : "none"},
        {"menu", id, d->menu ? "on" : "off"},
        {"palette", id, d->palette ? "on" : "off"},
    };
    char *current = malloc(len + 1);
    if (!current)
        return -2;
    if (len)
        memcpy(current, text, len);
    current[len] = 0;
    for (unsigned i = 0; i < sizeof edits / sizeof *edits; ++i) {
        char *next = NULL;
        size_t size = 0;
        int rc = fn_settings_family_edit(current, len, id, edits[i].sec, edits[i].key, edits[i].value, &next,
                                         &size, why, cap);
        free(current);
        if (rc)
            return rc;
        current = next;
        len = size;
    }
    *out = current;
    *out_len = len;
    return 0;
}
#endif
int relay_catalog_add(relay_catalog *c, const relay_catalog_entry *e)
{
    if (e->kind < RELAY_RUN || e->kind > RELAY_SENDTO || !string_ok(e->title, sizeof e->title) ||
        !string_ok(e->action, sizeof e->action) || !string_ok(e->handler, sizeof e->handler) ||
        !string_ok(e->types, sizeof e->types) || !string_ok(e->source, sizeof e->source) ||
        !string_ok(e->command, sizeof e->command) || !e->title[0] || (e->kind == RELAY_RUN && !e->command[0]))
        return -1;
    for (unsigned i = 0; i < c->count; ++i) {
        relay_catalog_entry *a = &c->entries[i];
        if (a->kind == e->kind && !strcmp(a->action, e->action) && !strcmp(a->handler, e->handler) &&
            !strcmp(a->types, e->types) && !strcmp(a->command, e->command)) {
            a->scopes |= e->scopes;
            return 0;
        }
    }
    if (c->count == RELAY_CATALOG_MAX) {
        ++c->omitted;
        return -1;
    }
    c->entries[c->count++] = *e;
    return 1;
}
int relay_catalog_draft(const relay_catalog_entry *e, relay_draft *d)
{
    relay_draft_init(d);
    int result = 0;
    d->kind = e->kind;
    d->scopes = e->scopes;
    snprintf(d->title, sizeof d->title, "%s", e->title);
    snprintf(d->action, sizeof d->action, "%s", e->action);
    snprintf(d->handler, sizeof d->handler, "%s", e->handler);
    snprintf(d->types, sizeof d->types, "%s", e->types);
    if (e->kind == RELAY_RUN) {
        wchar_t command[RELAY_LINE_MAX], program[2048];
        const wchar_t *arguments;
        if (MultiByteToWideChar(CP_UTF8, 0, e->command, -1, command, RELAY_LINE_MAX)) {
            relay_split(command, program, 2048, &arguments);
            if (wcslen(program) > 511 || wcslen(arguments) > 1023)
                result = -1;
            if (!WideCharToMultiByte(CP_UTF8, 0, program, -1, d->program, sizeof d->program, NULL, NULL))
                d->program[0] = 0, result = -1;
            if (!WideCharToMultiByte(CP_UTF8, 0, arguments, -1, d->arguments, sizeof d->arguments, NULL,
                                     NULL))
                d->arguments[0] = 0, result = -1;
        }
        if (!d->program[0])
            result = -1;
        d->window = 2;
    }
    unsigned k = 0;
    for (const char *p = e->title; *p && k < 32; ++p) {
        unsigned char ch = (unsigned char)*p;
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))
            d->name[k++] = (char)ch;
        else if (ch >= 'A' && ch <= 'Z')
            d->name[k++] = (char)(ch + 32);
        else if (k && d->name[k - 1] != '_')
            d->name[k++] = '_';
    }
    while (k && d->name[k - 1] == '_')
        --k;
    d->name[k] = 0;
    if (!k)
        snprintf(d->name, sizeof d->name, "explorer_action");
    return result;
}
