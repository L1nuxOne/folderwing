/* folderwing-shell.exe - runs relays of Explorer's menu items and Send to (relay.h).
 *
 * Shell extensions load here, so OpenPilot itself never maps them. It reads
 * requests on stdin (UTF-8 lines "key value", ended by a line "end"):
 *   op       item | verb | sendto
 *   name     the item's text ("7-Zip > Add to archive"), the verb, or the Send to entry
 *   handler  {CLSID}: only that extension is loaded (item, verb)
 *   dir      the folder;  item  a path (repeated; none: the folder's empty space)
 *   count    exact number of item lines (required, including zero for empty space)
 *   pt       x y (screen);  extended  1 when Shift was held
 * and answers each with one line: "ok" or "err <what went wrong>".
 *
 * Extensions often keep working after InvokeCommand returns (a dialog, a
 * copy with progress). As in Explorer, they hold the process through
 * SHGetInstanceExplorer; when stdin ends, it leaves once nothing holds it,
 * none of its windows are shown and nothing ran for 2 s. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "relaycmd.h"
#include "shellwire.h"

#define WM_REQ  (WM_APP + 1) /* lParam: a req */
#define WM_EOF  (WM_APP + 2)
#define TIMER_EXIT 1
#define QUIET_MS   2000

typedef struct req {
    wchar_t  op[16], *name, *handler, *dir;
    wchar_t **items;
    uint32_t n, expected;
    POINT    pt;
    int      extended, no_fallback, count_seen, op_seen, name_seen, bad;
} req;

static struct {
    HWND      inbox, owner;
    HANDLE    out;
    LONG      refs;
    int       closing;
    ULONGLONG last;
} S;

/* ---- the process reference extensions take ------------------------------------------- */

static HRESULT STDMETHODCALLTYPE ref_qi(IUnknown *u, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown)) {
        *out = u;
        InterlockedIncrement(&S.refs);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ref_add(IUnknown *u) { return (ULONG)InterlockedIncrement(&S.refs); }
static ULONG STDMETHODCALLTYPE ref_release(IUnknown *u)
{
    LONG n = InterlockedDecrement(&S.refs);
    if (S.inbox) PostMessageW(S.inbox, WM_NULL, 0, 0); /* look at leaving again */
    return (ULONG)n;
}
static IUnknownVtbl ref_vtbl = { ref_qi, ref_add, ref_release };
static IUnknown proc_ref = { &ref_vtbl };

/* ---- requests ------------------------------------------------------------------------------ */

static wchar_t *widen(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
    if (w && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n)) {
        free(w);
        return NULL;
    }
    return w;
}

static void req_free(req *r)
{
    if (!r) return;
    free(r->name);
    free(r->handler);
    free(r->dir);
    for (uint32_t i = 0; i < r->n; i++) free(r->items[i]);
    free(r->items);
    free(r);
}

/* 1 = the request is complete. Any invalid field poisons the whole request;
 * the reader keeps framing through end, but run() will never invoke it. */
