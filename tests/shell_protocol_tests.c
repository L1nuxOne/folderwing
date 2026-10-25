/* Exact framing and fail-closed allocation tests for the shell relay wire. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail_realloc;
static void *controlled_realloc(void *p, size_t n)
{
    if (fail_realloc) { fail_realloc = 0; return NULL; }
    return realloc(p, n);
}
#define SHELLWIRE_REALLOC controlled_realloc
#include "../app/shellwire.h"
static int shell_parse_calls;
static HRESULT WINAPI controlled_parse(PCWSTR name, IBindCtx *bind, PIDLIST_ABSOLUTE *id,
                                       SFGAOF attrs, SFGAOF *got)
{
    (void)name; (void)bind; (void)id; (void)attrs; (void)got;
    shell_parse_calls++;
    return E_FAIL;
}
#define realloc controlled_realloc
#define SHParseDisplayName controlled_parse
#define wWinMain embedded_shell_main
#include "../app/fpshell.c"
#undef wWinMain
#undef SHParseDisplayName
#undef realloc

static int passed, failed;
#define CHECK(label, condition) do { \
    if (condition) { passed++; printf("  ok    %s\n", label); } \
    else { failed++; printf("  FAIL  %s\n", label); } \
} while (0)

static int feed(req *r, const char *bytes, size_t count)
{
    char line[SHELL_LINE_MAX + 1];
    size_t len = 0;
    int complete = 0;
    for (size_t i = 0; i < count; i++) complete += req_byte(r, line, &len, bytes[i]);
    return complete;
}

static void test_existing_prefix_sibling(void)
{
    wchar_t temp[MAX_PATH], path[4096], full[4096], *dirs[32] = {0};
    int depth = 0, ready = 0, fixture_reported = 0;
    DWORD n = GetTempPathW(MAX_PATH, temp);
    if (!n || n >= MAX_PATH) goto done;
    _snwprintf(path, 4096, L"\\\\?\\%lsopenpilot-shellwire-%lu", temp, GetCurrentProcessId());
    if (!CreateDirectoryW(path, NULL)) goto done;
    dirs[depth++] = _wcsdup(path);
    const int target_bytes = SHELL_LINE_MAX - (int)strlen("item ");
    int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, NULL, 0, NULL, NULL) - 1;
    while (target_bytes - bytes - 1 > 700 && depth < 30) {
        size_t at = wcslen(path);
        path[at++] = L'\\';
        wmemset(path + at, L'\u6f22', 200);
        path[at + 200] = 0;
        if (!CreateDirectoryW(path, NULL)) goto done;
        dirs[depth++] = _wcsdup(path);
        bytes += 601; /* separator plus 200 three-byte UTF-8 characters */
    }
    int remaining = target_bytes - bytes - 1;
    if (remaining < 2 || remaining > 700) goto done;
    size_t at = wcslen(path);
    path[at++] = L'\\';
    wmemset(path + at, L'\u6f22', (size_t)remaining / 3);
    at += (size_t)remaining / 3;
    for (int i = 0; i < remaining % 3; i++) path[at++] = L'a';
    path[at] = 0;
    wcscpy(full, path);
    full[at] = L'x'; full[at + 1] = 0;
    HANDLE prefix = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE selected = CreateFileW(full, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (prefix != INVALID_HANDLE_VALUE) CloseHandle(prefix);
    if (selected != INVALID_HANDLE_VALUE) CloseHandle(selected);
    ready = prefix != INVALID_HANDLE_VALUE && selected != INVALID_HANDLE_VALUE;
    CHECK("an existing 8191-byte prefix and its longer selected sibling are created", ready);
    fixture_reported = 1;
    if (ready) {
        int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, full, -1, NULL, 0, NULL, NULL);
        char *encoded = malloc((size_t)length), *request = malloc((size_t)length + 100);
        req *r = calloc(1, sizeof *r);
        if (encoded && request && r) {
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, full, -1, encoded, length, NULL, NULL);
            int written = snprintf(request, (size_t)length + 100,
                                   "op item\nname Open\ncount 1\nitem %s\nend \n", encoded);
            int complete = feed(r, request, (size_t)written);
            CHECK("the existing-prefix sibling cannot be substituted for the selected target",
                  complete == 1 && r->bad && r->n == 0);
            HANDLE input = NULL, output = NULL;
            if (CreatePipe(&input, &output, NULL, 0)) {
                S.out = output;
                int before = shell_parse_calls;
                run(r);
                char reply_line[256] = {0}; DWORD got = 0;
                int read = ReadFile(input, reply_line, sizeof reply_line - 1, &got, NULL);
                CHECK("a malformed target returns an error without reaching shell parsing",
                      read && !strncmp(reply_line, "err malformed shell request", 27) &&
                      shell_parse_calls == before);
                CloseHandle(input); CloseHandle(output); S.out = NULL;
            } else CHECK("a malformed target returns an error without reaching shell parsing", 0);
        } else CHECK("the existing-prefix sibling cannot be substituted for the selected target", 0);
        free(encoded); free(request); req_free(r);
    }
    DeleteFileW(full);
    DeleteFileW(path);
