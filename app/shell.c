/* Shell integration: see shell.h. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#include "fpe.h"
#include "shell.h"
#include "termutil.h"

/* Not every MinGW import library carries this one. */
static const IID iid_image_list = { 0x46eb5926, 0x582e, 0x4017, { 0x9f, 0xdf, 0xe8, 0x99, 0x8d, 0xaa, 0x09, 0x50 } };

#define ICON_SLOTS 4096 /* power of two */
#define ICON_MAX   3000 /* cleared when reached, before probing gets slow */
#define QUEUE_CAP  256
#define KEY_CAP    1024
#define ICON_PENDING (-2)
#define ICON_FAILED  (-3)

#ifndef FILE_ATTRIBUTE_RECALL_ON_OPEN
#define FILE_ATTRIBUTE_RECALL_ON_OPEN 0x00040000
#endif
#ifndef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
#define FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS 0x00400000
#endif
#define CLOUD_ATTRS (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)

/* One cache for every icon, keyed by a lowercase string: "*.ext" for a file
 * type, "*" for files without an extension, "*\" for folders, or a full path
 * for an item with its own icon. Paths never start with '*'. */
typedef struct icon_slot {
    wchar_t *key; /* NULL when free */
    int      idx;
} icon_slot;

static struct {
    int           ready;       /* OLE is up on the UI thread: menus, drag and drop */
    volatile LONG icons_ready; /* the worker has the system image list */
    HWND          notify;
    UINT          msg;
    IImageList   *il;
    int           icon_size, folder_icon;
    IDropTargetHelper *drop_helper;
    int           drop_helper_tried;

    SRWLOCK    lock; /* guards slots + queue, shared with the worker */
    icon_slot *slots;
    uint32_t   nslots;
    wchar_t   *queue[QUEUE_CAP];
    uint32_t   qhead, qcount;
    HANDLE     wake, thread;
    volatile LONG stop;

    IContextMenu2 *menu2; /* set while a context menu is open */
    IContextMenu3 *menu3;
} sh = { .lock = SRWLOCK_INIT, .folder_icon = -1 };

static fpe_gauge g_icons = FPE_GAUGE_INIT("icon.cache");

int shell_ready(void) { return sh.ready; }
int shell_icon_size(void) { return sh.icon_size; }

static uint32_t hash_w(const wchar_t *s)
{
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint32_t)*s++) * 16777619u;
    return h;
}

/* ---- icon cache ---------------------------------------------------------------------- */

static icon_slot *slot_find(const wchar_t *key, uint32_t *free_at)
{
    uint32_t i = hash_w(key) & (ICON_SLOTS - 1);
    while (sh.slots[i].key) {
        if (!wcscmp(sh.slots[i].key, key)) return &sh.slots[i];
        i = (i + 1) & (ICON_SLOTS - 1);
    }
    if (free_at) *free_at = i;
    return NULL;
}

static void slots_clear(void)
{
    for (uint32_t i = 0; i < ICON_SLOTS; i++) {
        free(sh.slots[i].key);
        sh.slots[i].key = NULL;
    }
    fpe_gauge_add(&g_icons, -(int64_t)sh.nslots);
    sh.nslots = 0;
}

/* The cached icon for `key`, or -1 while the worker resolves it (or when
 * there is none). Never calls into the shell: painting only reads. */
static int lookup(const wchar_t *key)
{
    int result = -1;
    AcquireSRWLockExclusive(&sh.lock);
    uint32_t at = 0;
    icon_slot *s = slot_find(key, &at);
    if (s) {
        result = s->idx >= 0 ? s->idx : -1;
    } else if (sh.qcount < QUEUE_CAP) { /* when full, the next paint asks again */
        if (sh.nslots >= ICON_MAX) {
            slots_clear();
            slot_find(key, &at);
        }
        size_t bytes = (wcslen(key) + 1) * sizeof(wchar_t);
        wchar_t *k = malloc(bytes), *job = malloc(bytes);
        if (k && job) {
            memcpy(k, key, bytes);
            memcpy(job, key, bytes);
            sh.slots[at].key = k;
            sh.slots[at].idx = ICON_PENDING;
            sh.nslots++;
            fpe_gauge_add(&g_icons, 1);
            sh.queue[(sh.qhead + sh.qcount) % QUEUE_CAP] = job;
            sh.qcount++;
            SetEvent(sh.wake);
        } else {
            free(k);
            free(job);
        }
    }
    ReleaseSRWLockExclusive(&sh.lock);
    return result;
}

static int resolve(const wchar_t *key)
{
    SHFILEINFOW sfi = { 0 };
    int64_t t = fpe_prof_begin();
    int idx;
    if (key[0] == L'*') {
        wchar_t probe[24];
        DWORD attrs = FILE_ATTRIBUTE_NORMAL;
        if (key[1] == L'\\') {
            wcscpy(probe, L"folder");
            attrs = FILE_ATTRIBUTE_DIRECTORY;
        } else if (!key[1]) {
            wcscpy(probe, L"file");
        } else {
            _snwprintf(probe, 24, L"file%ls", key + 1);
            probe[23] = 0;
        }
        idx = SHGetFileInfoW(probe, attrs, &sfi, sizeof sfi, SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON)
                  ? sfi.iIcon
                  : ICON_FAILED;
        FPE_PROF_END("icon.type", t);
    } else {
        idx = SHGetFileInfoW(key, 0, &sfi, sizeof sfi, SHGFI_SYSICONINDEX | SHGFI_SMALLICON) ? sfi.iIcon : ICON_FAILED;
        FPE_PROF_END("icon.file", t);
    }
    return idx;
}