static int req_line(req *r, char *line)
{
    char *val = strchr(line, ' ');
    if (val) *val++ = 0;
    else val = "";
    if (!strcmp(line, "end")) {
        if (*val || !r->op_seen || !r->name_seen || !r->count_seen || r->n != r->expected)
            r->bad = 1;
        return 1;
    }
    if (r->bad) return 0;
    if (!strcmp(line, "count")) {
        uint32_t n = 0;
        if (r->count_seen || !*val) { r->bad = 1; return 0; }
        for (const char *p = val; *p; p++) {
            if (*p < '0' || *p > '9' || n > (SHELL_ITEMS_MAX - (unsigned)(*p - '0')) / 10) {
                r->bad = 1;
                return 0;
            }
            n = n * 10 + (unsigned)(*p - '0');
        }
        r->expected = n;
        r->count_seen = 1;
        return 0;
    }
    wchar_t *w = widen(val);
    if (!w) { r->bad = 1; return 0; }
    if (!strcmp(line, "op")) {
        if (r->op_seen || wcslen(w) >= sizeof r->op / sizeof r->op[0] ||
            (wcscmp(w, L"item") && wcscmp(w, L"verb") && wcscmp(w, L"sendto"))) r->bad = 1;
        else { wcscpy(r->op, w); r->op_seen = 1; }
        free(w);
    } else if (!strcmp(line, "name")) {
        if (r->name_seen || !w[0]) { r->bad = 1; free(w); }
        else { r->name = w; r->name_seen = 1; }
    } else if (!strcmp(line, "handler")) {
        if (r->handler) { r->bad = 1; free(w); }
        else r->handler = w;
    } else if (!strcmp(line, "dir")) {
        if (r->dir || !w[0]) { r->bad = 1; free(w); }
        else r->dir = w;
    } else if (!strcmp(line, "item")) {
        if (!w[0] || r->n >= SHELL_ITEMS_MAX || (r->count_seen && r->n >= r->expected)) {
            r->bad = 1;
            free(w);
        } else {
            wchar_t **v = realloc(r->items, (r->n + 1) * sizeof *v);
            if (v) { r->items = v; v[r->n++] = w; }
            else { r->bad = 1; free(w); }
        }
    } else if (!strcmp(line, "pt")) {
        wchar_t extra;
        if (swscanf(w, L"%ld %ld %lc", &r->pt.x, &r->pt.y, &extra) != 2) r->bad = 1;
        free(w);
    } else if (!strcmp(line,"fallback")) {
        if ((w[0] != L'0' && w[0] != L'1') || w[1]) r->bad = 1;
        else r->no_fallback = w[0] == L'0';
        free(w);
    } else if (!strcmp(line, "extended")) {
        if ((w[0] != L'0' && w[0] != L'1') || w[1]) r->bad = 1;
        else r->extended = w[0] == L'1';
        free(w);
    } else {
        r->bad = 1;
        free(w);
    }
    return 0;
}

static int req_byte(req *r, char *line, size_t *len, char ch)
{
    if (ch != '\n') {
        if (!ch) r->bad = 1;
        if (*len < SHELL_LINE_MAX) line[(*len)++] = ch;
        else r->bad = 1;
        return 0;
    }
    line[*len] = 0;
    if (*len && line[*len - 1] == '\r') line[*len - 1] = 0;
    *len = 0;
    return req_line(r, line);
}

static DWORD WINAPI reader(void *arg)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    char chunk[4096], line[SHELL_LINE_MAX + 1];
    size_t len = 0;
    req *r = calloc(1, sizeof *r);
    for (;;) {
        DWORD got = 0;
        if (!r || !ReadFile(in, chunk, sizeof chunk, &got, NULL) || !got) break;
        for (DWORD i = 0; i < got; i++) {
            if (req_byte(r, line, &len, chunk[i])) {
                if (!PostMessageW(S.inbox, WM_REQ, 0, (LPARAM)r)) req_free(r);
                r = calloc(1, sizeof *r);
                if (!r) break;
            }
        }
    }
    req_free(r);
    PostMessageW(S.inbox, WM_EOF, 0, 0);
    return 0;
}

static void reply(const wchar_t *err)
{
    char buf[1100];
    int n = 0;
    if (!err) {
        n = snprintf(buf, sizeof buf, "ok\n");
    } else {
        n = snprintf(buf, sizeof buf, "err ");
        int k = WideCharToMultiByte(CP_UTF8, 0, err, -1, buf + n, (int)sizeof buf - n - 2, NULL, NULL);
        n += k > 0 ? k - 1 : 0;
        for (int i = 4; i < n; i++)
            if (buf[i] == '\n' || buf[i] == '\r') buf[i] = ' ';
        buf[n++] = '\n';
    }
    DWORD wrote;
    WriteFile(S.out, buf, (DWORD)n, &wrote, NULL);
}

/* ---- what the items are, for the shell --------------------------------------------------- */

typedef struct items {
    PIDLIST_ABSOLUTE  folder;  /* the folder (the items' parent) */
    PIDLIST_ABSOLUTE *abs;
    PCUITEMID_CHILD  *kids;
    uint32_t          n;
    IShellFolder     *parent;  /* holds the kids (NULL with no items) */
} items;