done:
    while (depth) { depth--; if (dirs[depth]) { RemoveDirectoryW(dirs[depth]); free(dirs[depth]); } }
    if (!fixture_reported) CHECK("an existing 8191-byte prefix and its longer selected sibling are created", 0);
}

int wmain(void)
{
    puts("shell relay: bounded exact request framing");
    wchar_t *path = malloc((SHELL_LINE_MAX + 2) * sizeof *path);
    CHECK("boundary fixture allocates", path != NULL);
    if (!path) return 1;
    wmemset(path, L'a', SHELL_LINE_MAX - strlen("item "));
    path[SHELL_LINE_MAX - strlen("item ")] = 0;
    shellwire_text t = {0};
    CHECK("an exact 8191-byte item line is encoded", !shellwire_add(&t, "item", path) &&
          t.len == SHELL_LINE_MAX + 1);
    size_t prior = t.len;
    path[SHELL_LINE_MAX - strlen("item ")] = L'x';
    path[SHELL_LINE_MAX - strlen("item ") + 1] = 0;
    CHECK("an 8192-byte item line is rejected without modifying the request",
          shellwire_add(&t, "item", path) && t.len == prior && t.p[prior] == 0);
    free(t.p);
    free(path);

    shellwire_text malformed = {0};
    CHECK("line breaks in a target are rejected", shellwire_add(&malformed, "item", L"C:\\one\ntwo") && !malformed.p);
    wchar_t invalid_utf16[] = { L'C', L':', L'\\', 0xd800, 0 };
    CHECK("unencodable target names are rejected", shellwire_add(&malformed, "item", invalid_utf16) && !malformed.p);
    fail_realloc = 1;
    CHECK("serializer allocation failure leaves no partial request", shellwire_add(&malformed, "op", L"item") &&
          !malformed.p && !malformed.len);

    shellwire_text good = {0};
    int encoded = !shellwire_add(&good, "op", L"item") && !shellwire_add(&good, "name", L"Open") &&
                  !shellwire_add(&good, "count", L"2") &&
                  !shellwire_add(&good, "item", L"C:\\alpha") &&
                  !shellwire_add(&good, "item", L"C:\\beta") &&
                  !shellwire_add(&good, "end", L"");
    req *r = calloc(1, sizeof *r);
    CHECK("a complete two-item request preserves both exact targets", encoded && r &&
          feed(r, good.p, good.len) == 1 && !r->bad && r->n == 2 && r->expected == 2 &&
          !wcscmp(r->items[0], L"C:\\alpha") && !wcscmp(r->items[1], L"C:\\beta"));
    req_free(r);
    free(good.p);

    r = calloc(1, sizeof *r);
    const char *short_set = "op item\nname Open\ncount 2\nitem C:\\alpha\nend \n";
    CHECK("a missing selected item poisons the whole request", r &&
          feed(r, short_set, strlen(short_set)) == 1 && r->bad && r->n == 1);
    req_free(r);

    r = calloc(1, sizeof *r);
    const char *oom = "op item\nname Open\ncount 1\nitem C:\\alpha\nend \n";
    if (r) {
        char line[SHELL_LINE_MAX + 1]; size_t len = 0;
        const char *item = strstr(oom, "item C:");
        for (const char *p = oom; p < item; p++) req_byte(r, line, &len, *p);
        fail_realloc = 1;
        for (const char *p = item; *p; p++) req_byte(r, line, &len, *p);
    }
    CHECK("item-vector allocation failure rejects rather than omits the item", r && r->bad && r->n == 0);
    req_free(r);

    r = calloc(1, sizeof *r);
    char *overlong = malloc(SHELL_LINE_MAX + 100);
    CHECK("overlong-line fixture allocates", r && overlong);
    if (r && overlong) {
        const char *head = "op item\nname Open\ncount 1\nitem ";
        size_t start = strlen(head);
        memcpy(overlong, head, start);
        memset(overlong + start, 'p', SHELL_LINE_MAX - strlen("item "));
        size_t n = start + SHELL_LINE_MAX - strlen("item ");
        overlong[n++] = 'x'; /* selected target has a valid 8191-byte prefix */
        memcpy(overlong + n, "\nend \n", 6); n += 6;
        CHECK("an overlong selected path is never parsed as its 8191-byte prefix",
              feed(r, overlong, n) == 1 && r->bad && r->n == 0);
    }
    free(overlong);
    req_free(r);

    r = calloc(1, sizeof *r);
    const char *invalid = "op item\nname Open\ncount 1\nitem C:\\good\nitem C:\\extra\nend \n";
    CHECK("an extra selected item also rejects the whole request", r &&
          feed(r, invalid, strlen(invalid)) == 1 && r->bad);
    req_free(r);
    test_existing_prefix_sibling();
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