static DWORD WINAPI icon_worker(void *arg)
{
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    /* Building the system image list takes ~15 ms: done here, the UI thread
     * never waits for it. The list itself can be drawn from any thread. */
    int64_t t = fpe_prof_begin();
    if (SUCCEEDED(SHGetImageList(SHIL_SMALL, &iid_image_list, (void **)&sh.il))) {
        int cx = 16, cy = 16;
        IImageList_GetIconSize(sh.il, &cx, &cy);
        sh.icon_size = cx;
        lookup(L"*\\"); /* the folder icon is needed first, everywhere */
        InterlockedExchange(&sh.icons_ready, 1);
    }
    FPE_PROF_END("icon.image_list", t);
    while (!sh.stop) {
        WaitForSingleObject(sh.wake, INFINITE);
        int done = 0;
        while (!sh.stop) {
            AcquireSRWLockExclusive(&sh.lock);
            wchar_t *key = NULL;
            if (sh.qcount) {
                key = sh.queue[sh.qhead];
                sh.qhead = (sh.qhead + 1) % QUEUE_CAP;
                sh.qcount--;
            }
            ReleaseSRWLockExclusive(&sh.lock);
            if (!key) break;

            int idx = resolve(key);
            AcquireSRWLockExclusive(&sh.lock);
            icon_slot *s = slot_find(key, NULL);
            if (s) s->idx = idx;
            ReleaseSRWLockExclusive(&sh.lock);
            free(key);
            /* show progress on long lists without a message per icon */
            if (++done % 16 == 0) PostMessageW(sh.notify, sh.msg, 0, 0);
        }
        if (done % 16) PostMessageW(sh.notify, sh.msg, 0, 0);
    }
    CoUninitialize();
    return 0;
}

static int wants_own_icon(const wchar_t *ext, uint32_t len)
{
    static const wchar_t *own[] = { L"exe", L"lnk", L"ico", L"url", L"cpl", L"scr", L"msc", L"cur", L"ani", L"appref-ms" };
    if (!len || len > 9) return 0;
    for (size_t i = 0; i < sizeof own / sizeof own[0]; i++)
        if (wcslen(own[i]) == len && !_wcsnicmp(ext, own[i], len)) return 1;
    return 0;
}

int shell_icon_folder(void)
{
    if (!sh.icons_ready) return -1;
    if (sh.folder_icon < 0) sh.folder_icon = lookup(L"*\\");
    return sh.folder_icon;
}

int shell_icon_entry(const wchar_t *dir, const wchar_t *name, uint32_t name_len, uint32_t ext, uint32_t attrs)
{
    if (!sh.icons_ready) return -1;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) return shell_icon_folder();
    const wchar_t *e = name + ext;
    uint32_t el = name_len > ext ? name_len - ext : 0;

    wchar_t key[KEY_CAP];
    key[0] = L'*';
    if (!el || el > 15) {
        key[1] = 0;
    } else {
        key[1] = L'.';
        for (uint32_t i = 0; i < el; i++) key[2 + i] = towlower(e[i]);
        key[2 + el] = 0;
    }
    int type = lookup(key);
    if (!wants_own_icon(e, el) || (attrs & CLOUD_ATTRS) || !dir) return type;

    size_t dl = wcslen(dir), k = 0;
    if (dl + 1 + name_len + 1 > KEY_CAP) return type;
    for (size_t i = 0; i < dl; i++) key[k++] = towlower(dir[i]);
    if (dl && dir[dl - 1] != L'\\') key[k++] = L'\\';
    for (uint32_t i = 0; i < name_len; i++) key[k++] = towlower(name[i]);
    key[k] = 0;
    if(fpe_wsl_requires_native(key))return type;
    int own = lookup(key);
    return own >= 0 ? own : type;
}

int shell_icon_path(const wchar_t *path)
{
    if (!sh.icons_ready || !path[0] || path[0] == L'*'||fpe_wsl_requires_native(path)) return -1;
    wchar_t key[KEY_CAP];
    size_t n = wcslen(path);
    if (n >= KEY_CAP) return -1;
    for (size_t i = 0; i <= n; i++) key[i] = towlower(path[i]);
    return lookup(key);
}

int shell_icon_stock(int siid)
{
    if (!sh.icons_ready) return -1;
    SHSTOCKICONINFO si = { sizeof si };
    return SUCCEEDED(SHGetStockIconInfo((SHSTOCKICONID)siid, SHGSI_SYSICONINDEX, &si)) ? si.iSysImageIndex : -1;
}

void shell_icon_draw(HDC dc, int index, int x, int y, int faded)
{
    if (!sh.icons_ready || index < 0) return;
    IMAGELISTDRAWPARAMS p = { sizeof p };
    p.himl = (HIMAGELIST)sh.il;
    p.i = index;
    p.hdcDst = dc;
    p.x = x;
    p.y = y;
    p.rgbBk = CLR_NONE;
    p.rgbFg = CLR_NONE;
    p.fStyle = ILD_TRANSPARENT;
    if (faded) {
        p.fState = ILS_ALPHA;
        p.Frame = 110;
    }
    IImageList_Draw(sh.il, &p);
}

/* ---- init ----------------------------------------------------------------------------- */

int shell_init(void)
{
    if (sh.ready) return 0;
    int64_t t = fpe_prof_begin();
    if (FAILED(OleInitialize(NULL))) return -1;
    sh.ready = 1;
    FPE_PROF_END("shell.init", t);
    return 0;
}