static void items_free(items *it)
{
    for (uint32_t i = 0; it->abs && i < it->n; i++) CoTaskMemFree(it->abs[i]);
    free(it->abs);
    free(it->kids);
    if (it->parent) IShellFolder_Release(it->parent);
    CoTaskMemFree(it->folder);
    memset(it, 0, sizeof *it);
}

static HRESULT items_of(const req *r, items *it)
{
    memset(it, 0, sizeof *it);
    if (!r->n) {
        if (!r->dir) return E_INVALIDARG;
        return SHParseDisplayName(r->dir, NULL, &it->folder, 0, NULL);
    }
    it->abs = calloc(r->n, sizeof *it->abs);
    it->kids = calloc(r->n, sizeof *it->kids);
    if (!it->abs || !it->kids) return E_OUTOFMEMORY;
    HRESULT hr = S_OK;
    for (uint32_t i = 0; SUCCEEDED(hr) && i < r->n; i++, it->n++) {
        hr = SHParseDisplayName(r->items[i], NULL, &it->abs[i], 0, NULL);
        if (SUCCEEDED(hr)) it->kids[i] = (PCUITEMID_CHILD)ILFindLastID((PCUIDLIST_RELATIVE)it->abs[i]);
    }
    if (SUCCEEDED(hr)) hr = SHBindToParent(it->abs[0], &IID_IShellFolder, (void **)&it->parent, NULL);
    if (SUCCEEDED(hr)) {
        it->folder = ILClone(it->abs[0]);
        if (!it->folder) hr = E_OUTOFMEMORY;
        else ILRemoveLastID((PUIDLIST_RELATIVE)it->folder);
    }
    return hr;
}

static HRESULT data_object(const items *it, IDataObject **out)
{
    *out = NULL;
    if (!it->parent) return E_INVALIDARG;
    return IShellFolder_GetUIObjectOf(it->parent, S.owner, it->n, it->kids, &IID_IDataObject, NULL, (void **)out);
}

/* The registry key extensions are initialized with: the file type's, else *. */
static HKEY class_key(const req *r)
{
    wchar_t name[260] = L"*";
    if (!r->n) {
        wcscpy(name, L"Directory\\Background");
    } else {
        DWORD a = GetFileAttributesW(r->items[0]);
        const wchar_t *dot = wcsrchr(r->items[0], L'.'), *slash = wcsrchr(r->items[0], L'\\');
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
            wcscpy(name, L"Directory");
        } else if (dot && (!slash || dot > slash)) {
            DWORD size = sizeof name;
            if (RegGetValueW(HKEY_CLASSES_ROOT, dot, NULL, RRF_RT_REG_SZ, NULL, name, &size) || !name[0])
                wcsncpy(name, dot, 259);
        }
    }
    HKEY k = NULL;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, name, 0, KEY_READ, &k)) k = NULL;
    return k;
}

/* ---- context menus ------------------------------------------------------------------------- */

static void menu_text(HMENU m, int pos, wchar_t *out, size_t cap)
{
    wchar_t raw[512] = L"";
    MENUITEMINFOW mi = { sizeof mi };
    mi.fMask = MIIM_STRING;
    mi.dwTypeData = raw;
    mi.cch = 512;
    if (!GetMenuItemInfoW(m, (UINT)pos, TRUE, &mi)) raw[0] = 0;
    raw[511] = 0;
    relay_norm(raw, out, cap);
}

/* Lets a handler fill a submenu it only fills when it opens. */
static void open_sub(IContextMenu *cm, HMENU sub, int pos)
{
    IContextMenu3 *cm3 = NULL;
    IContextMenu2 *cm2 = NULL;
    LRESULT lr;
    if (SUCCEEDED(IContextMenu_QueryInterface(cm, &IID_IContextMenu3, (void **)&cm3))) {
        IContextMenu3_HandleMenuMsg2(cm3, WM_INITMENUPOPUP, (WPARAM)sub, MAKELPARAM(pos, FALSE), &lr);
        IContextMenu3_Release(cm3);
    } else if (SUCCEEDED(IContextMenu_QueryInterface(cm, &IID_IContextMenu2, (void **)&cm2))) {
        IContextMenu2_HandleMenuMsg(cm2, WM_INITMENUPOPUP, (WPARAM)sub, MAKELPARAM(pos, FALSE));
        IContextMenu2_Release(cm2);
    }
}

