/* relay.c - see relay.h. */
#include "relay.h"

#include <objbase.h>
#include <shellapi.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "relaycmd.h"
#include "shellwire.h"
#include "pipewriter.h"

#define RUNS_MAX 16 /* a command without %* runs once per item: more is likely a mistake */

enum { K_NONE, K_RUN, K_NEW, K_SENDTO, K_ITEM, K_VERB, K_MANY };
static const char *const kind_opt[] = { "", "run", "new", "sendto", "item", "verb" };

static const fn_opt relay_opts[] = {
    { "title", "", "menu text (default: the name after relay.)" },
    { "run", "", "a command line: %1 the item, %* every item, %D the folder, %NAME% a variable; "
                 "without %* it runs once per item" },
    { "new", "", "a file to make in the folder, then rename, e.g. New note.md" },
    { "template", "", "with new: a file to copy (default: an empty file)" },
    { "sendto", "", "an entry of Send to, e.g. Compressed (zipped) folder (runs in folderwing-shell.exe)" },
    { "item", "", "an item of Explorer's menu by its text, e.g. 7-Zip > Add to archive (runs in folderwing-shell.exe)" },
    { "verb", "", "an Explorer verb, e.g. runas or print (runs in folderwing-shell.exe)" },
    { "handler", "", "with item or verb: the shell extension's CLSID, so only it loads" },
    { "fallback", "on", "with handler: try the full Explorer menu if the selected extension cannot handle the action", "on off" },
    { "on", "", "where it shows: files, folders (drives too), background (empty space); default: files folders, "
                "for new background", "+files folders background" },
    { "types", "", "the file extensions it shows for, e.g. txt md (default: any)" },
    { "count", "any", "any, or one: only when one item is targeted", "any one" },
    { "window", "auto", "run: where it shows. auto: console programs in the panel terminal, others normal; "
                        "panel: in the panel terminal; normal, minimized, hidden: its own window",
      "auto panel normal minimized hidden" },
    { NULL },
};

static int kind_of(const fn_def *d)
{
    int k = K_NONE;
    for (int i = K_RUN; i <= K_VERB; i++)
        if (fn_option(d, kind_opt[i])[0]) k = k == K_NONE ? i : K_MANY;
    return k;
}

static int has_word(const char *list, const char *word)
{
    size_t n = strlen(word);
    for (const char *p = list; *p;) {
        while (*p == ' ' || *p == ',') p++;
        const char *e = p;
        while (*e && *e != ' ' && *e != ',') e++;
        if ((size_t)(e - p) == n && !_strnicmp(p, word, n)) return 1;
        p = e;
    }
    return 0;
}

static const char *where_on(const fn_def *d, int kind)
{
    const char *on = fn_option(d, "on");
    return on[0] ? on : kind == K_NEW ? "background" : "files folders";
}

static void title_of(const fn_def *d, wchar_t *buf, size_t cap)
{
    _snwprintf(buf, cap, L"%ls", d->title && d->title[0] ? d->title : L"Relay");
    buf[cap - 1] = 0;
}

static wchar_t *widen(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
    if (w && !MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) w[0] = 0;
    return w;
}

/* Whether d fits c. Without paths (a key, the palette) only what the
 * context says is checked; run checks again once it has them. */
static int fits(const fn_def *d, const fn_ctx *c)
{
    int kind = kind_of(d);
    if (kind == K_NONE || kind == K_MANY) return 0;
    const char *on = where_on(d, kind);
    if (c->n == 0) return has_word(on, "background") && c->dir && (kind != K_NEW || c->where == FN_PANE);
    if (kind == K_NEW) return 0;
    if (!_stricmp(fn_option(d, "count"), "one") && c->n != 1) return 0;
    int known = c->paths || c->n == 1;
    uint32_t dirs = c->paths ? c->dirs : (c->state & FN_S_DIR) ? 1 : 0;
    if (known) {
        if (dirs && !has_word(on, "folders")) return 0;
        if (c->n > dirs && !has_word(on, "files")) return 0;
    } else if (!has_word(on, "files") && !has_word(on, "folders")) {
        return 0;
    }
    const char *types = fn_option(d, "types");
    if (types[0] && c->paths) {
        uint32_t fit = 0;
        for (uint32_t i = 0; i < c->n; i++) fit += relay_type_ok(c->paths[i], types);
        if (fit + dirs < c->n) return 0;
    }
    return 1;
}