void shell_shutdown(void)
{
    if (!sh.ready) return;
    sh.ready = 0;
    if (sh.drop_helper) IDropTargetHelper_Release(sh.drop_helper);
    sh.drop_helper = NULL;
    sh.drop_helper_tried = 0;
    OleUninitialize();
}

int shell_icons_start(HWND notify, UINT msg)
{
    if (sh.thread) return 0;
    int64_t t = fpe_prof_begin();
    if (!sh.slots) sh.slots = calloc(ICON_SLOTS, sizeof *sh.slots);
    if (!sh.wake) sh.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!sh.slots || !sh.wake) return -1;
    sh.notify = notify;
    sh.msg = msg;
    InterlockedExchange(&sh.stop, 0);
    sh.thread = CreateThread(NULL, 0, icon_worker, NULL, 0, NULL);
    FPE_PROF_END("icon.start", t);
    return sh.thread ? 0 : -1;
}

void shell_icons_stop(void)
{
    if (!sh.thread) return;
    InterlockedExchange(&sh.icons_ready, 0);
    InterlockedExchange(&sh.stop, 1);
    SetEvent(sh.wake);
    /* A slow network icon must not hold up exit: the worker is left to
     * finish on its own and its cache with it. */
    if (WaitForSingleObject(sh.thread, 500) != WAIT_OBJECT_0) {
        CloseHandle(sh.thread);
        sh.thread = NULL;
        return;
    }
    CloseHandle(sh.thread);
    sh.thread = NULL;
    AcquireSRWLockExclusive(&sh.lock);
    while (sh.qcount) {
        free(sh.queue[sh.qhead]);
        sh.qhead = (sh.qhead + 1) % QUEUE_CAP;
        sh.qcount--;
    }
    slots_clear();
    ReleaseSRWLockExclusive(&sh.lock);
    free(sh.slots);
    sh.slots = NULL;
    sh.folder_icon = -1;
    if (sh.il) IImageList_Release(sh.il);
    sh.il = NULL;
}

/* ---- shell objects for paths -------------------------------------------------------------- */

/* GetUIObjectOf for items that share one parent folder. */
static HRESULT ui_object(HWND owner, const wchar_t *const *paths, uint32_t n, REFIID riid, void **out)
{
    *out = NULL;
    if (!n) return E_INVALIDARG;
    PIDLIST_ABSOLUTE *abs = calloc(n, sizeof *abs);
    PCUITEMID_CHILD *kids = calloc(n, sizeof *kids);
    IShellFolder *sf = NULL;
    HRESULT hr = abs && kids ? S_OK : E_OUTOFMEMORY;
    for (uint32_t i = 0; SUCCEEDED(hr) && i < n; i++) {
        if(fpe_wsl_requires_native(paths[i])){hr=HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);break;}
        hr = SHParseDisplayName(paths[i], NULL, &abs[i], 0, NULL);
        if (SUCCEEDED(hr)) kids[i] = (PCUITEMID_CHILD)ILFindLastID((PCUIDLIST_RELATIVE)abs[i]);
    }
    if (SUCCEEDED(hr)) hr = SHBindToParent(abs[0], &IID_IShellFolder, (void **)&sf, NULL);
    if (SUCCEEDED(hr)) hr = IShellFolder_GetUIObjectOf(sf, owner, n, kids, riid, NULL, out);
    if (sf) IShellFolder_Release(sf);
    for (uint32_t i = 0; abs && i < n; i++) CoTaskMemFree(abs[i]);
    free(abs);
    free(kids);
    return SUCCEEDED(hr) && *out ? S_OK : E_FAIL;
}

/* The menu for a folder's empty space ("background"): New, Paste, Properties... */
static HRESULT folder_menu(HWND owner, const wchar_t *dir, IContextMenu **out)
{
    *out = NULL;
    PIDLIST_ABSOLUTE pidl = NULL;
    IShellFolder *desk = NULL, *sf = NULL;
    if(fpe_wsl_requires_native(dir))return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    HRESULT hr = SHParseDisplayName(dir, NULL, &pidl, 0, NULL);
    if (SUCCEEDED(hr)) hr = SHGetDesktopFolder(&desk);
    if (SUCCEEDED(hr)) {
        if (pidl->mkid.cb == 0) {
            sf = desk;
            IShellFolder_AddRef(sf);
        } else {
            hr = IShellFolder_BindToObject(desk, (PCUIDLIST_RELATIVE)pidl, NULL, &IID_IShellFolder, (void **)&sf);
        }
    }
    if (SUCCEEDED(hr)) hr = IShellFolder_CreateViewObject(sf, owner, &IID_IContextMenu, (void **)out);
    if (sf) IShellFolder_Release(sf);
    if (desk) IShellFolder_Release(desk);
    CoTaskMemFree(pidl);
    return SUCCEEDED(hr) && *out ? S_OK : E_FAIL;
}

static int word_in(const char *list, const char *word)
{
    size_t n = strlen(word);
    if (!list || !n) return 0;
    for (const char *s = list; *s;) {
        while (*s == ' ') s++;
        const char *e = s;
        while (*e && *e != ' ') e++;
        if ((size_t)(e - s) == n && !_strnicmp(s, word, n)) return 1;
        s = e;
    }
    return 0;
}