/* The first part of "A > B" as written, for messages. */
static void raw_part(const wchar_t *path, wchar_t *out, size_t cap)
{
    const wchar_t *gt = wcsstr(path, L" > ");
    size_t n = gt ? (size_t)(gt - path) : wcslen(path);
    if (n >= cap) n = cap - 1;
    wmemcpy(out, path, n);
    out[n] = 0;
}

/* The id of the item at path (text by text, or a verb at the last step), or 0. */
static UINT find_item(IContextMenu *cm, HMENU m, const wchar_t *path, UINT first, wchar_t *why, size_t wcap)
{
    wchar_t seg[512], text[512], raw[512];
    const wchar_t *rest = relay_segment(path, seg, 512);
    raw_part(path, raw, 512);
    int count = GetMenuItemCount(m);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < count; i++) {
            MENUITEMINFOW mi = { sizeof mi };
            mi.fMask = MIIM_FTYPE | MIIM_ID | MIIM_SUBMENU | MIIM_STATE;
            if (!GetMenuItemInfoW(m, (UINT)i, TRUE, &mi) || (mi.fType & MFT_SEPARATOR)) continue;
            int match = 0;
            if (pass == 0) {
                menu_text(m, i, text, 512);
                match = text[0] && !wcscmp(text, seg);
            } else if (!rest && !mi.hSubMenu && mi.wID >= first) { /* by verb: "properties", "runas" */
                wchar_t verb[128] = L"";
                if (SUCCEEDED(IContextMenu_GetCommandString(cm, mi.wID - first, GCS_VERBW, NULL, (LPSTR)verb, 128)) &&
                    verb[0]) {
                    relay_norm(verb, text, 512);
                    match = !wcscmp(text, seg);
                }
            }
            if (!match) continue;
            if (mi.hSubMenu) {
                if (!rest) {
                    _snwprintf(why, wcap, L"\"%ls\" is a submenu: name an item in it (\"%ls > ...\")", raw, raw);
                    return 0;
                }
                open_sub(cm, mi.hSubMenu, i);
                return find_item(cm, mi.hSubMenu, rest, first, why, wcap);
            }
            if (rest) {
                _snwprintf(why, wcap, L"\"%ls\" has no submenu", raw);
                return 0;
            }
            if (mi.fState & MFS_DISABLED) {
                _snwprintf(why, wcap, L"\"%ls\" is greyed out for these items", raw);
                return 0;
            }
            return mi.wID;
        }
    }
    _snwprintf(why, wcap, L"no \"%ls\" in Explorer's menu for these items", raw);
    return 0;
}

static HRESULT invoke(IContextMenu *cm, const req *r, UINT offset, const wchar_t *verb)
{
    char averb[128] = "";
    if (verb) WideCharToMultiByte(CP_ACP, 0, verb, -1, averb, sizeof averb, NULL, NULL);
    CMINVOKECOMMANDINFOEX ici = { sizeof ici };
    ici.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE | CMIC_MASK_ASYNCOK;
    if (r->extended) ici.fMask |= CMIC_MASK_SHIFT_DOWN;
    ici.hwnd = S.owner;
    ici.lpVerb = verb ? averb : MAKEINTRESOURCEA(offset);
    ici.lpVerbW = verb ? verb : MAKEINTRESOURCEW(offset);
    ici.lpDirectoryW = r->dir;
    ici.nShow = SW_SHOWNORMAL;
    ici.ptInvoke = r->pt;
    return IContextMenu_InvokeCommand(cm, (CMINVOKECOMMANDINFO *)&ici);
}