static int relay_applies(const fn_ctx *c) { return fits(fn_self(), c); }

static void relay_configure(void)
{
    const fn_def *d = fn_self();
    int kind = kind_of(d);
    if (kind == K_NONE) fn_config_error(d, "[%s] needs one of run, new, sendto, item or verb", d->id);
    else if (kind == K_MANY) fn_config_error(d, "[%s] has more than one of run, new, sendto, item and verb", d->id);
    const char *handler = fn_option(d, "handler");
    if (handler[0]) {
        CLSID id;
        wchar_t *w = widen(handler);
        if (kind != K_ITEM && kind != K_VERB) fn_config_error(d, "[%s] handler goes with item or verb", d->id);
        else if (!w || FAILED(IIDFromString(w, &id))) fn_config_error(d, "[%s] handler = %s: use a CLSID like {...}", d->id, handler);
        free(w);
    }
    if (fn_option(d, "template")[0] && kind != K_NEW) fn_config_error(d, "[%s] template goes with new", d->id);
    if (kind == K_NEW && strpbrk(fn_option(d, "new"), "\\/:*?\"<>|"))
        fn_config_error(d, "[%s] new = %s: a name, without \\ / : * ? \" < > |", d->id, fn_option(d, "new"));
}

/* ---- run: command lines -------------------------------------------------------------- */

static int start_one(const wchar_t *line, const wchar_t *dir, int show, wchar_t *err, size_t ecap)
{
    STARTUPINFOW si = { sizeof si };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = (WORD)show;
    PROCESS_INFORMATION pi;
    size_t len = wcslen(line);
    wchar_t *mutable_line = malloc((len + 1) * sizeof *mutable_line);
    if (!mutable_line) return -1;
    wcscpy(mutable_line, line);
    DWORD flags = CREATE_DEFAULT_ERROR_MODE | CREATE_UNICODE_ENVIRONMENT | (show == SW_HIDE ? CREATE_NO_WINDOW : 0);
    BOOL ok = CreateProcessW(NULL, mutable_line, NULL, NULL, FALSE, flags, NULL, dir, &si, &pi);
    DWORD e = ok ? 0 : GetLastError();
    free(mutable_line);
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 0;
    }
    /* not a program on its own: a script, an App Paths name, a document */
    wchar_t program[MAX_PATH];
    const wchar_t *rest;
    relay_split(line, program, MAX_PATH, &rest);
    SHELLEXECUTEINFOW sei = { sizeof sei };
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = app_window();
    sei.lpFile = program;
    sei.lpParameters = rest[0] ? rest : NULL;
    sei.lpDirectory = dir;
    sei.nShow = show;
    if (ShellExecuteExW(&sei)) return 0;
    DWORD e2 = GetLastError();
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e2 ? e2 : e, 0, err, (DWORD)ecap, NULL);
    for (wchar_t *p = err; *p; p++)
        if (*p == L'\r' || *p == L'\n') *p = L' ';
    return -1;
}