static HRESULT invoke(IContextMenu *cm, HWND owner, const wchar_t *dir, UINT offset, const char *verb, POINT pt)
{
    wchar_t wverb[64] = L"";
    if (verb) MultiByteToWideChar(CP_ACP, 0, verb, -1, wverb, 64);
    CMINVOKECOMMANDINFOEX ici = { sizeof ici };
    ici.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
    if (GetKeyState(VK_CONTROL) < 0) ici.fMask |= CMIC_MASK_CONTROL_DOWN;
    if (GetKeyState(VK_SHIFT) < 0) ici.fMask |= CMIC_MASK_SHIFT_DOWN;
    ici.hwnd = owner;
    ici.lpVerb = verb ? verb : MAKEINTRESOURCEA(offset);
    ici.lpVerbW = verb ? wverb : MAKEINTRESOURCEW(offset);
    ici.lpDirectoryW = dir;
    ici.nShow = SW_SHOWNORMAL;
    ici.ptInvoke = pt;
    int64_t t = fpe_prof_begin();
    HRESULT hr = IContextMenu_InvokeCommand(cm, (CMINVOKECOMMANDINFO *)&ici);
    FPE_PROF_END("shell.invoke", t);
    return hr;
}

UINT shell_context_menu(HWND owner, HMENU menu, const wchar_t *dir, const wchar_t *const *paths, uint32_t n, POINT screen,
                        int extended, const char *intercept, char *verb_out, size_t verb_cap)
{
    if (verb_out && verb_cap) verb_out[0] = 0;
    if (!sh.ready) return 0;
    IContextMenu *cm = NULL;
    int64_t t = fpe_prof_begin();
    if (n) ui_object(owner, paths, n, &IID_IContextMenu, (void **)&cm);
    else if (dir) folder_menu(owner, dir, &cm);
    if (cm) {
        if (GetMenuItemCount(menu) > 0) AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        UINT flags = CMF_NORMAL | CMF_CANRENAME | (extended ? CMF_EXTENDEDVERBS : 0);
        if (FAILED(IContextMenu_QueryContextMenu(cm, menu, (UINT)GetMenuItemCount(menu), SHELL_CMD_FIRST, 0x7FFF, flags))) {
            IContextMenu_Release(cm);
            cm = NULL;
        } else if (FAILED(IContextMenu_QueryInterface(cm, &IID_IContextMenu3, (void **)&sh.menu3))) {
            IContextMenu_QueryInterface(cm, &IID_IContextMenu2, (void **)&sh.menu2);
        }
    }
    FPE_PROF_END("shell.menu_build", t);

    UINT cmd = (UINT)TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, owner, NULL);
    if (sh.menu3) IContextMenu3_Release(sh.menu3);
    if (sh.menu2) IContextMenu2_Release(sh.menu2);
    sh.menu3 = NULL;
    sh.menu2 = NULL;

    UINT result = 0;
    if (cmd && cmd < SHELL_CMD_FIRST) {
        result = cmd;
    } else if (cmd >= SHELL_CMD_FIRST && cm) {
        wchar_t wv[64] = L"";
        char verb[64] = "";
        if (SUCCEEDED(IContextMenu_GetCommandString(cm, cmd - SHELL_CMD_FIRST, GCS_VERBW, NULL, (LPSTR)wv, 64)))
            WideCharToMultiByte(CP_ACP, 0, wv, -1, verb, sizeof verb, NULL, NULL);
        if (verb[0] && word_in(intercept, verb)) {
            if (verb_out && verb_cap) {
                strncpy(verb_out, verb, verb_cap - 1);
                verb_out[verb_cap - 1] = 0;
            }
        } else {
            invoke(cm, owner, dir, cmd - SHELL_CMD_FIRST, NULL, screen);
        }
    }
    if (cm) IContextMenu_Release(cm);
    return result;
}

int shell_menu_msg(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *result)
{
    if (!sh.menu3 && !sh.menu2) return 0;
    switch (m) {
    case WM_INITMENUPOPUP:
    case WM_MENUCHAR:
        break;
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
        if (w) return 0; /* not from a menu */
        break;
    default:
        return 0;
    }
    if (sh.menu3) {
        LRESULT r = 0;
        if (FAILED(IContextMenu3_HandleMenuMsg2(sh.menu3, m, w, l, &r))) return 0;
        *result = r;
        return 1;
    }
    if (FAILED(IContextMenu2_HandleMenuMsg(sh.menu2, m, w, l))) return 0;
    *result = m == WM_INITMENUPOPUP ? 0 : TRUE;
    return 1;
}

int shell_invoke_verb(HWND owner, const wchar_t *dir, const wchar_t *const *paths, uint32_t n, const char *verb)
{
    if (!sh.ready) return -1;
    IContextMenu *cm = NULL;
    if (FAILED(n ? ui_object(owner, paths, n, &IID_IContextMenu, (void **)&cm) : folder_menu(owner, dir, &cm))) return -1;
    /* Many handlers only accept string verbs after building their menu. */
    HMENU menu = CreatePopupMenu();
    IContextMenu_QueryContextMenu(cm, menu, 0, SHELL_CMD_FIRST, 0x7FFF, CMF_NORMAL);
    POINT pt = { 0, 0 };
    HRESULT hr = invoke(cm, owner, dir, 0, verb, pt);
    DestroyMenu(menu);
    IContextMenu_Release(cm);
    return SUCCEEDED(hr) ? 0 : -1;
}