static HRESULT run_menu(IContextMenu *cm, const req *r, wchar_t *why, size_t wcap)
{
    HMENU m = CreatePopupMenu();
    UINT flags = CMF_NORMAL | (r->extended ? CMF_EXTENDEDVERBS : 0);
    HRESULT hr = IContextMenu_QueryContextMenu(cm, m, 0, 1, 0x7FFF, flags);
    if (FAILED(hr)) {
        _snwprintf(why, wcap, L"the menu could not be built (0x%08lX)", (unsigned long)hr);
    } else if (!wcscmp(r->op, L"verb")) {
        hr = invoke(cm, r, 0, r->name);
        if (FAILED(hr)) _snwprintf(why, wcap, L"the verb \"%ls\" did not run (0x%08lX)", r->name, (unsigned long)hr);
    } else {
        UINT id = find_item(cm, m, r->name, 1, why, wcap);
        hr = id ? invoke(cm, r, id - 1, NULL) : E_FAIL;
        if (id && FAILED(hr)) _snwprintf(why, wcap, L"\"%ls\" did not run (0x%08lX)", r->name, (unsigned long)hr);
    }
    DestroyMenu(m);
    return hr;
}

/* ---- packaged items (Windows 11 menu: IExplorerCommand) ------------------------------------- */

static int command_is(IExplorerCommand *c, IShellItemArray *arr, const wchar_t *seg)
{
    LPWSTR title = NULL;
    wchar_t norm[512] = L"";
    if (SUCCEEDED(IExplorerCommand_GetTitle(c, arr, &title)) && title) {
        relay_norm(title, norm, 512);
        CoTaskMemFree(title);
    }
    return norm[0] && !wcscmp(norm, seg);
}

/* Finds path under c (whose own title may or may not be its first part). */
static IExplorerCommand *find_command(IExplorerCommand *c, IShellItemArray *arr, const wchar_t *path, int top)
{
    wchar_t seg[512];
    if (!path || !path[0]) {
        IExplorerCommand_AddRef(c);
        return c;
    }
    const wchar_t *rest = relay_segment(path, seg, 512);
    if (top && command_is(c, arr, seg)) return find_command(c, arr, rest, 0);
    EXPCMDFLAGS f = 0;
    IEnumExplorerCommand *e = NULL;
    if (FAILED(IExplorerCommand_GetFlags(c, &f)) || !(f & ECF_HASSUBCOMMANDS) ||
        FAILED(IExplorerCommand_EnumSubCommands(c, &e)) || !e)
        return NULL;
    IExplorerCommand *sub = NULL, *found = NULL;
    ULONG got = 0;
    while (!found && IEnumExplorerCommand_Next(e, 1, &sub, &got) == S_OK && got) {
        if (command_is(sub, arr, seg)) found = rest ? find_command(sub, arr, rest, 0) : (IExplorerCommand_AddRef(sub), sub);
        IExplorerCommand_Release(sub);
    }
    IEnumExplorerCommand_Release(e);
    return found;
}

/* "Title (A, B)": what a handler offers, for a name that didn't match. */
static void offered(IExplorerCommand *c, IShellItemArray *arr, wchar_t *out, size_t cap)
{
    LPWSTR t = NULL;
    size_t n = 0;
    out[0] = 0;
    if (SUCCEEDED(IExplorerCommand_GetTitle(c, arr, &t)) && t) {
        n += (size_t)_snwprintf(out, cap, L"\"%ls\"", t);
        CoTaskMemFree(t);
    }
    EXPCMDFLAGS f = 0;
    IEnumExplorerCommand *e = NULL;
    if (FAILED(IExplorerCommand_GetFlags(c, &f)) || !(f & ECF_HASSUBCOMMANDS) || FAILED(IExplorerCommand_EnumSubCommands(c, &e)) || !e)
        return;
    IExplorerCommand *sub = NULL;
    ULONG got = 0;
    int k = 0;
    while (n + 4 < cap && IEnumExplorerCommand_Next(e, 1, &sub, &got) == S_OK && got) {
        if (SUCCEEDED(IExplorerCommand_GetTitle(sub, arr, &t)) && t) {
            int w = _snwprintf(out + n, cap - n, L"%ls\"%ls\"", k++ ? L", " : L" > ", t);
            if (w > 0) n += (size_t)w;
            CoTaskMemFree(t);
        }
        IExplorerCommand_Release(sub);
    }
    IEnumExplorerCommand_Release(e);
    out[cap - 1] = 0;
}