/* Whether the command line's program is a console .exe (its PE header says so). */
static int is_console(const wchar_t *line)
{
    wchar_t program[MAX_PATH], exe[MAX_PATH];
    const wchar_t *rest;
    relay_split(line, program, MAX_PATH, &rest);
    if (!program[0] || !SearchPathW(NULL, program, L".exe", MAX_PATH, exe, NULL)) return 0;
    HANDLE f = CreateFileW(exe, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt; /* Subsystem sits at the same place in 32-bit headers' OptionalHeader up to it */
    DWORD got = 0;
    int console = 0;
    if (ReadFile(f, &dos, sizeof dos, &got, NULL) && got == sizeof dos && dos.e_magic == IMAGE_DOS_SIGNATURE &&
        SetFilePointer(f, dos.e_lfanew, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
        ReadFile(f, &nt, sizeof nt, &got, NULL) && got >= offsetof(IMAGE_NT_HEADERS64, OptionalHeader.Subsystem) + 2 &&
        nt.Signature == IMAGE_NT_SIGNATURE) {
        WORD sub = nt.FileHeader.Machine == IMAGE_FILE_MACHINE_I386 ? ((IMAGE_NT_HEADERS32 *)&nt)->OptionalHeader.Subsystem
                                                                    : nt.OptionalHeader.Subsystem;
        console = sub == IMAGE_SUBSYSTEM_WINDOWS_CUI;
    }
    CloseHandle(f);
    return console;
}

static void run_command(const fn_def *d, fn_ctx *c)
{
    wchar_t title[80], err[200] = L"";
    title_of(d, title, 80);
    wchar_t *cmd = widen(fn_option(d, "run"));
    wchar_t *line = malloc(32768 * sizeof *line);
    const char *w = fn_option(d, "window");
    int show = !_stricmp(w, "hidden") ? SW_HIDE : !_stricmp(w, "minimized") ? SW_SHOWMINNOACTIVE : SW_SHOWNORMAL;
    int panel = !_stricmp(w, "panel"), automatic = !_stricmp(w, "auto");
    const wchar_t *const *paths = (const wchar_t *const *)c->paths;
    int once = !c->n || relay_takes_all(cmd ? cmd : L"");
    uint32_t runs = once ? 1 : c->n;
    if (!cmd || !line) {
    } else if (runs > RUNS_MAX) {
        app_status(c->pane, 1, L"%ls runs once per item: %u is more than %d (use %%* to give it all at once)", title,
                   runs, RUNS_MAX);
    } else {
        for (uint32_t i = 0; i < runs; i++) {
            if (relay_expand(cmd, paths, c->n, once ? -1 : (int)i, c->dir, line, 32768) < 0) {
                app_status(c->pane, 1, L"%ls: the command line is too long", title);
                break;
            }
            if (panel || (automatic && is_console(line))) {
                if (app_terminal_run(c->dir, line, title)) break;
            } else if (start_one(line, c->dir, show, err, 200)) {
                app_status(c->pane, 1, L"%ls: %ls", title, err);
                break;
            }
        }
    }
    free(cmd);
    free(line);
}

/* ---- the helper: folderwing-shell.exe --------------------------------------------------------------- */

static struct {
    HANDLE   proc, to;   /* to: its stdin */
    pipe_writer *writer;
    DWORD    pid;
    uint32_t gen;        /* replies from an older helper are dropped */
    uint32_t pending;    /* requests without a reply: each holds shell.helper */
} H;

typedef struct reader_arg {
    HANDLE   from;
    HWND     to;
    uint32_t gen;
} reader_arg;

static DWORD WINAPI reader(void *arg)
{
    reader_arg a = *(reader_arg *)arg;
    free(arg);
    char line[1024];
    size_t len = 0;
    for (;;) {
        char ch;
        DWORD got = 0;
        if (!ReadFile(a.from, &ch, 1, &got, NULL) || !got) break;
        if (ch != '\n') {
            if (len < sizeof line - 1) line[len++] = ch;
            continue;
        }
        line[len] = 0;
        len = 0;
        char *m = _strdup(line);
        if (m && !PostMessageW(a.to, WM_APP_RELAY, a.gen, (LPARAM)m)) free(m);
    }
    CloseHandle(a.from);
    PostMessageW(a.to, WM_APP_RELAY, a.gen, 0);
    return 0;
}

static const fn_def *helper_def(void) { return fn_find("shell.helper"); }

static void helper_stop(void)
{
    H.gen++;
    pipe_writer_cancel(&H.writer);
    if (H.to) CloseHandle(H.to); /* its stdin ends: it leaves once its windows are closed */
    if (H.proc) CloseHandle(H.proc);
    H.to = H.proc = NULL;
    H.pid = 0;
    while (H.pending) {
        H.pending--;
        fn_release(helper_def());
    }
}

static int helper_start(void)
{
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *slash = n && n < MAX_PATH ? wcsrchr(exe, L'\\') : NULL;
    if (!slash || slash - exe + 13 >= MAX_PATH) return -1;
    wcscpy(slash + 1, L"folderwing-shell.exe");
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE child_in = NULL, to = NULL, from = NULL, child_out = NULL;
    if (!CreatePipe(&child_in, &to, &sa, 1 << 16) || !CreatePipe(&from, &child_out, &sa, 1 << 12)) goto fail;
    SetHandleInformation(to, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(from, HANDLE_FLAG_INHERIT, 0);
    HANDLE inherit[2] = { child_in, child_out };
    SIZE_T asize = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &asize);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = malloc(asize);
    if (!attrs || !InitializeProcThreadAttributeList(attrs, 1, 0, &asize)) {
        free(attrs);
        goto fail;
    }
    STARTUPINFOEXW si = { { sizeof si } };
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = child_in;
    si.StartupInfo.hStdOutput = child_out;
    si.StartupInfo.hStdError = child_out;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi;
    wchar_t cmd[MAX_PATH + 4];
    _snwprintf(cmd, MAX_PATH + 4, L"\"%ls\"", exe);
    cmd[MAX_PATH + 3] = 0;
    /* no job: a dialog an extension opened outlives OpenPilot, as it would Explorer */
    BOOL ok = UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, NULL, NULL) &&
              CreateProcessW(exe, cmd, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_DEFAULT_ERROR_MODE, NULL,
                             NULL, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    free(attrs);
    CloseHandle(child_in);
    CloseHandle(child_out);
    child_in = child_out = NULL;
    if (!ok) goto fail;
    CloseHandle(pi.hThread);
    reader_arg *ra = malloc(sizeof *ra);
    HANDLE t = NULL;
    if (ra) {
        *ra = (reader_arg){ from, app_window(), H.gen + 1 };
        t = CreateThread(NULL, 64 * 1024, reader, ra, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    }
    if (!t) {
        free(ra);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        goto fail;
    }
    CloseHandle(t);
    H.gen++;
    H.proc = pi.hProcess;
    H.pid = pi.dwProcessId;
    H.to = to;
    H.writer = pipe_writer_begin(to, SHELL_REQUEST_MAX, app_window(), WM_APP_RELAY, H.gen);
    if (!H.writer) { helper_stop(); return -1; }
    return 0;
fail:
    if (child_in) CloseHandle(child_in);
    if (child_out) CloseHandle(child_out);
    if (to) CloseHandle(to);
    if (from) CloseHandle(from);
    return -1;
}

static void run_helper(const fn_def *d, int kind, fn_ctx *c)
{
    wchar_t title[80];
    title_of(d, title, 80);
    shellwire_text t = { 0 };
    static const wchar_t *ops[] = { L"", L"", L"", L"sendto", L"item", L"verb" };
    wchar_t *name = widen(fn_option(d, kind_opt[kind]));
    wchar_t *handler = widen(fn_option(d, "handler"));
    int ok = name && handler && c->n <= SHELL_ITEMS_MAX &&
             !shellwire_add(&t, "op", ops[kind]) && !shellwire_add(&t, "name", name);
    if (ok && handler[0]) ok = !shellwire_add(&t, "handler", handler);
    if (ok) ok = !shellwire_add(&t, "fallback", !strcmp(fn_option(d, "fallback"), "off") ? L"0" : L"1");
    if (ok && c->dir) ok = !shellwire_add(&t, "dir", c->dir);
    wchar_t count[32];
    _snwprintf(count, 32, L"%u", c->n);
    if (ok) ok = !shellwire_add(&t, "count", count);
    for (uint32_t i = 0; ok && i < c->n; i++) ok = c->paths && c->paths[i] && !shellwire_add(&t, "item", c->paths[i]);
    POINT pt;
    GetCursorPos(&pt);
    wchar_t num[48];
    _snwprintf(num, 48, L"%ld %ld", pt.x, pt.y);
    if (ok) ok = !shellwire_add(&t, "pt", num);
    if (ok) ok = !shellwire_add(&t, "extended", GetKeyState(VK_SHIFT) < 0 ? L"1" : L"0");
    if (ok) ok = !shellwire_add(&t, "end", L""); /* the only publication delimiter */
    free(name);
    free(handler);
    if (!ok) {
        free(t.p);
        app_status(c->pane, 1, L"%ls: shell request could not represent every selected item", title);
        return;
    }
    const fn_def *h = helper_def();
    if (fn_hold(h)) {
        free(t.p);
        app_status(c->pane, 1, L"%ls: folderwing-shell.exe did not start", title);
        return;
    }
    ok = !pipe_writer_send(H.writer, t.p, t.len, 0, 0);
    free(t.p);
    if (!ok) {
        fn_release(h);
        fn_stop(h); /* it is gone; the next use starts another */
        app_status(c->pane, 1, L"%ls: folderwing-shell.exe is not answering", title);
        return;
    }
    AllowSetForegroundWindow(H.pid); /* what it opens comes to the front */
    H.pending++;
}

void relay_message(WPARAM w, LPARAM l)
{
    char *m = (char *)l;
    if ((uint32_t)w != H.gen) {
        free(m);
        return;
    }
    const fn_def *h = helper_def();
    if (!m) { /* it ended */
        if (H.pending) app_status(app_active_pane(), 1, L"folderwing-shell.exe stopped before it was done");
        fn_stop(h);
        return;
    }
    if (H.pending) {
        H.pending--;
        if (!strncmp(m, "err ", 4)) {
            wchar_t *e = widen(m + 4);
            app_status(app_active_pane(), 1, L"%ls", e ? e : L"folderwing-shell.exe could not do it");
            free(e);
        }
        fn_release(h);
    }
    free(m);
}

/* ---- the function ---------------------------------------------------------------------- */

static void relay_run(fn_ctx *c)
{
    const fn_def *d = fn_self();
    wchar_t title[80];
    title_of(d, title, 80);
    fn_paths(c);
    if (!fits(d, c)) {
        app_status(c->pane, 1, L"%ls is not for these items", title);
        return;
    }
    int kind = kind_of(d);
    if(kind!=K_NEW){
        int native_only=fpe_wsl_requires_native(c->dir);
        for(uint32_t i=0;!native_only&&i<c->n;i++)native_only=fpe_wsl_requires_native(c->paths[i]);
        if(native_only){app_status(c->pane,1,L"This Linux name requires native operations; Windows relays cannot address it safely");return;}
    }
    if (kind == K_RUN) {
        run_command(d, c);
    } else if (kind == K_NEW) {
        wchar_t *name = widen(fn_option(d, "new")), *tmpl = NULL;
        wchar_t *raw = fn_option(d, "template")[0] ? widen(fn_option(d, "template")) : NULL;
        if (raw) {
            DWORD n = ExpandEnvironmentStringsW(raw, NULL, 0);
            tmpl = n ? malloc(n * sizeof *tmpl) : NULL;
            if (tmpl) ExpandEnvironmentStringsW(raw, tmpl, n);
        }
        if (name && (!raw || tmpl)) app_new_file(c->pane, name, tmpl, d);
        free(name);
        free(raw);
        free(tmpl);
    } else {
        run_helper(d, kind, c);
    }
}

static const fn_def relay_family = {
    .id = "relay.", .title = L"Relay", .life = FN_COLD, .where = FN_ANYWHERE,
    .menus = FN_MENU_ITEMS | FN_MENU_FOLDER | FN_MENU_TREE, .group = 6, .opts = relay_opts,
    .about = "a menu item of your own: a command line, a new file, a Send to entry or an item of Explorer's menu",
    .applies = relay_applies, .run = relay_run, .configure = relay_configure,
};

static const fn_def relay_functions[] = {
    { .id = "shell.helper", .title = L"Shell helper", .life = FN_WARM, .idle_ms = 60000, .flags = FN_HIDDEN,
      .about = "folderwing-shell.exe: runs relays of Explorer's items and Send to, so shell extensions load there, not in "
               "Folderwing (stopped 60 s after use; a window it opened keeps it until closed)",
      .start = helper_start, .stop = helper_stop },
};

void relay_register(void)
{
    fn_register(relay_functions, sizeof relay_functions / sizeof relay_functions[0]);
    fn_family(&relay_family);
}