int shell_properties(HWND owner, const wchar_t *dir, const wchar_t *const *paths, uint32_t n)
{
    if (!sh.ready) return -1;
    if (n <= 1) {
        const wchar_t *target = n ? paths[0] : dir;
        return SHObjectProperties(owner, SHOP_FILEPATH, target, NULL) ? 0 : -1;
    }
    IDataObject *obj = NULL;
    if (FAILED(ui_object(owner, paths, n, &IID_IDataObject, (void **)&obj))) return -1;
    HRESULT hr = SHMultiFileProperties(obj, 0);
    IDataObject_Release(obj);
    return SUCCEEDED(hr) ? 0 : -1;
}

/* ---- drag and drop ----------------------------------------------------------------------------- */

typedef struct drop_target {
    IDropTarget               iface; /* first: the interface pointer is the object */
    LONG                      ref;
    HWND                      hwnd;
    const shell_drop_handler *h;
    void                     *ctx;
    wchar_t                 **paths;
    uint32_t                  n;
    int                       right_button;
} drop_target;

static void dt_clear(drop_target *d)
{
    for (uint32_t i = 0; i < d->n; i++) free(d->paths[i]);
    free(d->paths);
    d->paths = NULL;
    d->n = 0;
    d->right_button = 0;
}

/* Only real files are accepted (CF_HDROP); virtual items like mail
 * attachments are refused rather than half-supported. */
static void dt_load(drop_target *d, IDataObject *obj)
{
    dt_clear(d);
    FORMATETC fe = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM sm;
    if (FAILED(IDataObject_GetData(obj, &fe, &sm))) return;
    HDROP drop = GlobalLock(sm.hGlobal);
    if (drop) {
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
        d->paths = n ? calloc(n, sizeof *d->paths) : NULL;
        for (UINT i = 0; d->paths && i < n; i++) {
            UINT len = DragQueryFileW(drop, i, NULL, 0);
            wchar_t *s = malloc((len + 1) * sizeof(wchar_t));
            if (s && DragQueryFileW(drop, i, s, len + 1)) d->paths[d->n++] = s;
            else free(s);
        }
        GlobalUnlock(sm.hGlobal);
    }
    ReleaseStgMedium(&sm);
}

static POINT dt_client(drop_target *d, POINTL pt)
{
    POINT p = { pt.x, pt.y };
    ScreenToClient(d->hwnd, &p);
    return p;
}

static IDropTargetHelper *drop_helper(void)
{
    if (!sh.drop_helper_tried) {
        sh.drop_helper_tried = 1;
        CoCreateInstance(&CLSID_DragDropHelper, NULL, CLSCTX_INPROC_SERVER, &IID_IDropTargetHelper,
                         (void **)&sh.drop_helper);
    }
    return sh.drop_helper;
}