static HRESULT run_command(IExplorerCommand *c, const req *r, const items *it, wchar_t *why, size_t wcap)
{
    IShellItemArray *arr = NULL;
    HRESULT hr = it->n ? SHCreateShellItemArray(NULL, it->parent, it->n, it->kids, &arr)
                       : SHCreateShellItemArrayFromIDLists(1, (PCIDLIST_ABSOLUTE_ARRAY)&it->folder, &arr);
    if (FAILED(hr)) {
        _snwprintf(why, wcap, L"the items could not be handed over (0x%08lX)", (unsigned long)hr);
        return hr;
    }
    /* What Explorer tells a registry verb's command before asking it anything:
     * its verb name (for verb requests) and the items. */
    IInitializeCommand *ic = NULL;
    if (SUCCEEDED(IExplorerCommand_QueryInterface(c, &IID_IInitializeCommand, (void **)&ic))) {
        IInitializeCommand_Initialize(ic, wcscmp(r->op, L"verb") ? L"" : r->name, NULL);
        IInitializeCommand_Release(ic);
    }
    IObjectWithSelection *ows = NULL;
    if (SUCCEEDED(IExplorerCommand_QueryInterface(c, &IID_IObjectWithSelection, (void **)&ows))) {
        IObjectWithSelection_SetSelection(ows, arr);
        IObjectWithSelection_Release(ows);
    }
    /* a command has no verbs: with verb, the handler's own command runs */
    IExplorerCommand *target = find_command(c, arr, wcscmp(r->op, L"verb") ? r->name : NULL, 1);
    if (!target) {
        wchar_t has[300];
        offered(c, arr, has, 300);
        _snwprintf(why, wcap, L"no \"%ls\" in that handler for these items (it offers %ls)", r->name, has[0] ? has : L"nothing");
        hr = E_FAIL;
    } else {
        EXPCMDSTATE st = ECS_ENABLED;
        /* hidden is Explorer's choice of what to show (some only with Shift); asked for by name, it runs */
        if (SUCCEEDED(IExplorerCommand_GetState(target, arr, TRUE, &st)) && (st & ECS_DISABLED)) {
            _snwprintf(why, wcap, L"\"%ls\" is greyed out for these items", r->name);
            hr = E_FAIL;
        } else {
            hr = IExplorerCommand_Invoke(target, arr, NULL);
            if (FAILED(hr)) _snwprintf(why, wcap, L"\"%ls\" did not run (0x%08lX)", r->name, (unsigned long)hr);
        }
        IExplorerCommand_Release(target);
    }
    IShellItemArray_Release(arr);
    return hr;
}

/* ---- one handler, or the whole menu ----------------------------------------------------------- */