static HRESULT STDMETHODCALLTYPE dt_qi(IDropTarget *self, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropTarget)) {
        *out = self;
        InterlockedIncrement(&((drop_target *)self)->ref);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dt_addref(IDropTarget *self) { return (ULONG)InterlockedIncrement(&((drop_target *)self)->ref); }

static ULONG STDMETHODCALLTYPE dt_release(IDropTarget *self)
{
    drop_target *d = (drop_target *)self;
    LONG r = InterlockedDecrement(&d->ref);
    if (!r) {
        dt_clear(d);
        free(d);
    }
    return (ULONG)r;
}

static DWORD dt_effect(drop_target *d, POINTL pt, DWORD keys, DWORD allowed)
{
    if (keys & MK_RBUTTON) d->right_button = 1;
    if (d->right_button) keys |= MK_RBUTTON;
    if (!d->n) return DROPEFFECT_NONE;
    return d->h->over(d->ctx, dt_client(d, pt), keys, allowed, (const wchar_t *const *)d->paths, d->n);
}

static HRESULT STDMETHODCALLTYPE dt_enter(IDropTarget *self, IDataObject *obj, DWORD keys, POINTL pt, DWORD *effect)
{
    drop_target *d = (drop_target *)self;
    dt_load(d, obj);
    *effect = dt_effect(d, pt, keys, *effect);
    POINT p = { pt.x, pt.y };
    if (drop_helper()) IDropTargetHelper_DragEnter(sh.drop_helper, d->hwnd, obj, &p, *effect);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dt_over(IDropTarget *self, DWORD keys, POINTL pt, DWORD *effect)
{
    drop_target *d = (drop_target *)self;
    *effect = dt_effect(d, pt, keys, *effect);
    POINT p = { pt.x, pt.y };
    if (sh.drop_helper) IDropTargetHelper_DragOver(sh.drop_helper, &p, *effect);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dt_leave(IDropTarget *self)
{
    drop_target *d = (drop_target *)self;
    if (sh.drop_helper) IDropTargetHelper_DragLeave(sh.drop_helper);
    d->h->leave(d->ctx);
    dt_clear(d);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dt_drop(IDropTarget *self, IDataObject *obj, DWORD keys, POINTL pt, DWORD *effect)
{
    drop_target *d = (drop_target *)self;
    DWORD allowed = *effect & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
    DWORD e = dt_effect(d, pt, keys, allowed);
    if (d->right_button) keys |= MK_RBUTTON;
    POINT p = { pt.x, pt.y };
    if (sh.drop_helper) IDropTargetHelper_Drop(sh.drop_helper, obj, &p, e);
    d->h->leave(d->ctx);
    if (e) e = d->h->drop(d->ctx, dt_client(d, pt), e, keys, allowed,
                          (const wchar_t *const *)d->paths, d->n) & allowed;
    /* The move runs in our own job queue. Reporting MOVE would tell the
     * source to delete the originals itself, possibly before we copied them. */
    *effect = e == DROPEFFECT_MOVE ? DROPEFFECT_NONE : e;
    dt_clear(d);
    return S_OK;
}

static IDropTargetVtbl dt_vtbl = { dt_qi, dt_addref, dt_release, dt_enter, dt_over, dt_leave, dt_drop };

int shell_drop_register(HWND h, const shell_drop_handler *handler, void *ctx)
{
    if (!sh.ready) return -1;
    drop_target *d = calloc(1, sizeof *d);
    if (!d) return -1;
    d->iface.lpVtbl = &dt_vtbl;
    d->ref = 1;
    d->hwnd = h;
    d->h = handler;
    d->ctx = ctx;
    HRESULT hr = RegisterDragDrop(h, &d->iface);
    dt_release(&d->iface); /* the registration holds its own reference */
    return SUCCEEDED(hr) ? 0 : -1;
}

void shell_drop_revoke(HWND h) { RevokeDragDrop(h); }

/* "C:" or "\\server\share": the part that decides whether a move is a rename. */
static size_t volume_len(const wchar_t *p)
{
    if (p[0] && p[1] == L':') return 2;
    if (p[0] == L'\\' && p[1] == L'\\') {
        const wchar_t *s = wcschr(p + 2, L'\\');
        if (!s) return wcslen(p);
        const wchar_t *s2 = wcschr(s + 1, L'\\');
        return s2 ? (size_t)(s2 - p) : wcslen(p);
    }
    return 0;
}

static int path_prefix_equal(const wchar_t *a,const wchar_t *b,size_t n)
{
    if(wcslen(a)<n||wcslen(b)<n)return 0;
    wchar_t *x=malloc((n+1)*sizeof(wchar_t)),*y=malloc((n+1)*sizeof(wchar_t));
    if(!x||!y){free(x);free(y);return 0;}
    memcpy(x,a,n*sizeof(wchar_t));memcpy(y,b,n*sizeof(wchar_t));x[n]=y[n]=0;
    int equal=fpe_path_equal(x,y);free(x);free(y);return equal;
}
static int same_dir(const wchar_t *path,const wchar_t *dir)
{
    const wchar_t *slash=wcsrchr(path,L'\\');if(!slash)return 0;
    size_t pl=(size_t)(slash-path),dl=wcslen(dir);
    if(pl==2&&path[1]==L':')pl=3;
    while(dl>3&&dir[dl-1]==L'\\')dl--;
    return pl==dl&&path_prefix_equal(path,dir,pl);
}

DWORD shell_drop_effect(const wchar_t *const *paths, uint32_t n, const wchar_t *dest, DWORD keys, DWORD allowed)
{
    if (!dest || !n) return DROPEFFECT_NONE;
    size_t dv = volume_len(dest), dl = wcslen(dest);
    int all_here = 1, one_volume = 1;
    for (uint32_t i = 0; i < n; i++) {
        const wchar_t *p = paths[i];
        size_t pl = wcslen(p);
        if (pl == dl && fpe_path_equal(p, dest)) return DROPEFFECT_NONE; /* onto itself */
        if (dl > pl && path_prefix_equal(dest,p,pl) && dest[pl] == L'\\') return DROPEFFECT_NONE; /* into itself */
        if (!same_dir(p, dest)) all_here = 0;
        size_t pv = volume_len(p);
        if (!pv || pv != dv || _wcsnicmp(p, dest, pv)) one_volume = 0;
    }
    DWORD want;
    if (keys & MK_CONTROL) want = DROPEFFECT_COPY;
    else if (keys & MK_SHIFT) want = DROPEFFECT_MOVE;
    else want = one_volume ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
    if (!(allowed & want)) want = (allowed & DROPEFFECT_COPY) ? DROPEFFECT_COPY : (allowed & DROPEFFECT_MOVE) ? DROPEFFECT_MOVE : 0;
    /* Dropping back where the items already are does nothing, unless Ctrl
     * explicitly asks for duplicates. */
    if (all_here && !(want == DROPEFFECT_COPY && (keys & MK_CONTROL))) return DROPEFFECT_NONE;
    return want;
}

DWORD shell_drop_choices(const wchar_t *const *paths, uint32_t n, const wchar_t *dest, DWORD allowed)
{
    DWORD result = 0;
    if (shell_drop_effect(paths,n,dest,MK_CONTROL,allowed & DROPEFFECT_COPY)) result |= DROPEFFECT_COPY;
    if (shell_drop_effect(paths,n,dest,MK_SHIFT,allowed & DROPEFFECT_MOVE)) result |= DROPEFFECT_MOVE;
    /* LINK has the same path guards as an explicit copy, but never removes
     * an original and is also useful when dropped back in its current folder. */
    if ((allowed & DROPEFFECT_LINK) && shell_drop_effect(paths,n,dest,MK_CONTROL,DROPEFFECT_COPY))
        result |= DROPEFFECT_LINK;
    return result;
}
HRESULT shell_drag_continue(int escape, DWORD keys, int right_button)
{
    DWORD button = right_button ? MK_RBUTTON : MK_LBUTTON;
    DWORD other = right_button ? MK_LBUTTON : MK_RBUTTON;
    if (escape || (keys & other)) return DRAGDROP_S_CANCEL;
    return (keys & button) ? S_OK : DRAGDROP_S_DROP;
}

typedef struct drag_source { IDropSource iface; LONG ref; int right; } drag_source;
static HRESULT STDMETHODCALLTYPE ds_qi(IDropSource *self, REFIID iid, void **out)
{
    if (!IsEqualIID(iid,&IID_IUnknown) && !IsEqualIID(iid,&IID_IDropSource)) { *out=NULL;return E_NOINTERFACE; }
    *out=self;InterlockedIncrement(&((drag_source *)self)->ref);return S_OK;
}
static ULONG STDMETHODCALLTYPE ds_addref(IDropSource *self) { return InterlockedIncrement(&((drag_source *)self)->ref); }
static ULONG STDMETHODCALLTYPE ds_release(IDropSource *self)
{
    drag_source *d=(drag_source *)self;LONG ref=InterlockedDecrement(&d->ref);
    if(!ref)free(d);return ref;
}
static HRESULT STDMETHODCALLTYPE ds_continue(IDropSource *self, BOOL escape, DWORD keys)
{
    return shell_drag_continue(escape,keys,((drag_source *)self)->right);
}
static HRESULT STDMETHODCALLTYPE ds_feedback(IDropSource *self, DWORD effect) { return DRAGDROP_S_USEDEFAULTCURSORS; }
static IDropSourceVtbl ds_vtbl={ds_qi,ds_addref,ds_release,ds_continue,ds_feedback};
DWORD shell_drag_out(HWND h, const wchar_t *const *paths, uint32_t n, int right_button)
{
    if (!sh.ready || !n) return DROPEFFECT_NONE;
    IDataObject *obj = NULL;
    if (FAILED(ui_object(h, paths, n, &IID_IDataObject, (void **)&obj))) return DROPEFFECT_NONE;
    drag_source *source=calloc(1,sizeof *source);
    if(!source){IDataObject_Release(obj);return DROPEFFECT_NONE;}
    source->iface.lpVtbl=&ds_vtbl;source->ref=1;source->right=right_button;
    DWORD effect = DROPEFFECT_NONE;
    SHDoDragDrop(h, obj, &source->iface, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
    ds_release(&source->iface);IDataObject_Release(obj);
    return effect;
}

DWORD shell_create_shortcut(const wchar_t *source, const wchar_t *output)
{
    if(fpe_wsl_requires_native(source)||fpe_wsl_requires_native(output))return ERROR_NOT_SUPPORTED;
    HRESULT init=CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    if(FAILED(init) && init!=RPC_E_CHANGED_MODE)return HRESULT_CODE(init);
    IShellLinkW *link=NULL;IPersistFile *persist=NULL;
    HRESULT hr=CoCreateInstance(&CLSID_ShellLink,NULL,CLSCTX_INPROC_SERVER,&IID_IShellLinkW,(void **)&link);
    wchar_t *tmp=NULL;DWORD err=0;HANDLE reserved=INVALID_HANDLE_VALUE;
    if(SUCCEEDED(hr))hr=IShellLinkW_SetPath(link,source);
    if(SUCCEEDED(hr))hr=IShellLinkW_QueryInterface(link,&IID_IPersistFile,(void **)&persist);
    if(SUCCEEDED(hr)){
        size_t chars=wcslen(output)+80;tmp=malloc(chars*sizeof *tmp);
        if(!tmp)err=ERROR_NOT_ENOUGH_MEMORY;
        else{
            static LONG serial;
            _snwprintf(tmp,chars,L"%ls.%lu.%ld.tmp",output,GetCurrentProcessId(),InterlockedIncrement(&serial));
            reserved=CreateFileW(tmp,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
            if(reserved==INVALID_HANDLE_VALUE)err=GetLastError();
            else{CloseHandle(reserved);hr=IPersistFile_Save(persist,tmp,TRUE);
                if(SUCCEEDED(hr) && !MoveFileExW(tmp,output,0))err=GetLastError();
                if(FAILED(hr)||err)DeleteFileW(tmp);
            }
        }
    }
    if(!err && FAILED(hr))err=HRESULT_CODE(hr)?HRESULT_CODE(hr):ERROR_GEN_FAILURE;
    if(persist)IPersistFile_Release(persist);if(link)IShellLinkW_Release(link);
    free(tmp);if(SUCCEEDED(init))CoUninitialize();return err;
}

/* ---- small actions ------------------------------------------------------------------------------- */

/* The shell's full path: the one asked for, or pwsh, powershell, cmd. */
static int find_shell(const wchar_t *shell, wchar_t *exe)
{
    static const wchar_t *shells[] = { L"pwsh.exe", L"powershell.exe", L"cmd.exe" };
    if (shell && shell[0] && _wcsicmp(shell, L"auto") && _wcsicmp(shell, L"profile")) {
        wchar_t name[MAX_PATH];
        _snwprintf(name, MAX_PATH, L"%ls%ls", shell, wcschr(shell, L'.') ? L"" : L".exe");
        name[MAX_PATH - 1] = 0;
        return SearchPathW(NULL, name, NULL, MAX_PATH, exe, NULL) != 0;
    }
    for (size_t i = 0; i < sizeof shells / sizeof shells[0]; i++)
        if (SearchPathW(NULL, shells[i], NULL, MAX_PATH, exe, NULL)) return 1;
    return 0;
}

int shell_open_terminal(const wchar_t *dir, const wchar_t *program, const wchar_t *shell)
{
    wchar_t distro[128],linux[MAX_PATH];
    int native=term_wsl_split(dir,distro,128,linux,MAX_PATH);
    if(!native&&fpe_wsl_requires_native(dir))return -1;
    int use_wsl=shell&& !_wcsicmp(shell,L"wsl");
    if(native&&(!shell||!shell[0]||!_wcsicmp(shell,L"auto")))use_wsl=1;
    if(use_wsl && (!program||!program[0]||!_wcsicmp(program,L"auto")||!_wcsicmp(program,L"wt")||!_wcsicmp(program,L"wsl"))){
        wchar_t helper[MAX_PATH],arg[8192],params[9000],wsl[MAX_PATH],distarg[160]=L"";
        GetModuleFileNameW(NULL,helper,MAX_PATH);wchar_t *slash=wcsrchr(helper,L'\\');if(!slash)return -1;*slash=0;
        wchar_t file[MAX_PATH];_snwprintf(file,MAX_PATH,L"%ls\\folderwing-wsl",helper);
        if(GetFileAttributesW(file)==INVALID_FILE_ATTRIBUTES||!term_wsl_arg(dir,arg,8192))return -1;
        GetSystemDirectoryW(wsl,MAX_PATH);wcscat(wsl,L"\\wsl.exe");
        if(native)_snwprintf(distarg,160,L" --distribution %ls",distro);
        _snwprintf(params,9000,L"%ls --cd \"%ls\" --exec ./folderwing-wsl --shell %ls",distarg,helper,arg);
        wchar_t wt[MAX_PATH];
        if((!program||_wcsicmp(program,L"wsl"))&&ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Microsoft\\WindowsApps\\wt.exe",wt,MAX_PATH)&&GetFileAttributesW(wt)!=INVALID_FILE_ATTRIBUTES){
            wchar_t args[9500];_snwprintf(args,9500,L"-d . \"%ls\" %ls",wsl,params);
            return (INT_PTR)ShellExecuteW(NULL,NULL,wt,args,helper,SW_SHOWNORMAL)>32?0:-1;
        }
        return (INT_PTR)ShellExecuteW(NULL,NULL,wsl,params,helper,SW_SHOWNORMAL)>32?0:-1;
    }
    wchar_t exe[MAX_PATH], sh[MAX_PATH];
    int any = !program || !program[0] || !_wcsicmp(program, L"auto");
    /* Windows Terminal's app alias; "-d ." starts in the working directory we pass,
     * and a command line after it picks the shell (else its default profile runs). */
    if ((any || !_wcsicmp(program, L"wt")) &&
        ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Microsoft\\WindowsApps\\wt.exe", exe, MAX_PATH) &&
        GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES) {
        wchar_t args[MAX_PATH + 16] = L"-d .";
        if (!(shell && !_wcsicmp(shell, L"profile")) && find_shell(shell, sh)) _snwprintf(args, MAX_PATH + 16, L"-d . \"%ls\"", sh);
        args[MAX_PATH + 15] = 0;
        return (INT_PTR)ShellExecuteW(NULL, NULL, exe, args, dir, SW_SHOWNORMAL) > 32 ? 0 : -1;
    }
    if (!any) { /* "pwsh", "cmd.exe" or a full path */
        if (!find_shell(program, exe)) return -1;
        return (INT_PTR)ShellExecuteW(NULL, NULL, exe, NULL, dir, SW_SHOWNORMAL) > 32 ? 0 : -1;
    }
    if (!find_shell(shell, exe)) return -1;
    return (INT_PTR)ShellExecuteW(NULL, NULL, exe, NULL, dir, SW_SHOWNORMAL) > 32 ? 0 : -1;
}

/* 0 means publication accepted; -1 failed; -2 also failed Unicode restoration.
 * Prepare the prior Unicode bytes before EmptyClipboard. Other formats and
 * clipboard owner/sequence are not transactional rollback guarantees. */
int shell_copy_paths(HWND owner, const wchar_t *const *paths, uint32_t n)
{
    size_t chars = 1;
    for (uint32_t i = 0; i < n; i++) chars += wcslen(paths[i]) + 2;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, chars * sizeof(wchar_t)), previous = NULL;
    int opened = 0, copied = 0, restore_failed = 0;
    if (!h) return -1;
    wchar_t *p = GlobalLock(h);
    if (!p) goto done;
    for (uint32_t i = 0; i < n; i++) {
        size_t l = wcslen(paths[i]);
        memcpy(p, paths[i], l * sizeof(wchar_t));
        p += l;
        if (i + 1 < n) { *p++ = L'\r'; *p++ = L'\n'; }
    }
    *p = 0;
    GlobalUnlock(h);
    if (!(opened = OpenClipboard(owner))) goto done;
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE old = GetClipboardData(CF_UNICODETEXT);
        SIZE_T bytes = old ? GlobalSize(old) : 0;
        const void *before = bytes ? GlobalLock(old) : NULL;
        if (!before) goto done;
        previous = GlobalAlloc(GMEM_MOVEABLE, bytes);
        void *backup = previous ? GlobalLock(previous) : NULL;
        if (backup) {
            memcpy(backup, before, bytes);
            GlobalUnlock(previous);
        }
        GlobalUnlock(old);
        if (!backup) goto done;
    }
    if (!EmptyClipboard()) goto done;
    if (SetClipboardData(CF_UNICODETEXT, h)) {
        h = NULL;
        copied = 1;
    } else if (previous) {
        if (SetClipboardData(CF_UNICODETEXT, previous)) previous = NULL;
        else restore_failed = 1;
    }
done:
    if (opened) CloseClipboard();
    if (h) GlobalFree(h);
    if (previous) GlobalFree(previous);
    return copied ? 0 : restore_failed ? -2 : -1;
}