static HRESULT run_item(const req *r, wchar_t *why, size_t wcap)
{
    items it;
    HRESULT hr = items_of(r, &it);
    if (FAILED(hr)) {
        _snwprintf(why, wcap, L"the items could not be found (0x%08lX)", (unsigned long)hr);
        items_free(&it);
        return hr;
    }
    IContextMenu *cm = NULL;
    int done = 0;
    if (r->handler && r->handler[0]) {
        CLSID clsid;
        IUnknown *unk = NULL;
        IExplorerCommand *ec = NULL;
        IShellExtInit *init = NULL;
        hr = CLSIDFromString(r->handler, &clsid);
        if (SUCCEEDED(hr))
            hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER, &IID_IUnknown, (void **)&unk);
        if (FAILED(hr)) {
            _snwprintf(why, wcap, L"the handler %ls could not be loaded (0x%08lX)", r->handler, (unsigned long)hr);
        } else if (SUCCEEDED(IUnknown_QueryInterface(unk, &IID_IExplorerCommand, (void **)&ec))) {
            hr = run_command(ec, r, &it, why, wcap);
            IExplorerCommand_Release(ec);
            done = 1;
        } else if (SUCCEEDED(hr = IUnknown_QueryInterface(unk, &IID_IShellExtInit, (void **)&init))) {
            IDataObject *obj = NULL;
            HKEY key = class_key(r);
            if (it.n) data_object(&it, &obj);
            hr = IShellExtInit_Initialize(init, it.folder, obj, key);
            if (key) RegCloseKey(key);
            if (obj) IDataObject_Release(obj);
            IShellExtInit_Release(init);
            if (SUCCEEDED(hr)) hr = IUnknown_QueryInterface(unk, &IID_IContextMenu, (void **)&cm);
            if (FAILED(hr)) _snwprintf(why, wcap, L"the handler %ls refused these items (0x%08lX)", r->handler, (unsigned long)hr);
        } else {
            _snwprintf(why, wcap, L"%ls is not a context menu handler", r->handler);
        }
        if (unk) IUnknown_Release(unk);
    } else if (it.n) {
        hr = IShellFolder_GetUIObjectOf(it.parent, S.owner, it.n, it.kids, &IID_IContextMenu, NULL, (void **)&cm);
    } else {
        IShellFolder *desk = NULL, *sf = NULL;
        hr = SHGetDesktopFolder(&desk);
        if (SUCCEEDED(hr)) {
            if (it.folder->mkid.cb == 0) sf = desk, IShellFolder_AddRef(sf);
            else hr = IShellFolder_BindToObject(desk, (PCUIDLIST_RELATIVE)it.folder, NULL, &IID_IShellFolder, (void **)&sf);
        }
        if (SUCCEEDED(hr)) hr = IShellFolder_CreateViewObject(sf, S.owner, &IID_IContextMenu, (void **)&cm);
        if (sf) IShellFolder_Release(sf);
        if (desk) IShellFolder_Release(desk);
    }
    if (cm) {
        hr = run_menu(cm, r, why, wcap);
        IContextMenu_Release(cm);
    } else if (!done && SUCCEEDED(hr) && !why[0]) {
        hr = E_FAIL;
        _snwprintf(why, wcap, L"Explorer's menu could not be built for these items");
    }
    items_free(&it);
    return hr;
}

/* ---- Send to --------------------------------------------------------------------------------- */

static HRESULT run_sendto(const req *r, wchar_t *why, size_t wcap)
{
    if (!r->n) {
        _snwprintf(why, wcap, L"Send to needs items");
        return E_INVALIDARG;
    }
    PWSTR dir = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_SendTo, 0, NULL, &dir))) {
        _snwprintf(why, wcap, L"there is no Send to folder");
        return E_FAIL;
    }
    wchar_t want[512], pattern[MAX_PATH + 4], target[MAX_PATH * 2] = L"";
    relay_norm(r->name ? r->name : L"", want, 512);
    _snwprintf(pattern, MAX_PATH + 4, L"%ls\\*", dir);
    pattern[MAX_PATH + 3] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pattern, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == L'.' || (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)) continue;
            wchar_t stem[MAX_PATH], norm[512];
            wcscpy(stem, fd.cFileName);
            wchar_t *dot = wcsrchr(stem, L'.');
            relay_norm(stem, norm, 512);
            int hit = !wcscmp(norm, want);
            if (!hit && dot) {
                *dot = 0;
                relay_norm(stem, norm, 512);
                hit = !wcscmp(norm, want);
            }
            if (hit) _snwprintf(target, MAX_PATH * 2, L"%ls\\%ls", dir, fd.cFileName);
        } while (!target[0] && FindNextFileW(f, &fd));
        FindClose(f);
    }
    CoTaskMemFree(dir);
    if (!target[0]) {
        _snwprintf(why, wcap, L"no \"%ls\" in Send to", r->name ? r->name : L"");
        return E_FAIL;
    }
    items it;
    IDataObject *obj = NULL;
    IDropTarget *dt = NULL;
    PIDLIST_ABSOLUTE tp = NULL;
    IShellFolder *tparent = NULL;
    PCUITEMID_CHILD tchild = NULL;
    HRESULT hr = items_of(r, &it);
    if (SUCCEEDED(hr)) hr = data_object(&it, &obj);
    if (SUCCEEDED(hr)) hr = SHParseDisplayName(target, NULL, &tp, 0, NULL);
    if (SUCCEEDED(hr)) hr = SHBindToParent(tp, &IID_IShellFolder, (void **)&tparent, &tchild);
    if (SUCCEEDED(hr)) hr = IShellFolder_GetUIObjectOf(tparent, S.owner, 1, &tchild, &IID_IDropTarget, NULL, (void **)&dt);
    if (FAILED(hr)) {
        _snwprintf(why, wcap, L"Send to \"%ls\" could not be reached (0x%08lX)", r->name, (unsigned long)hr);
    } else {
        POINTL pt = { r->pt.x, r->pt.y };
        DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
        hr = IDropTarget_DragEnter(dt, obj, MK_LBUTTON, pt, &effect);
        if (SUCCEEDED(hr) && effect) {
            hr = IDropTarget_Drop(dt, obj, MK_LBUTTON, pt, &effect);
            if (FAILED(hr)) _snwprintf(why, wcap, L"Send to \"%ls\" failed (0x%08lX)", r->name, (unsigned long)hr);
        } else {
            IDropTarget_DragLeave(dt);
            hr = E_FAIL;
            _snwprintf(why, wcap, L"\"%ls\" does not take these items", r->name);
        }
    }
    if (dt) IDropTarget_Release(dt);
    if (tparent) IShellFolder_Release(tparent);
    CoTaskMemFree(tp);
    if (obj) IDataObject_Release(obj);
    items_free(&it);
    return hr;
}

/* ---- the loop ------------------------------------------------------------------------------- */

static void run(req *r)
{
    wchar_t why[600] = L"";
    HRESULT hr;
    if (r->bad) {
        reply(L"malformed shell request: selected items were incomplete or invalid");
        S.last = GetTickCount64();
        return;
    }
    if (!r->name || !r->name[0]) {
        hr = E_INVALIDARG;
        _snwprintf(why, 600, L"nothing named to run");
    } else if (!wcscmp(r->op, L"sendto")) {
        hr = run_sendto(r, why, 600);
    } else if (!wcscmp(r->op, L"item") || !wcscmp(r->op, L"verb")) {
        hr = run_item(r, why, 600);
        /* Some handlers only work inside Explorer's window: Explorer's whole
         * menu (slower, every extension loads) may still have the item. */
        if (FAILED(hr) && r->handler && r->handler[0] && !r->no_fallback) {
            wchar_t *h = r->handler, why2[600] = L"";
            r->handler = NULL;
            if (SUCCEEDED(run_item(r, why2, 600))) hr = S_OK;
            r->handler = h;
        }
    } else {
        hr = E_INVALIDARG;
        _snwprintf(why, 600, L"unknown request \"%ls\"", r->op);
    }
    why[599] = 0;
    reply(SUCCEEDED(hr) ? NULL : why[0] ? why : L"it did not run");
    S.last = GetTickCount64();
}

static BOOL CALLBACK any_shown(HWND h, LPARAM l)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h)) {
        *(int *)l = 1;
        return FALSE;
    }
    return TRUE;
}

static void maybe_leave(void)
{
    if (!S.closing || S.refs > 0 || GetTickCount64() - S.last < QUIET_MS) return;
    int shown = 0;
    EnumWindows(any_shown, (LPARAM)&shown);
    if (!shown) PostQuitMessage(0);
}

static LRESULT CALLBACK inbox_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_REQ:
        run((req *)l);
        req_free((req *)l);
        return 0;
    case WM_EOF:
        S.closing = 1;
        SetTimer(h, TIMER_EXIT, 250, NULL);
        maybe_leave();
        return 0;
    case WM_TIMER:
    case WM_NULL:
        maybe_leave();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(OleInitialize(NULL))) return 1;
    S.out = GetStdHandle(STD_OUTPUT_HANDLE);
    S.last = GetTickCount64();
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = inbox_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"fpshell";
    RegisterClassW(&wc);
    S.inbox = CreateWindowExW(0, L"fpshell", L"fpshell", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, inst, NULL);
    /* dialogs need an owner that can own them: a window never shown */
    S.owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"OpenPilot", WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!S.inbox) return 1;
    SHSetInstanceExplorer(&proc_ref);
    HANDLE t = CreateThread(NULL, 64 * 1024, reader, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t) return 1;
    CloseHandle(t);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    SHSetInstanceExplorer(NULL);
    OleUninitialize();
    return 0;
}
