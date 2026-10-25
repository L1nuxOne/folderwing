#include "../src/namecodec.h"
#include "internal.h"
#include "winsecurity.h"
#include "winstreams.h"
#include "wslproto.h"
#include <stddef.h>

/*
 * One folderwing-wsl helper per distro, run with wsl.exe. A host starts on a thread
 * the first time one of its folders is listed or watched, and stops (stdin
 * closed) idle_ms after its last request and watch are gone. Everything
 * shared is guarded by g_lock; the reader thread of each host turns the
 * helper's answers into snapshots and watch events under it.
 */

#define HOSTS_MAX        8
#define START_TIMEOUT_MS 30000
#define REQ_TIMEOUT_MS   30000
#define RETRY_MS         120000 /* after a start that failed */
#define READ_BUF         (128u * 1024)

enum { H_OFF, H_STARTING, H_READY };

typedef struct req {
    struct req *next;
    uint32_t    id;
    int         done;
    fpe_err     err;
    fpe_dir    *dir;
} req;

typedef struct host host;

struct wsl_watch {
    wsl_watch *next;
    host      *h;
    uint32_t   id;
    char      *path; /* in the distro */
    HANDLE     ev;
};

struct host {
    wchar_t    name[64];
    int        state;
    int        stopping; /* stdin closed on purpose: no restart */
    HANDLE     proc, to, from, reader;
    uint32_t   next_id;
    req       *reqs;
    wsl_watch *watches;
    uint32_t   busy;       /* requests in flight */
    uint64_t   idle_since; /* tick when it last became unused, 0 while used */
    uint64_t   started_at;
    uint64_t   failed_at;
};

static SRWLOCK            g_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
static host               g_hosts[HOSTS_MAX];
static uint32_t           g_nhosts;
static fpe_wsl_config     g_cfg = { 1, 0, 60000 };
static void index_shutdown_async(void);
static int                g_have = -1; /* folderwing-wsl and wsl.exe exist: -1 not checked yet */
static wchar_t            g_dir[MAX_PATH];
static wchar_t            g_wsl[MAX_PATH];
static HANDLE             g_job;
static PTP_TIMER          g_idle;
static fpe_wsl_stats      g_stats;
static fpe_gauge          g_helpers = FPE_GAUGE_INIT("wsl.helpers");

/* ---- paths and names --------------------------------------------------------- */

static int prefix_len(const wchar_t *p)
{
    if (!_wcsnicmp(p, L"\\\\wsl.localhost\\", 16)) return 16;
    if (!_wcsnicmp(p, L"\\\\wsl$\\", 7)) return 7;
    if(!_wcsnicmp(p,L"\\\\?\\UNC\\wsl.localhost\\",22))return 22;
    if(!_wcsnicmp(p,L"\\\\?\\UNC\\wsl$\\",13))return 13;
    return 0;
}

static int check_helper(void)
{
    if (g_have >= 0) return g_have;
    HMODULE m = NULL;
    g_have = 0;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(void *)&check_helper, &m))
        return 0;
    DWORD n = GetModuleFileNameW(m, g_dir, MAX_PATH);
    wchar_t *slash = n && n < MAX_PATH ? wcsrchr(g_dir, L'\\') : NULL;
    if (!slash) return 0;
    *slash = 0;
    wchar_t probe[MAX_PATH + 16];
    _snwprintf(probe, MAX_PATH + 16, L"%ls\\folderwing-wsl", g_dir);
    probe[MAX_PATH + 15] = 0;
    UINT sn = GetSystemDirectoryW(g_wsl, MAX_PATH - 10);
    if (!sn || sn >= MAX_PATH - 10) return 0;
    wcscat(g_wsl, L"\\wsl.exe");
    g_have = GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES && GetFileAttributesW(g_wsl) != INVALID_FILE_ATTRIBUTES;
    return g_have;
}

static SRWLOCK g_helper_lock=SRWLOCK_INIT;
static int have_helper(void)
{
    AcquireSRWLockExclusive(&g_helper_lock);
    int yes=check_helper();
    ReleaseSRWLockExclusive(&g_helper_lock);return yes;
}

/* Presence of wsl.exe alone is insufficient on a stock Windows install.
 * Check the user's distro registrations without loading WSL or calling its
 * service from an OLE/input dispatch. Only actual launch/read paths ask this. */
static int registered_distro(const wchar_t *unc)
{
    int prefix=prefix_len(unc);if(!prefix)return 0;
    wchar_t distro[64];size_t n=0;while(unc[prefix+n]&&unc[prefix+n]!=L'\\'&&n<63){distro[n]=unc[prefix+n];n++;}distro[n]=0;
    if(!n||(unc[prefix+n]&&unc[prefix+n]!=L'\\'))return 0;
    static SRWLOCK lock=SRWLOCK_INIT;
    static struct {wchar_t name[64];int yes;uint64_t at;} cache[16];
    uint64_t now=GetTickCount64();AcquireSRWLockExclusive(&lock);int slot=0;
    for(int i=0;i<16;i++){
        if(!_wcsicmp(cache[i].name,distro)&&now-cache[i].at<10000){int yes=cache[i].yes;ReleaseSRWLockExclusive(&lock);return yes;}
        if(cache[i].at<cache[slot].at)slot=i;
    }
    int yes=0;HKEY registrations;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss",0,KEY_READ,&registrations)==ERROR_SUCCESS){
        for(DWORD index=0;;index++){
            wchar_t key_name[128];DWORD length=128;LONG err=RegEnumKeyExW(registrations,index,key_name,&length,NULL,NULL,NULL,NULL);
            if(err==ERROR_NO_MORE_ITEMS)break;if(err!=ERROR_SUCCESS)break;
            HKEY key;if(RegOpenKeyExW(registrations,key_name,0,KEY_QUERY_VALUE,&key)!=ERROR_SUCCESS)continue;
            wchar_t name[128]={0};DWORD bytes=sizeof name,type=0;
            err=RegQueryValueExW(key,L"DistributionName",NULL,&type,(BYTE*)name,&bytes);RegCloseKey(key);
            if(err==ERROR_SUCCESS&&type==REG_SZ&&bytes>=sizeof(wchar_t)&&bytes<=sizeof name&&name[bytes/sizeof(wchar_t)-1]==0&&!_wcsicmp(name,distro)){yes=1;break;}
        }
        RegCloseKey(registrations);
    }
    wcscpy(cache[slot].name,distro);cache[slot].at=now;cache[slot].yes=yes;ReleaseSRWLockExclusive(&lock);return yes;
}

/* WSL's own rule for distro names; anything else isn't one the helper can be started for */
static int distro_len(const wchar_t *d)
{
    int n = 0;
    while (d[n] && d[n] != L'\\') {
        if (!iswalnum(d[n]) && !wcschr(L"._-", d[n])) return 0;
        n++;
    }
    return n < 64 ? n : 0;
}

/* Drive letters mapped to a WSL share (net use Z: \\wsl.localhost\Ubuntu), per letter,
 * asked again after 10 s: WNetGetConnection goes through the network providers. */
static SRWLOCK  g_dlock = SRWLOCK_INIT;
static wchar_t  g_dmap[26][MAX_PATH]; /* "" = not a WSL share */
static uint64_t g_dat[26];

static int drive_share(wchar_t letter, wchar_t *out)
{
    int i = towupper(letter) - L'A';
    out[0] = 0;
    if (i < 0 || i >= 26) return 0;
    uint64_t now = GetTickCount64();
    AcquireSRWLockShared(&g_dlock);
    int fresh = g_dat[i] && now - g_dat[i] < 10000;
    if (fresh) wcscpy(out, g_dmap[i]);
    ReleaseSRWLockShared(&g_dlock);
    if (fresh) return out[0] != 0;
    wchar_t root[4] = { letter, L':', L'\\', 0 }, local[3] = { letter, L':', 0 }, remote[MAX_PATH];
    DWORD len = MAX_PATH;
    if (GetDriveTypeW(root) == DRIVE_REMOTE && WNetGetConnectionW(local, remote, &len) == NO_ERROR) {
        size_t n = wcslen(remote);
        while (n && remote[n - 1] == L'\\') remote[--n] = 0;
        int k = prefix_len(remote);
        if (k && distro_len(remote + k)) wcscpy(out, remote);
    }
    AcquireSRWLockExclusive(&g_dlock);
    wcscpy(g_dmap[i], out);
    g_dat[i] = now ? now : 1;
    ReleaseSRWLockExclusive(&g_dlock);
    return out[0] != 0;
}

/* The \\wsl.localhost form of a WSL path, also for one on a mapped drive
 * ("Z:\home" -> "\\wsl.localhost\Ubuntu\home"); NULL for anything else. */
static wchar_t *unc_form(const wchar_t *display)
{
    int k = prefix_len(display);
    if (k) return distro_len(display + k) ? fpe_wcsdup(display) : NULL;
    wchar_t share[MAX_PATH];
    if (!display[0] || display[1] != L':' || !drive_share(display[0], share)) return NULL;
    size_t sl = wcslen(share), rl = wcslen(display + 2);
    wchar_t *s = fpe_alloc((sl + rl + 1) * sizeof(wchar_t));
    if (!s) return NULL;
    memcpy(s, share, sl * sizeof(wchar_t));
    memcpy(s + sl, display + 2, (rl + 1) * sizeof(wchar_t));
    return s;
}

int fpe_wsl_owns(const wchar_t *display)
{
    wchar_t *unc = unc_form(display);
    if (!unc) return 0;
    int installed=registered_distro(unc);fpe_free(unc);
    AcquireSRWLockShared(&g_lock);
    int on = g_cfg.enabled;
    ReleaseSRWLockShared(&g_lock);
    return on && installed && have_helper();
}

/* The distro and its path for a display path: "\\wsl.localhost\Ubuntu\home" -> "Ubuntu", "/home".
 * Bytes that aren't UTF-8 come back as U+DC80 + byte; they go back as that byte. */
static int split(const wchar_t *display, wchar_t *distro, char **path)
{
    wchar_t *unc = unc_form(display);
    if (!unc) return 0;
    const wchar_t *d = unc + prefix_len(unc);
    size_t dl = (size_t)distro_len(d);
    const wchar_t *rest = d + dl;
    memcpy(distro, d, dl * sizeof(wchar_t));
    distro[dl] = 0;
    while (rest[0] == L'\\' && rest[1] == L'\\') rest++;
    if (rest[0] == L'\\' && !rest[1]) rest++; /* the distro's root */
    size_t n = wcslen(rest);
    char *s = fpe_alloc(n * 3 + 2), *o = s;
    if (!s) {
        fpe_free(unc);
        return 0;
    }
    if (!n) *o++ = '/';
    size_t bytes=0;
    if(n&&!fpe_lx_to_bytes(rest,(uint8_t*)o,n*3,1,&bytes)){
        fpe_free(s);fpe_free(unc);SetLastError(ERROR_INVALID_NAME);return 0;
    }
    o+=bytes;*o=0;
    *path = s;
    fpe_free(unc);
    return 1;
}

/* Capacity is bounded by the original byte length, including quoted PUA. */
static uint32_t to_wide(const uint8_t *s,uint32_t n,wchar_t *out,int map)
{
    size_t length=0;
    return fpe_lx_from_bytes(s,n,out,(size_t)n+1,map,0,&length)?(uint32_t)length:0;
}

static fpe_err from_errno(int e)
{
    switch (e) {
    case 0: return 0;
    case 1:  /* EPERM */
    case 13: return ERROR_ACCESS_DENIED;
    case 2:  return ERROR_PATH_NOT_FOUND;
    case 5:  return ERROR_IO_DEVICE;
    case 12: return ERROR_NOT_ENOUGH_MEMORY;
    case 17: return ERROR_FILE_EXISTS;
    case 18: return ERROR_NOT_SAME_DEVICE;
    case 21: return ERROR_DIRECTORY;
    case 28: return ERROR_DISK_FULL;
    case 39: return ERROR_DIR_NOT_EMPTY;
    case 95: return ERROR_NOT_SUPPORTED;
    case 116: return ERROR_FILE_INVALID;
    case 125: return ERROR_CANCELLED;
    case 20: return ERROR_DIRECTORY;
    case 22: return ERROR_INVALID_NAME;
    case 36: return ERROR_FILENAME_EXCED_RANGE;
    case 40: return ERROR_CANT_RESOLVE_FILENAME; /* ELOOP */
    default: return ERROR_GEN_FAILURE;
    }
}

/* ---- hosts (g_lock held) --------------------------------------------------------- */

static host *host_get(const wchar_t *name)
{
    for (uint32_t i = 0; i < g_nhosts; i++)
        if (!_wcsicmp(g_hosts[i].name, name)) return &g_hosts[i];
    if (g_nhosts == HOSTS_MAX) return NULL;
    host *h = &g_hosts[g_nhosts++];
    memset(h, 0, sizeof *h);
    wcscpy(h->name, name);
    return h;
}

static int send_msg(host *h, uint32_t type, uint32_t id, const void *payload, uint32_t size)
{
    if (h->state != H_READY || !h->to) return 0;
    uint8_t small[512], *buf = small;
    uint32_t total = (uint32_t)sizeof(wslp_head) + size;
    if (total > sizeof small && !(buf = fpe_alloc(total))) return 0;
    wslp_head hd = { type, size, id };
    memcpy(buf, &hd, sizeof hd);
    if (size) memcpy(buf + sizeof hd, payload, size);
    DWORD w = 0;
    BOOL ok = WriteFile(h->to, buf, total, &w, NULL) && w == total;
    if (buf != small) fpe_free(buf);
    return ok;
}

static void idle_check(void);

static void note_use(host *h)
{
    if (h->busy || h->watches) {
        h->idle_since = 0;
        return;
    }
    if (h->idle_since) return;
    h->idle_since = GetTickCount64();
    idle_check();
}

/* ---- the reader ------------------------------------------------------------------ */

static req *req_find(host *h, uint32_t id)
{
    for (req *r = h->reqs; r; r = r->next)
        if (r->id == id) return r;
    return NULL;
}

static wsl_watch *watch_find(host *h, uint32_t id)
{
    for (wsl_watch *w = h->watches; w; w = w->next)
        if (w->id == id) return w;
    return NULL;
}

#define LX_SYMLINK 0xA000001Du
#define LX_SOCK    0x80000023u
#define LX_FIFO    0x80000024u
#define LX_CHR     0x80000025u
#define LX_BLK     0x80000026u

static int64_t filetime(int64_t ns) { return ns / 100 + 116444736000000000LL; }

static void add_entries(req *r, const uint8_t *p, uint32_t n)
{
    wchar_t name[256], link[4096];
    for (uint32_t at = 0; !r->err && at + sizeof(wslp_entry) <= n;) {
        wslp_entry e;
        memcpy(&e, p + at, sizeof e);
        uint32_t len = (uint32_t)sizeof e + e.name_len + e.link_len;
        if (at + len > n || e.name_len > 255 || e.link_len >= 4096) {
            r->err = ERROR_INVALID_DATA;
            return;
        }
        const uint8_t *nm = p + at + sizeof e;
        uint32_t wn = to_wide(nm, e.name_len, name, 1);
        uint32_t wl = e.link_len ? to_wide(nm + e.name_len, e.link_len, link, 0) : 0;
        uint32_t type = e.mode & 0170000, attrs, tag = 0;
        switch (type) {
        case 0040000: attrs = FILE_ATTRIBUTE_DIRECTORY; break;
        case 0120000:
            attrs = FILE_ATTRIBUTE_REPARSE_POINT | ((e.flags & WSLP_E_DIRLINK) ? FILE_ATTRIBUTE_DIRECTORY : 0);
            tag = LX_SYMLINK;
            break;
        case 0010000: attrs = FILE_ATTRIBUTE_REPARSE_POINT, tag = LX_FIFO; break;
        case 0020000: attrs = FILE_ATTRIBUTE_REPARSE_POINT, tag = LX_CHR; break;
        case 0060000: attrs = FILE_ATTRIBUTE_REPARSE_POINT, tag = LX_BLK; break;
        case 0140000: attrs = FILE_ATTRIBUTE_REPARSE_POINT, tag = LX_SOCK; break;
        default: attrs = FILE_ATTRIBUTE_NORMAL; break;
        }
        int64_t mt = filetime(e.mtime);
        r->err = fpe_dir_add(r->dir, name, wn, e.size, mt, e.btime ? filetime(e.btime) : mt, attrs, tag);
        if (!r->err)
            r->err = fpe_dir_add_unix(r->dir, e.mode, e.uid, e.gid, type == 0120000 ? link : NULL, wl);
        at += wslp_pad8(len);
    }
}

static void add_owners(req *r, const uint8_t *p, uint32_t n)
{
    wchar_t name[256];
    for (uint32_t at = 0; !r->err && at + sizeof(wslp_owner) <= n;) {
        wslp_owner o;
        memcpy(&o, p + at, sizeof o);
        uint32_t len = (uint32_t)sizeof o + o.len;
        if (at + len > n) break;
        uint32_t wn = to_wide(p + at + sizeof o, o.len, name, 0);
        r->err = fpe_dir_add_owner(r->dir, o.id, o.kind == WSLP_OWNER_GROUP, name, wn);
        at += wslp_pad8(len);
    }
}

static void dispatch(host *h, const wslp_head *hd, const uint8_t *p)
{
    req *r;
    wsl_watch *w;
    switch (hd->type) {
    case WSLP_ENTRIES:
        if ((r = req_find(h, hd->id)) && r->dir) add_entries(r, p, hd->size);
        break;
    case WSLP_OWNERS:
        if ((r = req_find(h, hd->id)) && r->dir) add_owners(r, p, hd->size);
        break;
    case WSLP_LIST_END:
        if ((r = req_find(h, hd->id)) && hd->size >= sizeof(wslp_end)) {
            wslp_end e;
            memcpy(&e, p, sizeof e);
            if (!r->err) r->err = from_errno(e.err);
            if (!r->err) r->err = fpe_dir_finish(r->dir);
            r->done = 1;
            WakeAllConditionVariable(&g_cv);
        }
        break;
    case WSLP_CHANGED:
        if ((w = watch_find(h, hd->id))) SetEvent(w->ev);
        break;
    default: break; /* WATCHED: a folder that can't be watched just stays quiet */
    }
}

static int read_all(HANDLE f, void *p, DWORD n)
{
    for (DWORD k = 0; k < n;) {
        DWORD got = 0;
        if (!ReadFile(f, (uint8_t *)p + k, n - k, &got, NULL) || !got) return 0;
        k += got;
    }
    return 1;
}

static DWORD WINAPI start_thread(LPVOID p);

static DWORD WINAPI reader_thread(LPVOID p)
{
    host *h = p;
    uint8_t *buf = fpe_alloc(READ_BUF);
    HANDLE from = h->from;
    wslp_head hd;
    while (buf && read_all(from, &hd, sizeof hd) && hd.size <= READ_BUF && read_all(from, buf, hd.size)) {
        AcquireSRWLockExclusive(&g_lock);
        dispatch(h, &hd, buf);
        ReleaseSRWLockExclusive(&g_lock);
    }
    fpe_free(buf);

    /* gone: fail what waits, keep the watches for the next start */
    AcquireSRWLockExclusive(&g_lock);
    for (req *r = h->reqs; r; r = r->next)
        if (!r->done) {
            r->err = ERROR_BROKEN_PIPE;
            r->done = 1;
        }
    if (h->to) CloseHandle(h->to);
    CloseHandle(h->from);
    if (h->proc) {
        if (WaitForSingleObject(h->proc, 2000) == WAIT_TIMEOUT) TerminateProcess(h->proc, 1);
        CloseHandle(h->proc);
    }
    h->to = h->from = h->proc = NULL;
    h->state = H_OFF;
    fpe_gauge_add(&g_helpers, -1);
    g_stats.helpers--;
    /* it died by itself: start again for the watches, unless it dies young */
    int again = !h->stopping && h->watches && GetTickCount64() - h->started_at > 10000;
    if (!h->stopping && !again) h->failed_at = GetTickCount64();
    if (again) {
        h->state = H_STARTING;
        HANDLE t = CreateThread(NULL, 64 * 1024, start_thread, h, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (t) CloseHandle(t);
        else h->state = H_OFF;
    }
    WakeAllConditionVariable(&g_cv);
    ReleaseSRWLockExclusive(&g_lock);
    return 0;
}

/* ---- starting ------------------------------------------------------------------- */

static SRWLOCK g_job_lock = SRWLOCK_INIT;
static HANDLE job(void)
{
    AcquireSRWLockExclusive(&g_job_lock);
    if (g_job) { ReleaseSRWLockExclusive(&g_job_lock); return g_job; }
    g_job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = { 0 };
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (g_job) SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof li);
    ReleaseSRWLockExclusive(&g_job_lock);
    return g_job;
}

/* wsl.exe with only the two pipes and NUL inherited, in a job that ends with us */
static int launch(host *h, int as_root, HANDLE *to, HANDLE *from, HANDLE *proc, const wchar_t *extra)
{
    wchar_t distro_path[80];swprintf(distro_path,80,L"\\\\wsl$\\%ls",h->name);
    if(!have_helper()||!registered_distro(distro_path))return 0;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE in_r = NULL, in_w = NULL, out_r = NULL, out_w = NULL;
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    int ok = nul != INVALID_HANDLE_VALUE && CreatePipe(&in_r, &in_w, &sa, 1 << 16) && CreatePipe(&out_r, &out_w, &sa, 1 << 16);
    if (ok) {
        SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    }
    SIZE_T asz = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &asz);
    LPPROC_THREAD_ATTRIBUTE_LIST al = ok ? fpe_alloc(asz) : NULL;
    HANDLE inherit[3] = { in_r, out_w, nul };
    ok = al && InitializeProcThreadAttributeList(al, 1, 0, &asz) &&
         UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, NULL, NULL);

    wchar_t cmd[3 * MAX_PATH];
    /* wsl.exe would take quotes around the name as part of it; split() allows no characters that need them */
    _snwprintf(cmd, 3 * MAX_PATH, L"\"%ls\" --distribution %ls%ls --cd \"%ls\" --exec ./folderwing-wsl%ls", g_wsl, h->name,
               as_root ? L" --user root" : L"", g_dir, extra);
    cmd[3 * MAX_PATH - 1] = 0;
    STARTUPINFOEXW si = { { sizeof si } };
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_r;
    si.StartupInfo.hStdOutput = out_w;
    si.StartupInfo.hStdError = nul;
    si.lpAttributeList = al;
    PROCESS_INFORMATION pi = { 0 };
    ok = ok && CreateProcessW(g_wsl, cmd, NULL, NULL, TRUE,
                              CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                              &si.StartupInfo, &pi);
    if (al) {
        DeleteProcThreadAttributeList(al);
        fpe_free(al);
    }
    if (ok) {
        if (!job() || !AssignProcessToJobObject(g_job, pi.hProcess)) {
            TerminateProcess(pi.hProcess,1);CloseHandle(pi.hProcess);ok=0;
        } else ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
    }
    if (in_r) CloseHandle(in_r);
    if (out_w) CloseHandle(out_w);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        if (in_w) CloseHandle(in_w);
        if (out_r) CloseHandle(out_r);
        return 0;
    }
    *to = in_w;
    *from = out_r;
    *proc = pi.hProcess;
    return 1;
}

/* Waits for HELLO without blocking forever on a wsl.exe that never answers. */
static int hello(HANDLE from, HANDLE proc)
{
    uint64_t t0 = GetTickCount64();
    DWORD need = sizeof(wslp_head) + sizeof(wslp_hello);
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(from, NULL, 0, NULL, &avail, NULL)) return 0;
        if (avail >= need) break;
        if (WaitForSingleObject(proc, 2) == WAIT_OBJECT_0) {
            if (!PeekNamedPipe(from, NULL, 0, NULL, &avail, NULL) || avail < need) return 0;
            break;
        }
        if (GetTickCount64() - t0 > START_TIMEOUT_MS) return 0;
        AcquireSRWLockShared(&g_lock);
        int quit = g_cfg.enabled == 0;
        ReleaseSRWLockShared(&g_lock);
        if (quit) return 0;
    }
    wslp_head hd;
    wslp_hello hi;
    return read_all(from, &hd, sizeof hd) && read_all(from, &hi, sizeof hi) && hd.type == WSLP_HELLO &&
           hd.size == sizeof hi && hi.magic == WSLP_MAGIC && hi.version == WSLP_VERSION;
}

static DWORD WINAPI start_thread(LPVOID p)
{
    host *h = p;
    int64_t t = fpe_prof_begin();
    uint64_t t0 = GetTickCount64();
    AcquireSRWLockShared(&g_lock);
    int as_root = g_cfg.as_root;
    ReleaseSRWLockShared(&g_lock);

    HANDLE to = NULL, from = NULL, proc = NULL;
    int ok = launch(h, as_root, &to, &from, &proc, L"") && hello(from, proc);

    AcquireSRWLockExclusive(&g_lock);
    g_stats.starts++;
    g_stats.last_start_ms = (uint32_t)(GetTickCount64() - t0);
    HANDLE old = h->reader;
    h->reader = NULL;
    if (ok && !h->stopping) {
        h->to = to, h->from = from, h->proc = proc;
        h->reader = CreateThread(NULL, 64 * 1024, reader_thread, h, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        ok = h->reader != NULL;
    } else {
        ok = 0;
    }
    if (ok) {
        h->state = H_READY;
        h->started_at = GetTickCount64();
        g_stats.helpers++;
        fpe_gauge_add(&g_helpers, 1);
        /* watches made while it started: arm them, and let their folders
         * reload so they show what the helper sees */
        for (wsl_watch *w = h->watches; w; w = w->next) {
            send_msg(h, WSLP_WATCH, w->id, w->path, (uint32_t)strlen(w->path));
            SetEvent(w->ev);
        }
        note_use(h);
    } else {
        h->to = h->from = h->proc = NULL;
        if (proc) TerminateProcess(proc, 1);
        if (to) CloseHandle(to);
        if (from) CloseHandle(from);
        if (proc) CloseHandle(proc);
        h->state = H_OFF;
        if (!h->stopping) {
            h->failed_at = GetTickCount64();
            g_stats.failures++;
        }
    }
    WakeAllConditionVariable(&g_cv);
    ReleaseSRWLockExclusive(&g_lock);
    if (old) {
        WaitForSingleObject(old, INFINITE); /* the previous reader, finished by now */
        CloseHandle(old);
    }
    FPE_PROF_END("wsl.start", t);
    return 0;
}

/* Starts h unless it runs, is starting, or failed recently. */
static void kick(host *h)
{
    if (h->state != H_OFF || !g_cfg.enabled) return;
    if (h->failed_at && GetTickCount64() - h->failed_at < RETRY_MS) return;
    h->stopping = 0;
    h->state = H_STARTING;
    HANDLE t = CreateThread(NULL, 64 * 1024, start_thread, h, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (t) CloseHandle(t);
    else h->state = H_OFF;
}

/* ---- idle stop ------------------------------------------------------------------ */

static void stop_host(host *h)
{
    h->stopping = 1;
    if (h->to) {
        CloseHandle(h->to); /* folderwing-wsl exits on EOF; its reader cleans up */
        h->to = NULL;
    }
}

static void CALLBACK idle_tick(PTP_CALLBACK_INSTANCE inst, PVOID ctx, PTP_TIMER timer)
{
    (void)inst, (void)ctx, (void)timer;
    AcquireSRWLockExclusive(&g_lock);
    idle_check();
    ReleaseSRWLockExclusive(&g_lock);
}

/* Stops hosts idle long enough; arms the timer for the next one. */
static void idle_check(void)
{
    uint64_t now = GetTickCount64(), next = 0;
    for (uint32_t i = 0; i < g_nhosts; i++) {
        host *h = &g_hosts[i];
        if (h->state != H_READY || h->busy || h->watches || !h->idle_since || h->stopping) continue;
        uint64_t due = h->idle_since + g_cfg.idle_ms;
        if (now >= due) stop_host(h);
        else if (!next || due < next) next = due;
    }
    if (!next) return;
    if (!g_idle) g_idle = CreateThreadpoolTimer(idle_tick, NULL, NULL);
    if (!g_idle) return;
    LARGE_INTEGER li;
    li.QuadPart = -(LONGLONG)(next - now + 10) * 10000;
    FILETIME ft = { li.LowPart, (DWORD)li.HighPart };
    SetThreadpoolTimer(g_idle, &ft, 0, 100);
}

/* ---- requests ------------------------------------------------------------------- */

fpe_err fpe_wsl_list(const wchar_t *display, int wait, fpe_dir **out)
{
    *out = NULL;
    wchar_t distro[64];
    char *path = NULL;
    if (!split(display, distro, &path)) return ERROR_INVALID_NAME;
    int64_t t = fpe_prof_begin();

    AcquireSRWLockExclusive(&g_lock);
    host *h = host_get(distro);
    if (!h) {
        ReleaseSRWLockExclusive(&g_lock);
        fpe_free(path);
        return FPE_WSL_NOT_READY;
    }
    /* one stopping for idleness is waited out, then started again */
    for (uint64_t t0 = GetTickCount64(); wait && h->stopping && h->state != H_OFF && GetTickCount64() - t0 < 3000;)
        SleepConditionVariableSRW(&g_cv, &g_lock, 100, 0);
    kick(h);
    if ((h->state != H_READY || h->stopping) && !wait) {
        ReleaseSRWLockExclusive(&g_lock);
        fpe_free(path);
        return FPE_WSL_NOT_READY;
    }
    while (h->state == H_STARTING) SleepConditionVariableSRW(&g_cv, &g_lock, START_TIMEOUT_MS, 0);

    req r = { 0 };
    fpe_err err = FPE_WSL_NOT_READY;
    if (h->state == H_READY && !h->stopping && (r.dir = fpe_dir_new(display, 0))) {
        r.id = ++h->next_id;
        r.next = h->reqs;
        h->reqs = &r;
        h->busy++;
        note_use(h);
        if (send_msg(h, WSLP_LIST, r.id, path, (uint32_t)strlen(path))) {
            uint64_t t0 = GetTickCount64();
            while (!r.done && GetTickCount64() - t0 < REQ_TIMEOUT_MS)
                SleepConditionVariableSRW(&g_cv, &g_lock, REQ_TIMEOUT_MS, 0);
            err = r.done ? r.err : ERROR_TIMEOUT;
        } else {
            err = ERROR_BROKEN_PIPE;
        }
        for (req **pp = &h->reqs; *pp; pp = &(*pp)->next)
            if (*pp == &r) {
                *pp = r.next;
                break;
            }
        h->busy--;
        note_use(h);
    }
    ReleaseSRWLockExclusive(&g_lock);
    fpe_free(path);
    /* the helper failing isn't the folder's fault: \\wsl.localhost may still read it */
    if (err == ERROR_BROKEN_PIPE || err == ERROR_TIMEOUT || err == ERROR_INVALID_DATA) err = FPE_WSL_NOT_READY;
    if (err) {
        fpe_dir_release(r.dir);
        FPE_PROF_END("wsl.list_failed", t);
        return err;
    }
    *out = r.dir;
    FPE_PROF_END("wsl.list", t);
    return 0;
}

wsl_watch *fpe_wsl_watch(const wchar_t *display, HANDLE ev)
{
    wchar_t distro[64];
    char *path = NULL;
    if (!split(display, distro, &path)) return NULL;
    wsl_watch *w = fpe_zalloc(sizeof *w);
    if (!w) {
        fpe_free(path);
        return NULL;
    }
    AcquireSRWLockExclusive(&g_lock);
    host *h = host_get(distro);
    if (!h) {
        ReleaseSRWLockExclusive(&g_lock);
        fpe_free(path);
        fpe_free(w);
        return NULL;
    }
    w->h = h;
    w->id = ++h->next_id;
    w->path = path;
    w->ev = ev;
    w->next = h->watches;
    h->watches = w;
    note_use(h);
    if (h->state == H_READY) send_msg(h, WSLP_WATCH, w->id, path, (uint32_t)strlen(path));
    else kick(h); /* armed when it is up */
    ReleaseSRWLockExclusive(&g_lock);
    return w;
}

void fpe_wsl_unwatch(wsl_watch *w)
{
    if (!w) return;
    AcquireSRWLockExclusive(&g_lock);
    host *h = w->h;
    for (wsl_watch **pp = &h->watches; *pp; pp = &(*pp)->next)
        if (*pp == w) {
            *pp = w->next;
            break;
        }
    send_msg(h, WSLP_UNWATCH, w->id, NULL, 0);
    note_use(h);
    ReleaseSRWLockExclusive(&g_lock);
    fpe_free(w->path);
    fpe_free(w);
}

/* ---- public ------------------------------------------------------------------- */

FPE_API void fpe_wsl_configure(const fpe_wsl_config *cfg)
{
    AcquireSRWLockExclusive(&g_lock);
    int changed=g_cfg.enabled!=(cfg?cfg->enabled:1)||g_cfg.as_root!=(cfg?cfg->as_root:0);
    g_cfg.enabled = cfg ? cfg->enabled : 1;
    g_cfg.as_root = cfg ? cfg->as_root : 0;
    g_cfg.idle_ms = cfg && cfg->idle_ms ? cfg->idle_ms : 60000;
    ReleaseSRWLockExclusive(&g_lock);
    if(changed)index_shutdown_async();
}

FPE_API void fpe_wsl_get_stats(fpe_wsl_stats *out)
{
    AcquireSRWLockShared(&g_lock);
    *out = g_stats;
    ReleaseSRWLockShared(&g_lock);
}
FPE_API int fpe_wsl_available(const wchar_t *path){return path&&fpe_wsl_owns(path);}

FPE_API fpe_err fpe_wsl_read(const wchar_t *path, fpe_dir **out)
{
    if (!out) return ERROR_INVALID_PARAMETER;
    *out = NULL;
    wchar_t *disp = NULL;
    fpe_err err = fpe_path_display(path, &disp);
    if (!err) err = fpe_wsl_owns(disp) ? fpe_wsl_list(disp, 1, out) : ERROR_NOT_SUPPORTED;
    fpe_free(disp);
    return err == FPE_WSL_NOT_READY ? ERROR_NOT_SUPPORTED : err;
}

/* Stops every helper and waits for them (engine shutdown). */
void fpe_wsl_shutdown(void)
{
    AcquireSRWLockExclusive(&g_lock);
    for (uint32_t i = 0; i < g_nhosts; i++) {
        host *h = &g_hosts[i];
        h->stopping = 1;
        if (h->to) {
            CloseHandle(h->to);
            h->to = NULL;
        }
    }
    uint64_t t0 = GetTickCount64();
    for (;;) {
        int busy = 0;
        for (uint32_t i = 0; i < g_nhosts; i++) busy |= g_hosts[i].state != H_OFF;
        if (!busy || GetTickCount64() - t0 > 3000) break;
        SleepConditionVariableSRW(&g_cv, &g_lock, 100, 0);
    }
    for (uint32_t i = 0; i < g_nhosts; i++) {
        host *h = &g_hosts[i];
        if (h->proc) TerminateProcess(h->proc, 1); /* didn't leave in time; its reader sees EOF */
        h->failed_at = 0;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (g_idle) {
        SetThreadpoolTimer(g_idle, NULL, 0, 0);
        WaitForThreadpoolTimerCallbacks(g_idle, TRUE);
        CloseThreadpoolTimer(g_idle);
        g_idle = NULL;
    }
}

/* ---- native filesystem jobs -------------------------------------------------- */

FPE_API int fpe_wsl_path_kind(const wchar_t *path)
{
    if(!path||!path[0])return 0;
    wchar_t *unc=unc_form(path);int yes=unc!=NULL;fpe_free(unc);return yes;
}
FPE_API int fpe_wsl_requires_native(const wchar_t *path)
{
    return path&&wcschr(path,FPE_LX_ESCAPE)&&fpe_wsl_path_kind(path);
}

FPE_API int fpe_wsl_native(const wchar_t *path)
{
    AcquireSRWLockShared(&g_lock);int on=g_cfg.enabled;ReleaseSRWLockShared(&g_lock);
    return on&&fpe_wsl_path_kind(path);
}

void fpe_wsl_job_policy(int *enabled, int *as_root)
{
    AcquireSRWLockShared(&g_lock);
    *enabled = g_cfg.enabled;
    *as_root = g_cfg.as_root;
    ReleaseSRWLockShared(&g_lock);
}

fpe_err fpe_wsl_endpoint(const wchar_t *path, wchar_t **out)
{
    *out = NULL;
    if (!path || !*path) return 0;
    int k = prefix_len(path);
    wchar_t share[MAX_PATH];
    int known = k ? distro_len(path+k) != 0 :
        path[1] == L':' && drive_share(path[0],share);
    if (!known) return 0;
    *out = unc_form(path);
    return *out ? 0 : ERROR_NOT_ENOUGH_MEMORY;
}

static char *job_path(const wchar_t *path, const wchar_t *distro, uint32_t *flags)
{
    *flags = 0;
    if (!path || !path[0]) return fpe_zalloc(1);
    wchar_t other[64]; char *bytes = NULL;
    if (split(path, other, &bytes)) {
        if (_wcsicmp(other, distro)) { fpe_free(bytes); SetLastError(ERROR_NOT_SUPPORTED); return NULL; }
        return bytes;
    }
    /* A malformed native identity must not fall through to Windows path conversion. */
    if(fpe_wsl_path_kind(path)){SetLastError(ERROR_INVALID_NAME);return NULL;}
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, NULL, 0, NULL, NULL);
    if (!n) return NULL;
    bytes = fpe_alloc((size_t)n);
    if (!bytes || !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, bytes, n, NULL, NULL)) {
        fpe_free(bytes); return NULL;
    }
    *flags = WSLP_PATH_WINDOWS;
    return bytes;
}

typedef struct job_writer {
    HANDLE pipe;
    uint8_t *bytes;
    DWORD size;
    fpe_err err;
} job_writer;
static DWORD WINAPI job_write(void *arg)
{
    job_writer *w = arg;
    for (DWORD at = 0; at < w->size;) {
        DWORD n = w->size - at, wrote = 0;
        if (n > 32768) n = 32768;
        if (!WriteFile(w->pipe, w->bytes + at, n, &wrote, NULL) || !wrote) {
            w->err = GetLastError(); if (!w->err) w->err = ERROR_BROKEN_PIPE; break;
        }
        at += wrote;
    }
    return 0;
}
static int job_control(HANDLE pipe, uint32_t type)
{
    wslp_head h = { type, 0, 1 }; DWORD wrote = 0;
    return WriteFile(pipe, &h, sizeof h, &wrote, NULL) && wrote == sizeof h;
}
static wchar_t *job_shown(uint32_t root, const uint8_t *suffix, uint32_t bytes,
                          const wchar_t *const *src, uint32_t count, const wchar_t *dest)
{
    const wchar_t *base = root == 0 ? dest : root <= count ? src[root-1] : NULL;
    if (root == WSLP_NO_ROOT && !bytes) return NULL;
    if (root != WSLP_NO_ROOT && (!base || root > count)) return NULL;
    size_t n = base ? wcslen(base) : 0;
    wchar_t *s = fpe_alloc((n + bytes + 2) * sizeof(wchar_t));
    if (!s) return NULL;
    if (n) memcpy(s, base, n * sizeof(wchar_t));
    uint32_t k = to_wide(suffix,bytes,s+n,base?fpe_wsl_path_kind(base):1);
    for (uint32_t i = 0; i < k; i++) if (s[n+i] == L'/') s[n+i] = L'\\';
    if (n && k && s[n-1] == L'\\' && s[n] == L'\\') { memmove(s+n, s+n+1, (--k)*sizeof(wchar_t)); }
    s[n+k] = 0;
    return s;
}

/* Inventory the held Windows object. Unknown/failed enumeration is not proof
 * that DrvFS can represent it. Even an empty named stream carries identity. */
static fpe_err windows_source_streams(const wchar_t *path)
{
    wchar_t *nt=NULL;fpe_err error=fpe_path_nt(path,&nt);if(error)return error;
    const wchar_t *colon=wcschr(nt+4,L':');
    if(colon&&(colon!=nt+5||wcschr(colon+1,L':'))){fpe_free(nt);return ERROR_NOT_SUPPORTED;}
    HANDLE file=CreateFileW(nt,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,NULL);fpe_free(nt);
    if(file==INVALID_HANDLE_VALUE)return GetLastError();
    error=win_require_unnamed_streams(file);
    CloseHandle(file);return error;
}
static fpe_err windows_source_request(const wslp_source_check *q,const uint8_t *suffix,const wchar_t *const *src,
                                      uint32_t count,const wchar_t *dest,const uint32_t *flags,uint32_t op)
{
    if(q->root>count||!flags[q->root]||!((op<=1||op==WSLP_OP_COPY_FILE)?q->root!=0:op==WSLP_OP_NEWFILE&&q->root==0))return ERROR_INVALID_DATA;
    const wchar_t *base=q->root?src[q->root-1]:dest;if(!base)return ERROR_INVALID_DATA;
    if(!q->path_bytes)return windows_source_streams(base);
    if(suffix[0]!='/'||q->path_bytes<2||memchr(suffix,0,q->path_bytes))return ERROR_INVALID_DATA;
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,(const char *)suffix+1,(int)q->path_bytes-1,NULL,0);
    if(!n)return ERROR_INVALID_NAME;
    size_t length=wcslen(base);wchar_t *path=fpe_alloc((length+(size_t)n+2)*sizeof(wchar_t));if(!path)return ERROR_NOT_ENOUGH_MEMORY;
    memcpy(path,base,length*sizeof(wchar_t));if(length&&path[length-1]!=L'\\')path[length++]=L'\\';
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,(const char *)suffix+1,(int)q->path_bytes-1,path+length,n);path[length+n]=0;
    for(int i=0;i<n;i++)if(path[length+i]==L'/')path[length+i]=L'\\';
    fpe_err error=fpe_relative_valid(path+length,1)?windows_source_streams(path):ERROR_INVALID_NAME;fpe_free(path);return error;
}

/* A blocking pipe reader avoids a Windows timer tick between each streamed
 * frame. The worker owns callbacks, cancellation and the Windows file handle. */
typedef struct job_reader {
    HANDLE pipe,ready,consumed,thread;
    volatile LONG stop;
    uint8_t *buf;
    wslp_head head;
    fpe_err err;
} job_reader;
static DWORD WINAPI job_read(void *arg)
{
    job_reader *r=arg;
    while(!r->stop){
        if(!read_all(r->pipe,&r->head,sizeof r->head))r->err=ERROR_BROKEN_PIPE;
        else if(r->head.size>READ_BUF)r->err=ERROR_INVALID_DATA;
        else if(!read_all(r->pipe,r->buf,r->head.size))r->err=ERROR_BROKEN_PIPE;
        SetEvent(r->ready);
        if(r->err)return 0;
        WaitForSingleObject(r->consumed,INFINITE);
    }
    return 0;
}
static void job_reader_stop(job_reader *r)
{
    InterlockedExchange(&r->stop,1);
    if(r->consumed)SetEvent(r->consumed);
    if(r->thread){CancelSynchronousIo(r->thread);WaitForSingleObject(r->thread,INFINITE);CloseHandle(r->thread);}
    if(r->ready)CloseHandle(r->ready);if(r->consumed)CloseHandle(r->consumed);
    fpe_free(r->buf);
}

typedef struct windows_file {
    HANDLE file,directory,previous;
    BY_HANDLE_FILE_INFORMATION previous_info;
    FILE_BASIC_INFO previous_basic;
    wchar_t *dest,*temp;
    fpe_err err;
    uint32_t replace,active,serial,created;
    FILETIME atime,mtime;
    fpe_win_ancestry *ancestry; /* root-level streamed file: fence via held root */
} windows_file;
static void windows_abort(windows_file *w)
{
    if(w->file&&w->file!=INVALID_HANDLE_VALUE){
        /* Clean up the owned file object, never a name after releasing it. */
        if(w->created){FILE_DISPOSITION_INFO remove={TRUE};SetFileInformationByHandle(w->file,FileDispositionInfo,&remove,sizeof remove);}
        CloseHandle(w->file);
    }
    if(w->directory&&w->directory!=INVALID_HANDLE_VALUE)CloseHandle(w->directory);
    if(w->previous&&w->previous!=INVALID_HANDLE_VALUE)CloseHandle(w->previous);
    fpe_free(w->dest);fpe_free(w->temp);
    uint32_t serial=w->serial;memset(w,0,sizeof *w);w->serial=serial;
}
static int errno_windows(fpe_err err)
{
    switch(err){case 0:return 0;case ERROR_ACCESS_DENIED:case ERROR_WRITE_PROTECT:return 13;
        case ERROR_FILE_EXISTS:case ERROR_ALREADY_EXISTS:return 17;
        case ERROR_FILE_NOT_FOUND:case ERROR_PATH_NOT_FOUND:return 2;
        case ERROR_DISK_FULL:return 28;case ERROR_INVALID_NAME:case ERROR_NO_UNICODE_TRANSLATION:return 22;
        case ERROR_NOT_SUPPORTED:case ERROR_INVALID_FUNCTION:return 95;
        case ERROR_FILE_INVALID:return 116;
        case ERROR_CANCELLED:return 125;default:return 5;}
}
/* A writable directory descriptor is required for FlushFileBuffers. Keep its
 * name pinned while it participates in a barrier, and reject child reparses.
 * The explicitly selected root may be an alias; opening it follows that alias. */
static HANDLE windows_directory_open(const wchar_t *path,int root,fpe_err *error)
{
    HANDLE h=CreateFileW(path,GENERIC_WRITE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS|(root?0:FILE_FLAG_OPEN_REPARSE_POINT),NULL);
    if(h==INVALID_HANDLE_VALUE){*error=GetLastError();return h;}
    BY_HANDLE_FILE_INFORMATION info;
    if(!GetFileInformationByHandle(h,&info))*error=GetLastError();
    else if(!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))*error=ERROR_DIRECTORY;
    else if(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)*error=ERROR_CANT_ACCESS_FILE;
    if(*error){CloseHandle(h);return INVALID_HANDLE_VALUE;}return h;
}
static HANDLE windows_ancestry_directory(fpe_win_ancestry *a,int *exchanged,fpe_err *error)
{
    HANDLE pin=fpe_win_ancestry_root(a);
    *error=fpe_win_ancestry_exchange(a,pin,0);if(*error)return INVALID_HANDLE_VALUE;*exchanged=1;
    HANDLE h=fpe_win_reopen_directory(pin,GENERIC_WRITE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE);
    *error=h==INVALID_HANDLE_VALUE?GetLastError():fpe_win_ancestry_validate(a,h);
    if(*error&&h!=INVALID_HANDLE_VALUE){CloseHandle(h);h=INVALID_HANDLE_VALUE;}return h;
}
static fpe_err windows_directory_sync_guarded(const wslp_directory *d,const uint8_t *suffix,const wchar_t *dest,volatile LONG *stop,fpe_win_ancestry *ancestry)
{
    if(d->root||d->metadata>1||d->pad||(d->metadata&&!d->path_bytes)||(!d->metadata&&(d->atime||d->mtime))||d->path_bytes>=32700||!dest||(d->path_bytes&&suffix[0]!='/')||
       memchr(suffix,0,d->path_bytes)||(d->path_bytes&&!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,(const char *)suffix,(int)d->path_bytes,NULL,0)))return ERROR_INVALID_DATA;
    if(stop&&*stop)return ERROR_CANCELLED;
    wchar_t *path=job_shown(0,suffix,d->path_bytes,NULL,0,dest),*nt=NULL,*parent=NULL;
    if(!path)return ERROR_NOT_ENOUGH_MEMORY;
    /* job_shown is also used for display; validate before using it for I/O. */
    size_t base=wcslen(dest);while(base&&dest[base-1]==L'\\')base--;
    const wchar_t *relative=path+base;while(*relative==L'\\')relative++;
    fpe_err error=(d->path_bytes&&(!*relative||!fpe_relative_valid(relative,1)))?ERROR_INVALID_NAME:fpe_path_nt(path,&nt);
    HANDLE dir=INVALID_HANDLE_VALUE,up=INVALID_HANDLE_VALUE;int exchanged=0;
    if(!error)dir=ancestry&&!d->path_bytes?windows_ancestry_directory(ancestry,&exchanged,&error):windows_directory_open(nt,!d->path_bytes,&error);
    if(!error&&d->path_bytes){
        wchar_t *slash=wcsrchr(path,L'\\');if(!slash)error=ERROR_INVALID_NAME;
        else {int root=(size_t)(slash-path)==base;if(slash==path+2&&path[1]==L':')slash[1]=0;else *slash=0;error=fpe_path_nt(path,&parent);if(!error)up=ancestry&&root?windows_ancestry_directory(ancestry,&exchanged,&error):windows_directory_open(parent,root,&error);}
    }
    if(!error&&d->metadata){
        uint64_t a=(uint64_t)(d->atime/100+116444736000000000LL),m=(uint64_t)(d->mtime/100+116444736000000000LL);
        FILETIME at={(DWORD)a,(DWORD)(a>>32)},mt={(DWORD)m,(DWORD)(m>>32)},got_a={0},got_m={0};
        if(!SetFileTime(dir,NULL,&at,&mt)||!GetFileTime(dir,NULL,&got_a,&got_m))error=GetLastError();
        else if(CompareFileTime(&at,&got_a)||CompareFileTime(&mt,&got_m))error=ERROR_WRITE_FAULT;
    }
    if(!error&&!FlushFileBuffers(dir))error=GetLastError();
    /* Attempt the parent even when the first barrier failed. Preserve the
     * original failure; a successful second flush never turns it into success. */
    if(up!=INVALID_HANDLE_VALUE&&!FlushFileBuffers(up)&&!error)error=GetLastError();
    if(!error&&stop&&*stop)error=ERROR_CANCELLED;
    if(up!=INVALID_HANDLE_VALUE)CloseHandle(up);if(dir!=INVALID_HANDLE_VALUE)CloseHandle(dir);
    if(exchanged){fpe_err e=fpe_win_ancestry_exchange(ancestry,fpe_win_ancestry_root(ancestry),1);if(e&&!error)error=e;}
    fpe_free(path);fpe_free(nt);fpe_free(parent);return error;
}
static fpe_err windows_directory_sync(const wslp_directory *d,const uint8_t *suffix,const wchar_t *dest,volatile LONG *stop)
{return windows_directory_sync_guarded(d,suffix,dest,stop,NULL);}
static void windows_begin(windows_file *w,const wslp_file *f,const uint8_t *suffix,const wchar_t *dest,int sync_directory)
{
    windows_abort(w);w->active=1;w->replace=f->replace;
    /* Raw Linux names which NTFS cannot represent fail explicitly. */
    for(uint32_t i=0;i<f->path_bytes;i++)if(!suffix[i]||suffix[i]<32||strchr("\\:*?\"<>|",suffix[i])){w->err=ERROR_INVALID_NAME;return;}
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,(const char *)suffix,(int)f->path_bytes,NULL,0);
    if(!n){w->err=GetLastError();return;}
    size_t base=wcslen(dest);if(base&&dest[base-1]==L'\\'&&suffix[0]=='/')base--;
    wchar_t *path=fpe_alloc((base+(size_t)n+2)*sizeof(wchar_t));
    if(!path){w->err=ERROR_NOT_ENOUGH_MEMORY;return;}
    memcpy(path,dest,base*sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,(const char *)suffix,(int)f->path_bytes,path+base,n);
    for(int i=0;i<n;i++)if(path[base+i]==L'/')path[base+i]=L'\\';path[base+n]=0;
    w->err=fpe_path_nt(path,&w->dest);fpe_free(path);if(w->err)return;
    if(w->replace){
        /* Retain the version accepted at stream start, including its inode
         * identity. Opening the target only at commit would accept late edits. */
        w->previous=CreateFileW(w->dest,FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,NULL);
        if(w->previous==INVALID_HANDLE_VALUE){w->err=GetLastError();return;}
        if(!GetFileInformationByHandle(w->previous,&w->previous_info)||
           !GetFileInformationByHandleEx(w->previous,FileBasicInfo,&w->previous_basic,sizeof w->previous_basic)){w->err=GetLastError();return;}
        if(w->previous_info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DEVICE)){w->err=ERROR_FILE_INVALID;return;}
    }
    const wchar_t *slash=wcsrchr(w->dest,L'\\');size_t dir=slash?(size_t)(slash-w->dest):0;
    if(sync_directory&&slash){
        size_t cut=dir==6&&w->dest[5]==L':'?dir+1:dir;
        wchar_t saved=w->dest[cut];w->dest[cut]=0;
        int root=memchr(suffix+1,'/',f->path_bytes-1)==NULL;
        w->directory=windows_directory_open(w->dest,root,&w->err);w->dest[cut]=saved;
        if(w->err)return;
    }
    w->temp=fpe_alloc((dir+100)*sizeof(wchar_t));if(!w->temp){w->err=ERROR_NOT_ENOUGH_MEMORY;return;}
    memcpy(w->temp,w->dest,dir*sizeof(wchar_t));
    for(unsigned try=0;try<100;try++){
        _snwprintf(w->temp+dir,100,L"\\.openpilot-host-%lu-%u.tmp",GetCurrentProcessId(),++w->serial);
        w->file=CreateFileW(w->temp,GENERIC_WRITE|FILE_WRITE_ATTRIBUTES|DELETE|(w->replace?WRITE_DAC:0),0,NULL,CREATE_NEW,
                            FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_TEMPORARY,NULL);
        if(w->file!=INVALID_HANDLE_VALUE)break;
        w->err=GetLastError();if(w->err!=ERROR_FILE_EXISTS&&w->err!=ERROR_ALREADY_EXISTS)return;
    }
    if(w->file==INVALID_HANDLE_VALUE){w->temp[0]=0;return;}
    w->err=0;w->created=1;
    uint64_t at=(uint64_t)(f->atime/100+116444736000000000LL),mt=(uint64_t)(f->mtime/100+116444736000000000LL);
    FILETIME a={(DWORD)at,(DWORD)(at>>32)},m={(DWORD)mt,(DWORD)(mt>>32)};
    w->atime=a;w->mtime=m;
}
static fpe_err windows_end(windows_file *w,int abort,volatile LONG *stop)
{
    fpe_err err=w->err;if((abort||(stop&&*stop))&&!err)err=ERROR_CANCELLED;
    if(w->file&&w->file!=INVALID_HANDLE_VALUE){
        if(!err&&!SetFileTime(w->file,NULL,&w->atime,&w->mtime))err=GetLastError();
        FILE_BASIC_INFO basic={0};basic.FileAttributes=FILE_ATTRIBUTE_NORMAL;
        if(!err&&!SetFileInformationByHandle(w->file,FileBasicInfo,&basic,sizeof basic))err=GetLastError();
        /* Replacement retains the accepted target's DACL, including its
         * inheritance protection. Persist security with the completed stage,
         * before publication or a successful source-removal ACK. */
        if(!err&&w->replace)err=win_copy_dacl(w->previous,w->file);
        if(!err&&stop&&*stop)err=ERROR_CANCELLED;
        /* The held writable object includes final data and metadata. A failed
         * flush must not publish it or let the Linux exporter remove source. */
        if(!err&&!FlushFileBuffers(w->file))err=GetLastError();
        if(!err&&stop&&*stop)err=ERROR_CANCELLED;
        if(!err){
            /* Keep the exclusive staging object through publication. A simple
             * leaf rename stays in its actual directory and works while the
             * selected-copy parent guard denies write/delete sharing. */
            const wchar_t *leaf=wcsrchr(w->dest,L'\\');
            HANDLE target=INVALID_HANDLE_VALUE;
            if(w->replace){
                /* Deny new writers while validating and publishing; an open
                 * writer makes this fail rather than discarding its changes.
                 * Delete sharing is required by our held-object replacement. */
                target=CreateFileW(w->dest,DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,NULL);
                if(target==INVALID_HANDLE_VALUE)err=GetLastError();
                else {
                    BY_HANDLE_FILE_INFORMATION a,*b=&w->previous_info;FILE_BASIC_INFO basic;
                    if(!GetFileInformationByHandle(target,&a)||!GetFileInformationByHandleEx(target,FileBasicInfo,&basic,sizeof basic))err=GetLastError();
                    else if(a.dwVolumeSerialNumber!=b->dwVolumeSerialNumber||a.nFileIndexHigh!=b->nFileIndexHigh||a.nFileIndexLow!=b->nFileIndexLow||
                        a.dwFileAttributes!=b->dwFileAttributes||a.nFileSizeHigh!=b->nFileSizeHigh||a.nFileSizeLow!=b->nFileSizeLow||
                        CompareFileTime(&a.ftCreationTime,&b->ftCreationTime)||CompareFileTime(&a.ftLastWriteTime,&b->ftLastWriteTime)||
                        basic.ChangeTime.QuadPart!=w->previous_basic.ChangeTime.QuadPart)err=ERROR_FILE_INVALID;
                }
            }
            if(!err)err=leaf&&leaf[1]?fpe_win_rename_handle(w->file,NULL,leaf+1,w->replace,0):ERROR_INVALID_NAME;
            /* Rename is already visible even if its following flush fails.
             * Cleanup must never mark that completed destination for deletion. */
            if(!err)w->created=0;
            if(target!=INVALID_HANDLE_VALUE)CloseHandle(target);
            if(!err&&!FlushFileBuffers(w->file))err=GetLastError();
            if(!err&&w->directory&&!FlushFileBuffers(w->directory))err=GetLastError();
            if(!err&&w->ancestry){wslp_directory d={0};err=windows_directory_sync_guarded(&d,(const uint8_t*)"",fpe_win_ancestry_path(w->ancestry),stop,w->ancestry);}
            if(!err&&stop&&*stop)err=ERROR_CANCELLED;
        }
    }else if(!err)err=ERROR_INVALID_HANDLE;
    windows_abort(w);return err;
}
#include "winprepare.h"
static int windows_group_requested(const wchar_t *dest);
static int windows_file_reply(HANDLE pipe,uint32_t type,const wslp_file_result *result)
{
    wslp_head h={type,sizeof *result,1};uint8_t data[sizeof h+sizeof *result];DWORD wrote=0;
    memcpy(data,&h,sizeof h);memcpy(data+sizeof h,result,sizeof *result);
    return WriteFile(pipe,data,sizeof data,&wrote,NULL)&&wrote==sizeof data;
}

#include "wingroup.h"

/* Two bounded pipe readers and one synchronous-in-the-worker writer per
 * cross-distro pair. The UI only sees the normal job callbacks. */
static fpe_err cross_hello(HANDLE from,HANDLE process,volatile LONG *stop)
{
    DWORD need=sizeof(wslp_head)+sizeof(wslp_hello);uint64_t begin=GetTickCount64();
    for(;;){
        if(stop&&*stop)return ERROR_CANCELLED;
        DWORD available=0;if(!PeekNamedPipe(from,NULL,0,NULL,&available,NULL))return ERROR_NOT_SUPPORTED;
        if(available>=need)return hello(from,process)?0:ERROR_INVALID_DATA;
        if(WaitForSingleObject(process,2)==WAIT_OBJECT_0)return ERROR_NOT_SUPPORTED;
        if(GetTickCount64()-begin>START_TIMEOUT_MS)return ERROR_TIMEOUT;
        AcquireSRWLockShared(&g_lock);int enabled=g_cfg.enabled;ReleaseSRWLockShared(&g_lock);
        if(!enabled)return ERROR_NOT_SUPPORTED;
    }
}
static fpe_err cross_reader_start(job_reader *r,HANDLE pipe)
{
    r->pipe=pipe;r->buf=fpe_alloc(READ_BUF);
    r->ready=CreateEventW(NULL,FALSE,FALSE,NULL);r->consumed=CreateEventW(NULL,FALSE,FALSE,NULL);
    if(!r->buf||!r->ready||!r->consumed)return ERROR_NOT_ENOUGH_MEMORY;
    r->thread=CreateThread(NULL,64*1024,job_read,r,STACK_SIZE_PARAM_IS_A_RESERVATION,NULL);
    return r->thread?0:GetLastError();
}
static fpe_err cross_wait(job_reader *r,volatile LONG *stop)
{
    uint64_t begin=GetTickCount64();
    for(;;){
        if(stop&&*stop)return ERROR_CANCELLED;
        DWORD wait=WaitForSingleObject(r->ready,10);
        if(wait==WAIT_OBJECT_0)return r->err?r->err:r->head.id!=1?ERROR_INVALID_DATA:0;
        if(wait!=WAIT_TIMEOUT)return ERROR_BROKEN_PIPE;
        if(GetTickCount64()-begin>REQ_TIMEOUT_MS)return ERROR_TIMEOUT;
    }
}
typedef struct cross_writer {
    HANDLE thread,wake,done;volatile LONG quit;job_writer request;
} cross_writer;
static DWORD WINAPI cross_write_loop(void *arg)
{
    cross_writer *w=arg;
    for(;;){WaitForSingleObject(w->wake,INFINITE);if(w->quit)break;job_write(&w->request);SetEvent(w->done);}return 0;
}
static fpe_err cross_writer_start(cross_writer *w)
{
    w->wake=CreateEventW(NULL,FALSE,FALSE,NULL);w->done=CreateEventW(NULL,FALSE,FALSE,NULL);
    if(!w->wake||!w->done)return ERROR_NOT_ENOUGH_MEMORY;
    w->thread=CreateThread(NULL,64*1024,cross_write_loop,w,STACK_SIZE_PARAM_IS_A_RESERVATION,NULL);return w->thread?0:GetLastError();
}
static void cross_writer_stop(cross_writer *w)
{
    if(w->thread){InterlockedExchange(&w->quit,1);SetEvent(w->wake);WaitForSingleObject(w->thread,INFINITE);CloseHandle(w->thread);}
    if(w->wake)CloseHandle(w->wake);if(w->done)CloseHandle(w->done);
}
static fpe_err cross_send(cross_writer *w,HANDLE pipe,HANDLE process,uint32_t type,const void *data,uint32_t size,volatile LONG *stop)
{
    if(stop&&*stop)return ERROR_CANCELLED;
    if(size>READ_BUF)return ERROR_BUFFER_OVERFLOW;
    uint8_t *bytes=fpe_alloc(sizeof(wslp_head)+size);if(!bytes)return ERROR_NOT_ENOUGH_MEMORY;
    wslp_head h={type,size,1};memcpy(bytes,&h,sizeof h);if(size)memcpy(bytes+sizeof h,data,size);
    w->request=(job_writer){pipe,bytes,(DWORD)(sizeof h+size),0};SetEvent(w->wake);
    fpe_err err=0;uint64_t begin=GetTickCount64();
    for(;;){
        DWORD wait=WaitForSingleObject(w->done,10);if(wait==WAIT_OBJECT_0)break;
        if((stop&&*stop)||GetTickCount64()-begin>REQ_TIMEOUT_MS||WaitForSingleObject(process,0)==WAIT_OBJECT_0){
            err=stop&&*stop?ERROR_CANCELLED:ERROR_TIMEOUT;
            /* Repeat cancellation while waiting so a preempted writer cannot
             * issue a fresh blocking WriteFile after a single cancel call. */
            do{CancelSynchronousIo(w->thread);}while(WaitForSingleObject(w->done,10)==WAIT_TIMEOUT);
            break;
        }
    }
    if(!err)err=w->request.err;fpe_free(bytes);return err;
}
static fpe_err cross_request(cross_writer *writer,HANDLE pipe,HANDLE process,uint32_t op,fpe_conflict policy,const char *path,volatile LONG *stop)
{
    size_t n=strlen(path);if(!n||n>=65536)return ERROR_FILENAME_EXCED_RANGE;
    uint8_t *buf=fpe_alloc(sizeof(wslp_transfer)+n);if(!buf)return ERROR_NOT_ENOUGH_MEMORY;
    wslp_transfer request={op,(uint32_t)policy,(uint32_t)n,0};memcpy(buf,&request,sizeof request);memcpy(buf+sizeof request,path,n);
    fpe_err err=cross_send(writer,pipe,process,WSLP_TRANSFER,buf,(uint32_t)(sizeof request+n),stop);fpe_free(buf);return err;
}
static fpe_err cross_pair(uint32_t op,fpe_conflict policy,const wchar_t *source,const wchar_t *dest,int as_root,
                          uint64_t id,fpe_job_cb cb,fpe_job_event_cb events,void *user,volatile LONG *stop)
{
    host source_host={0},dest_host={0};char *source_path=NULL,*dest_path=NULL;
    HANDLE source_to=NULL,source_from=NULL,source_proc=NULL,dest_to=NULL,dest_from=NULL,dest_proc=NULL;
    job_reader sr={0},dr={0};cross_writer writer={0};fpe_err err=0;int finished=0;
    if(!split(source,source_host.name,&source_path)||!split(dest,dest_host.name,&dest_path)){err=ERROR_INVALID_NAME;goto done;}
    if(stop&&*stop){err=ERROR_CANCELLED;goto done;}
    if((err=cross_writer_start(&writer)))goto done;
    if(!launch(&dest_host,as_root,&dest_to,&dest_from,&dest_proc,L" --import")){err=ERROR_NOT_SUPPORTED;goto done;}
    if((err=cross_hello(dest_from,dest_proc,stop)))goto done;
    if((err=cross_reader_start(&dr,dest_from))||(err=cross_request(&writer,dest_to,dest_proc,op,policy,dest_path,stop))||(err=cross_wait(&dr,stop)))goto done;
    if(dr.head.type!=WSLP_TRANSFER_ACK||dr.head.size!=sizeof(wslp_transfer_ack)){err=ERROR_INVALID_DATA;goto done;}
    wslp_transfer_ack ack;memcpy(&ack,dr.buf,sizeof ack);SetEvent(dr.consumed);
    if(ack.err){err=from_errno(ack.err);goto done;}
    if(!launch(&source_host,as_root,&source_to,&source_from,&source_proc,L" --export")){err=ERROR_NOT_SUPPORTED;goto done;}
    if((err=cross_hello(source_from,source_proc,stop)))goto done;
    if((err=cross_reader_start(&sr,source_from))||(err=cross_request(&writer,source_to,source_proc,op,policy,source_path,stop)))goto done;
    const wchar_t *sources[]={source};int continued=0;
    for(;;){
        if((err=cross_wait(&sr,stop)))break;
        wslp_head h=sr.head;uint8_t *buf=sr.buf;
        if(h.type==WSLP_JOB_PROGRESS&&h.size>=sizeof(wslp_progress)){
            wslp_progress p;memcpy(&p,buf,sizeof p);
            if(p.state>FPE_JOB_FAILED||p.path_bytes!=h.size-sizeof p||(p.root!=WSLP_NO_ROOT&&p.root>1)){err=ERROR_INVALID_DATA;break;}
            wchar_t *path=job_shown(p.root,buf+sizeof p,p.path_bytes,sources,1,dest);
            fpe_job_progress v={.job=id,.op=(fpe_op)op,.state=(fpe_job_state)p.state,.bytes_done=p.bytes_done,.bytes_total=p.bytes_total,
                .items_done=p.items_done,.items_total=p.items_total,.errors=p.errors,.skipped=p.skipped,.last_error=from_errno(p.error),.current=path};
            if(cb)cb(user,&v);fpe_free(path);
            if(p.state==FPE_JOB_RUNNING&&!continued){continued=1;if((err=cross_send(&writer,source_to,source_proc,WSLP_JOB_CONTINUE,NULL,0,stop)))break;}
            if(p.state>=FPE_JOB_DONE){finished=1;break;}
        }else if(h.type==WSLP_JOB_EVENT&&h.size>=sizeof(wslp_event)){
            wslp_event p;memcpy(&p,buf,sizeof p);if(p.kind>FPE_JOB_SKIPPED||p.root>1||p.path_bytes!=h.size-sizeof p){err=ERROR_INVALID_DATA;break;}
            wchar_t *path=job_shown(p.root,buf+sizeof p,p.path_bytes,sources,1,dest);fpe_job_event v={id,p.kind,from_errno(p.error),path};if(events)events(user,&v);fpe_free(path);
        }else if(h.type==WSLP_TRANSFER_NODE||h.type==WSLP_TRANSFER_DATA||h.type==WSLP_TRANSFER_END||h.type==WSLP_TRANSFER_DIR_END){
            if((h.type==WSLP_TRANSFER_NODE&&h.size<sizeof(wslp_node))||(h.type==WSLP_TRANSFER_DATA&&(!h.size||h.size>WSLP_TRANSFER_CHUNK))||
               (h.type==WSLP_TRANSFER_END&&h.size!=sizeof(wslp_transfer_end))||(h.type==WSLP_TRANSFER_DIR_END&&h.size)){err=ERROR_INVALID_DATA;break;}
            if((err=cross_send(&writer,dest_to,dest_proc,h.type,buf,h.size,stop)))break;
            if(h.type!=WSLP_TRANSFER_DATA){
                if((err=cross_wait(&dr,stop)))break;
                if(dr.head.type!=WSLP_TRANSFER_ACK||dr.head.size!=sizeof ack){err=ERROR_INVALID_DATA;break;}
                memcpy(&ack,dr.buf,sizeof ack);SetEvent(dr.consumed);
                if(ack.skipped>1||ack.committed>1){err=ERROR_INVALID_DATA;break;}
                if((err=cross_send(&writer,source_to,source_proc,WSLP_TRANSFER_ACK,&ack,sizeof ack,stop)))break;
            }
        }else{err=ERROR_INVALID_DATA;break;}
        SetEvent(sr.consumed);
    }
    if(finished&&!err)err=cross_send(&writer,dest_to,dest_proc,WSLP_TRANSFER_QUIT,NULL,0,NULL);
done:
    /* EOF is the importer's cooperative abort: remove its unfinished sibling
     * before exiting. Closing both pipes also releases a source waiting for ACK. */
    cross_writer_stop(&writer);
    if(source_to)CloseHandle(source_to);if(dest_to)CloseHandle(dest_to);
    job_reader_stop(&sr);job_reader_stop(&dr);
    if(source_from)CloseHandle(source_from);if(dest_from)CloseHandle(dest_from);
    HANDLE processes[2];DWORD n=0;if(source_proc)processes[n++]=source_proc;if(dest_proc)processes[n++]=dest_proc;
    if(n&&WaitForMultipleObjects(n,processes,TRUE,3000)==WAIT_TIMEOUT)for(DWORD i=0;i<n;i++)if(WaitForSingleObject(processes[i],0)==WAIT_TIMEOUT)TerminateProcess(processes[i],1);
    for(DWORD i=0;i<n;i++)CloseHandle(processes[i]);
    fpe_free(source_path);fpe_free(dest_path);return err;
}

static fpe_err job_native(uint32_t op, fpe_conflict policy, const wchar_t *const *src, uint32_t count,
                    const wchar_t *dest, uint64_t id, fpe_job_cb cb, fpe_job_event_cb events, void *user,
                    volatile LONG *stop, int as_root,fpe_win_ancestry *ancestry)
{
    if (!src || !count || count > 100000 || op > WSLP_OP_COPY_FILE || policy > FPE_CONFLICT_FAIL) return ERROR_INVALID_PARAMETER;
    if (!have_helper()) return ERROR_NOT_SUPPORTED;
    host h = { 0 }; char *probe = NULL;
    if (!dest || !split(dest, h.name, &probe)) {
        for (uint32_t i = 0; i < count && !h.name[0]; i++) split(src[i], h.name, &probe);
    }
    fpe_free(probe);
    if (!h.name[0]) return ERROR_NOT_SUPPORTED;
    char **paths = fpe_zalloc(((size_t)count + 1) * sizeof(char *));
    uint32_t *flags = fpe_zalloc(((size_t)count + 1) * sizeof(uint32_t));
    uint8_t *packet = NULL; HANDLE to = NULL, from = NULL, proc = NULL, writer = NULL;
    fpe_err err = 0; int finished = 0, cancel_sent = 0, continued = 0;
    windows_file exporting={0};
    windows_prepare preparing={0};
    job_reader reader={0};
    if (!paths || !flags) { err = ERROR_NOT_ENOUGH_MEMORY; goto done; }
    size_t total = sizeof(wslp_head) + sizeof(wslp_job);
    for (uint32_t i = 0; i <= count; i++) {
        const wchar_t *path = i ? src[i-1] : dest;
        if (!path || !path[0]) {
            paths[i] = fpe_zalloc(1);
        } else {
            SetLastError(0); paths[i] = job_path(path, h.name, flags+i);
        }
        if (!paths[i]) { err = GetLastError(); if (!err) err = ERROR_NOT_ENOUGH_MEMORY; goto done; }
        total += sizeof(wslp_path) + strlen(paths[i]);
        if (total > WSLP_JOB_MAX + sizeof(wslp_head)) { err = ERROR_BUFFER_OVERFLOW; goto done; }
    }
    packet = fpe_alloc(total);
    if (!packet) { err = ERROR_NOT_ENOUGH_MEMORY; goto done; }
    wslp_head head = { WSLP_JOB, (uint32_t)(total-sizeof head), 1 };
    preparing.width=op==0&&flags[0]?windows_prepare_width():0;
    preparing.group=preparing.width&&windows_group_requested(dest);
    wslp_job request = { op, (uint32_t)policy, count, preparing.width|(preparing.group?WSLP_GROUP_FLAG:0) };
    memcpy(packet, &head, sizeof head); memcpy(packet+sizeof head, &request, sizeof request);
    uint8_t *p = packet+sizeof head+sizeof request;
    for (uint32_t i = 0; i <= count; i++) {
        wslp_path field = { (uint32_t)strlen(paths[i]), flags[i] };
        memcpy(p,&field,sizeof field);p+=sizeof field;memcpy(p,paths[i],field.bytes);p+=field.bytes;
    }
    if (stop && *stop) { err = ERROR_CANCELLED; goto done; }
    if (!launch(&h, as_root, &to, &from, &proc, L" --jobs") || !hello(from,proc)) { err = ERROR_NOT_SUPPORTED; goto done; }
    job_writer w = { to, packet, (DWORD)total, 0 };
    writer = CreateThread(NULL,64*1024,job_write,&w,STACK_SIZE_PARAM_IS_A_RESERVATION,NULL);
    if (!writer) { err = GetLastError(); goto done; }
    uint64_t sent_at = GetTickCount64();
    while (WaitForSingleObject(writer,10) == WAIT_TIMEOUT) {
        if ((stop && *stop) || GetTickCount64()-sent_at > START_TIMEOUT_MS || WaitForSingleObject(proc,0)==WAIT_OBJECT_0) {
            CancelSynchronousIo(writer); TerminateProcess(proc,1);
            err = stop && *stop ? ERROR_CANCELLED : ERROR_TIMEOUT; break;
        }
    }
    WaitForSingleObject(writer,INFINITE); CloseHandle(writer); writer = NULL;
    if (err || w.err) { if (!err) err = w.err; goto done; }
    uint64_t last = GetTickCount64();
    reader.pipe=from;reader.buf=fpe_alloc(READ_BUF);
    reader.ready=CreateEventW(NULL,FALSE,FALSE,NULL);reader.consumed=CreateEventW(NULL,FALSE,FALSE,NULL);
    if(!reader.buf||!reader.ready||!reader.consumed){err=ERROR_NOT_ENOUGH_MEMORY;goto done;}
    reader.thread=CreateThread(NULL,64*1024,job_read,&reader,STACK_SIZE_PARAM_IS_A_RESERVATION,NULL);
    if(!reader.thread){err=GetLastError();goto done;}
    uint8_t *buf=reader.buf;
    for (;;) {
        if (stop && *stop && !cancel_sent) {
            if (!job_control(to,WSLP_JOB_CANCEL)) { err=ERROR_BROKEN_PIPE; break; }
            cancel_sent=1;
        }
        if(WaitForSingleObject(reader.ready,10)==WAIT_TIMEOUT){
            if(GetTickCount64()-last>REQ_TIMEOUT_MS){err=ERROR_TIMEOUT;break;}continue;
        }
        if(reader.err){err=reader.err;break;}
        wslp_head reply=reader.head;
        if(reply.id!=1){err=ERROR_INVALID_DATA;break;}
        last=GetTickCount64();
        if (reply.type == WSLP_JOB_PROGRESS && reply.size >= sizeof(wslp_progress)) {
            wslp_progress v;memcpy(&v,buf,sizeof v);
            if (v.state>FPE_JOB_FAILED || v.path_bytes!=reply.size-sizeof v || (v.root!=WSLP_NO_ROOT && v.root>count)) {err=ERROR_INVALID_DATA;break;}
            wchar_t *path=job_shown(v.root,buf+sizeof v,v.path_bytes,src,count,dest);
            fpe_job_progress progress = { .job=id, .op=(fpe_op)op, .state=(fpe_job_state)v.state,
                .bytes_done=v.bytes_done,.bytes_total=v.bytes_total,.items_done=v.items_done,.items_total=v.items_total,
                .errors=v.errors,.skipped=v.skipped,.last_error=from_errno(v.error),.current=path };
            if(cb)cb(user,&progress);fpe_free(path);
            if(v.state==FPE_JOB_RUNNING && !continued) {
                continued=1;
                if(!cancel_sent && stop && *stop) cancel_sent=1,job_control(to,WSLP_JOB_CANCEL);
                else if(!cancel_sent && !job_control(to,WSLP_JOB_CONTINUE)){err=ERROR_BROKEN_PIPE;break;}
            }
            if(v.state>=FPE_JOB_DONE){if(exporting.active||preparing.count){err=ERROR_INVALID_DATA;break;}finished=1;break;}
        } else if(reply.type==WSLP_JOB_EVENT && reply.size>=sizeof(wslp_event)) {
            wslp_event v;memcpy(&v,buf,sizeof v);
            if(v.kind>FPE_JOB_SKIPPED || v.path_bytes!=reply.size-sizeof v || (v.root!=WSLP_NO_ROOT && v.root>count)){err=ERROR_INVALID_DATA;break;}
            wchar_t *path=job_shown(v.root,buf+sizeof v,v.path_bytes,src,count,dest);
            fpe_job_event event={id,v.kind,from_errno(v.error),path};if(events)events(user,&event);fpe_free(path);
        } else if(reply.type==WSLP_SOURCE_CHECK&&reply.size>=sizeof(wslp_source_check)){
            wslp_source_check q;memcpy(&q,buf,sizeof q);
            if(q.path_bytes!=reply.size-sizeof q){err=ERROR_INVALID_DATA;break;}
            int32_t result=errno_windows(stop&&*stop?ERROR_CANCELLED:windows_source_request(&q,buf+sizeof q,src,count,dest,flags,op));
            wslp_head ack={WSLP_SOURCE_ACK,sizeof result,1};uint8_t data[sizeof ack+sizeof result];DWORD wrote=0;
            memcpy(data,&ack,sizeof ack);memcpy(data+sizeof ack,&result,sizeof result);
            if(!WriteFile(to,data,sizeof data,&wrote,NULL)||wrote!=sizeof data){err=ERROR_BROKEN_PIPE;break;}
        } else if(reply.type==WSLP_FILE_PREPARE&&reply.size>=sizeof(wslp_prepared_file)){
            wslp_prepared_file f;memcpy(&f,buf,sizeof f);
            if(!dest||!flags[0]||op||f.file.root||!f.file.path_bytes||f.file.path_bytes!=reply.size-sizeof f||buf[sizeof f]!='/'){err=ERROR_INVALID_DATA;break;}
            int guarded=ancestry&&memchr(buf+sizeof f+1,'/',f.file.path_bytes-1)==NULL;
            err=windows_prepare_begin(&preparing,&exporting,&f,buf+sizeof f,dest,!guarded);
            if(err)break;if(guarded)exporting.ancestry=ancestry;
        } else if(reply.type==WSLP_GROUP_COMMIT){
            if(exporting.active){err=ERROR_INVALID_DATA;break;}
            if((err=windows_group_commit(&preparing,buf,reply.size,to,&reader,stop,&cancel_sent)))break;
        } else if(reply.type==WSLP_FILE_COMMIT&&reply.size==sizeof(wslp_file_result)){
            if(preparing.group){err=ERROR_INVALID_DATA;break;}
            wslp_file_result command,result;memcpy(&command,buf,sizeof command);
            if(exporting.active){err=ERROR_INVALID_DATA;break;}
            if((err=windows_prepare_commit(&preparing,&command,stop,&result)))break;
            if(!windows_file_reply(to,WSLP_FILE_COMMITTED,&result)){err=ERROR_BROKEN_PIPE;break;}
        } else if(reply.type==WSLP_FILE_BEGIN&&reply.size>=sizeof(wslp_file)){
            wslp_file f;memcpy(&f,buf,sizeof f);
            if(exporting.active||preparing.count||!dest||!flags[0]||f.root||f.replace>1||f.path_bytes!=reply.size-sizeof f||!f.path_bytes||buf[sizeof f]!='/'){err=ERROR_INVALID_DATA;break;}
            /* Selected regular-only copies keep their existing read guards
             * and file-flush contract; they never remove a source. */
            /* Keep the captured endpoint's read barrier through data writes.
             * Root-level files use a handle-relative flush handoff only at
             * commit; no writable root handle spans progress callbacks. */
            int guarded=ancestry&&op<=1&&memchr(buf+sizeof f+1,'/',f.path_bytes-1)==NULL;
            windows_begin(&exporting,&f,buf+sizeof f,dest,op<=1&&!guarded);
            if(guarded)exporting.ancestry=ancestry;
        } else if(reply.type==WSLP_FILE_DATA){
            if(!exporting.active||reply.size>65536){err=ERROR_INVALID_DATA;break;}
            if((err=windows_prepare_data(&preparing,reply.size)))break;
            if(!exporting.err&&!(stop&&*stop)){
                for(DWORD at=0;at<reply.size;){DWORD wrote=0;
                    if(!WriteFile(exporting.file,buf+at,reply.size-at,&wrote,NULL)||!wrote){exporting.err=GetLastError();if(!exporting.err)exporting.err=ERROR_WRITE_FAULT;break;}at+=wrote;
                }
            }
        } else if(reply.type==WSLP_FILE_END&&reply.size==sizeof(int32_t)){
            if(!exporting.active){err=ERROR_INVALID_DATA;break;}
            int32_t source_error;memcpy(&source_error,buf,sizeof source_error);
            if(preparing.active_token){
                if(source_error<0||source_error>4095){err=ERROR_INVALID_DATA;break;}
                wslp_file_result result=windows_prepare_end(&preparing,&exporting,source_error,stop);
                if(!windows_file_reply(to,WSLP_FILE_PREPARED,&result)){err=ERROR_BROKEN_PIPE;break;}
                SetEvent(reader.consumed);continue;
            }
            int32_t result=errno_windows(windows_end(&exporting,source_error,stop));
            wslp_head ack={WSLP_FILE_ACK,sizeof result,1};DWORD wrote=0;uint8_t data[sizeof ack+sizeof result];
            memcpy(data,&ack,sizeof ack);memcpy(data+sizeof ack,&result,sizeof result);
            if(!WriteFile(to,data,sizeof data,&wrote,NULL)||wrote!=sizeof data){err=ERROR_BROKEN_PIPE;break;}
        } else if(reply.type==WSLP_DIRECTORY_SYNC&&reply.size>=sizeof(wslp_directory)){
            wslp_directory d;memcpy(&d,buf,sizeof d);
            if(exporting.active||preparing.count||!dest||!flags[0]||op>1||d.path_bytes!=reply.size-sizeof d){err=ERROR_INVALID_DATA;break;}
            int32_t result=errno_windows(ancestry?windows_directory_sync_guarded(&d,buf+sizeof d,dest,stop,ancestry):windows_directory_sync(&d,buf+sizeof d,dest,stop));
            wslp_head ack={WSLP_DIRECTORY_ACK,sizeof result,1};uint8_t data[sizeof ack+sizeof result];DWORD wrote=0;
            memcpy(data,&ack,sizeof ack);memcpy(data+sizeof ack,&result,sizeof result);
            if(!WriteFile(to,data,sizeof data,&wrote,NULL)||wrote!=sizeof data){err=ERROR_BROKEN_PIPE;break;}
        } else {err=ERROR_INVALID_DATA;break;}
        SetEvent(reader.consumed);
    }
done:
    job_reader_stop(&reader);
    windows_abort(&exporting);
    windows_prepare_abort(&preparing);
    if(writer){CancelSynchronousIo(writer);if(proc)TerminateProcess(proc,1);WaitForSingleObject(writer,INFINITE);CloseHandle(writer);}
    if(to)CloseHandle(to);if(from)CloseHandle(from);
    if(proc){if(!finished || WaitForSingleObject(proc,3000)==WAIT_TIMEOUT)TerminateProcess(proc,1);CloseHandle(proc);}
    if(paths)for(uint32_t i=0;i<=count;i++)fpe_free(paths[i]);
    fpe_free(paths);fpe_free(flags);fpe_free(packet);
    return err;
}

typedef struct cross_batch {
    fpe_job_progress prior,last;
    fpe_job_cb cb;fpe_job_event_cb events;void *user;
} cross_batch;
static void cross_progress(void *user,const fpe_job_progress *p)
{
    cross_batch *b=user;b->last=*p;b->last.current=NULL;
    fpe_job_progress v=*p;v.bytes_done+=b->prior.bytes_done;v.bytes_total+=b->prior.bytes_total;
    v.items_done+=b->prior.items_done;v.items_total+=b->prior.items_total;v.errors+=b->prior.errors;v.skipped+=b->prior.skipped;
    if(p->state>=FPE_JOB_DONE)v.state=FPE_JOB_RUNNING;
    if(b->cb)b->cb(b->user,&v);
}
static void cross_event(void *user,const fpe_job_event *p){cross_batch *b=user;if(b->events)b->events(b->user,p);}
fpe_err fpe_wsl_job_guarded(uint32_t op,fpe_conflict policy,const wchar_t *const *src,uint32_t count,const wchar_t *dest,
                   uint64_t id,fpe_job_cb cb,fpe_job_event_cb events,void *user,volatile LONG *stop,int as_root,fpe_win_ancestry *ancestry)
{
    if(!src||!count||count>100000||op>WSLP_OP_COPY_FILE||policy>FPE_CONFLICT_FAIL)return ERROR_INVALID_PARAMETER;
    for(uint32_t i=0;i<count;i++)if(!src[i]||!src[i][0])return ERROR_INVALID_PARAMETER;
    int enabled,current_root;fpe_wsl_job_policy(&enabled,&current_root);
    if(!enabled)return ERROR_NOT_SUPPORTED;
    wchar_t distro[64];char *path=NULL;int cross=0;
    if((op<=1||op==WSLP_OP_COPY_FILE)&&dest&&split(dest,distro,&path)){
        fpe_free(path);path=NULL;
        for(uint32_t i=0;i<count&&!cross;i++){wchar_t other[64];if(split(src[i],other,&path)){cross=_wcsicmp(distro,other)!=0;fpe_free(path);path=NULL;}}
    }
    if(!cross)return job_native(op,policy,src,count,dest,id,cb,events,user,stop,as_root,ancestry);
    if(!have_helper())return ERROR_NOT_SUPPORTED;
    cross_batch batch={.prior={.job=id,.op=(fpe_op)op},.cb=cb,.events=events,.user=user};
    for(uint32_t i=0;i<count&&!(stop&&*stop);i++){
        wchar_t other[64];int remote=split(src[i],other,&path)&&_wcsicmp(distro,other)!=0;fpe_free(path);path=NULL;
        batch.last=(fpe_job_progress){.job=id,.op=(fpe_op)op};const wchar_t *single[]={src[i]};
        fpe_err err=remote?cross_pair(op,policy,src[i],dest,as_root,id,cross_progress,cross_event,&batch,stop)
            :job_native(op,policy,single,1,dest,id,cross_progress,cross_event,&batch,stop,as_root,ancestry);
        if(err&&err!=ERROR_CANCELLED){if(!batch.last.items_total)batch.last.items_total=1;if(!batch.last.items_done)batch.last.items_done=1;batch.last.errors++;batch.last.last_error=err;fpe_job_event event={id,FPE_JOB_ERROR,err,src[i]};if(events)events(user,&event);}
        batch.prior.bytes_done+=batch.last.bytes_done;batch.prior.bytes_total+=batch.last.bytes_total;
        batch.prior.items_done+=batch.last.items_done;batch.prior.items_total+=batch.last.items_total;
        batch.prior.errors+=batch.last.errors;batch.prior.skipped+=batch.last.skipped;if(batch.last.last_error)batch.prior.last_error=batch.last.last_error;
        if(err==ERROR_CANCELLED)break;
    }
    batch.prior.state=stop&&*stop?FPE_JOB_CANCELLED:FPE_JOB_DONE;
    if(cb)cb(user,&batch.prior);return 0;
}
fpe_err fpe_wsl_job(uint32_t op,fpe_conflict policy,const wchar_t *const *src,uint32_t count,const wchar_t *dest,
                   uint64_t id,fpe_job_cb cb,fpe_job_event_cb events,void *user,volatile LONG *stop,int as_root)
{return fpe_wsl_job_guarded(op,policy,src,count,dest,id,cb,events,user,stop,as_root,NULL);}

typedef struct mutation_result { fpe_err err; } mutation_result;
static void mutation_progress(void *user,const fpe_job_progress *p)
{
    mutation_result *r=user;
    if(p->errors || p->state==FPE_JOB_FAILED)r->err=p->last_error?p->last_error:ERROR_GEN_FAILURE;
    if(p->state==FPE_JOB_CANCELLED)r->err=ERROR_CANCELLED;
}
FPE_API fpe_err fpe_wsl_rename(const wchar_t *from,const wchar_t *to)
{
    const wchar_t *paths[]={from};mutation_result r={0};
    fpe_err err=fpe_wsl_job(WSLP_OP_RENAME,FPE_CONFLICT_FAIL,paths,1,to,0,mutation_progress,NULL,&r,NULL,fpe_wsl_root());
    return err?err:r.err;
}
FPE_API fpe_err fpe_wsl_create(const wchar_t *path,int directory,const wchar_t *template_path)
{
    const wchar_t *paths[]={path};mutation_result r={0};
    fpe_err err=fpe_wsl_job(directory?WSLP_OP_MKDIR:WSLP_OP_NEWFILE,FPE_CONFLICT_FAIL,paths,1,template_path,0,mutation_progress,NULL,&r,NULL,fpe_wsl_root());
    return err?err:r.err;
}

FPE_API int fpe_wsl_symlink_compatible(const wchar_t *source,const wchar_t *destination)
{
    if(!source||!destination||!fpe_wsl_native(destination))return 0;
    wchar_t *d=unc_form(destination),*s=unc_form(source);int compatible=0;
    if(d&&s){
        const wchar_t *dp=d+prefix_len(d),*sp=s+prefix_len(s);int dn=distro_len(dp),sn=distro_len(sp);
        const wchar_t *tail=sp+sn;while(*tail==L'\\')tail++;
        compatible=dn==sn&&!_wcsnicmp(dp,sp,dn)&&*tail;
    }else if(d){
        wchar_t c=source[0]|32;
        compatible=c>=L'a'&&c<=L'z'&&source[1]==L':'&&(source[2]==L'\\'||source[2]==L'/')&&source[3];
    }
    fpe_free(d);fpe_free(s);return compatible;
}
FPE_API fpe_err fpe_wsl_symlink(const wchar_t *source,const wchar_t *output,int relative)
{
    if(!fpe_wsl_symlink_compatible(source,output))return ERROR_NOT_SUPPORTED;
    const wchar_t *paths[]={source};mutation_result r={0};
    fpe_err err=fpe_wsl_job(relative?WSLP_OP_SYMLINK_RELATIVE:WSLP_OP_SYMLINK_ABSOLUTE,
        FPE_CONFLICT_FAIL,paths,1,output,0,mutation_progress,NULL,&r,NULL,fpe_wsl_root());
    return err?err:r.err;
}

/* Worker-scoped metadata RPC. Literal paths/names travel in binary messages;
 * only the captured distro/user selects the fixed helper executable. */
typedef struct metadata_reader {
    HANDLE pipe,wake,ready,thread;
    volatile LONG stop;
    wslp_head expected;
    uint8_t data[sizeof(wslp_permission_result)];
    fpe_err err;
} metadata_reader;
static DWORD WINAPI metadata_read(void *arg)
{
    metadata_reader *r=arg;
    for(;;){
        WaitForSingleObject(r->wake,INFINITE);
        if(InterlockedCompareExchange(&r->stop,0,0))return 0;
        wslp_head head;
        if(!read_all(r->pipe,&head,sizeof head))r->err=ERROR_BROKEN_PIPE;
        else if(head.type!=r->expected.type||head.id!=r->expected.id||head.size!=r->expected.size)r->err=ERROR_INVALID_DATA;
        else if(!read_all(r->pipe,r->data,head.size))r->err=ERROR_BROKEN_PIPE;
        SetEvent(r->ready);
        if(r->err)return 0;
    }
}
static void metadata_reader_stop(metadata_reader *r)
{
    InterlockedExchange(&r->stop,1);
    if(r->wake)SetEvent(r->wake);
    if(r->thread){
        /* Cancellation can race entry into either ReadFile. Repeat until the
         * reader has exited, before releasing its pipe or session storage. */
        do{CancelSynchronousIo(r->thread);}while(WaitForSingleObject(r->thread,10)==WAIT_TIMEOUT);
        CloseHandle(r->thread);
    }
    if(r->wake)CloseHandle(r->wake);if(r->ready)CloseHandle(r->ready);
    memset(r,0,sizeof *r);
}
static fpe_err metadata_reader_start(metadata_reader *r,HANDLE pipe)
{
    r->pipe=pipe;
    r->wake=CreateEventW(NULL,FALSE,FALSE,NULL);r->ready=CreateEventW(NULL,FALSE,FALSE,NULL);
    if(r->wake&&r->ready)r->thread=CreateThread(NULL,64*1024,metadata_read,r,STACK_SIZE_PARAM_IS_A_RESERVATION,NULL);
    if(r->thread)return 0;
    fpe_err err=GetLastError();metadata_reader_stop(r);return err?err:ERROR_NOT_ENOUGH_MEMORY;
}
static fpe_err metadata_receive(metadata_reader *r,uint32_t type,uint32_t id,void *reply,uint32_t size,DWORD timeout)
{
    if(size>sizeof r->data)return ERROR_INVALID_DATA;
    if(r->err)return r->err;
    r->expected=(wslp_head){type,size,id};
    if(!SetEvent(r->wake))return GetLastError();
    DWORD wait=WaitForSingleObject(r->ready,timeout);
    if(wait!=WAIT_OBJECT_0)return wait==WAIT_TIMEOUT?ERROR_TIMEOUT:GetLastError();
    if(r->err)return r->err;
    memcpy(reply,r->data,size);return 0;
}
struct fpe_wsl_metadata {
    wchar_t distro[64];
    int as_root;
    fpe_err failed;
    uint32_t next_id;
    HANDLE to,from,proc;
    metadata_reader reader;
};
static void metadata_disconnect(fpe_wsl_metadata *s)
{
    metadata_reader_stop(&s->reader);
    if(s->to){CloseHandle(s->to);s->to=NULL;}
    if(s->from){CloseHandle(s->from);s->from=NULL;}
    if(s->proc){if(WaitForSingleObject(s->proc,250)==WAIT_TIMEOUT)TerminateProcess(s->proc,1);CloseHandle(s->proc);s->proc=NULL;}
    s->distro[0]=0;
}
FPE_API fpe_wsl_metadata *fpe_wsl_metadata_begin(int as_root)
{
    fpe_wsl_metadata *s=fpe_alloc(sizeof *s);
    if(s){memset(s,0,sizeof *s);s->as_root=!!as_root;}return s;
}
FPE_API void fpe_wsl_metadata_end(fpe_wsl_metadata *s)
{
    if(s){metadata_disconnect(s);fpe_free(s);}
}
static fpe_err permission_wire(fpe_wsl_metadata *session,const wchar_t *context,int as_root,uint32_t type,
    void *q,uint32_t qsize,uint32_t *path_bytes,uint32_t reply_type,void *reply,uint32_t reply_size)
{
    if(!fpe_wsl_native(context)||!have_helper())return ERROR_NOT_SUPPORTED;
    fpe_wsl_metadata cold={.as_root=!!as_root},*s=session?session:&cold;
    if(s->failed)return s->failed;
    host h={0};char *bytes=NULL;if(!split(context,h.name,&bytes))return ERROR_NOT_SUPPORTED;
    size_t n=path_bytes?strlen(bytes):0;fpe_err err=ERROR_NOT_SUPPORTED;
    if(path_bytes&&(!n||n>16384)){err=ERROR_FILENAME_EXCED_RANGE;goto done;}
    if(path_bytes)*path_bytes=(uint32_t)n;
    if(s->proc&&_wcsicmp(s->distro,h.name))metadata_disconnect(s);
    if(!s->proc){
        if(!launch(&h,s->as_root,&s->to,&s->from,&s->proc,L" --permissions")||!hello(s->from,s->proc))goto broken;
        wcscpy(s->distro,h.name);
        err=metadata_reader_start(&s->reader,s->from);if(err)goto broken;
    }
    uint32_t id=++s->next_id;if(!id)id=++s->next_id;
    wslp_head head={type,(uint32_t)(qsize+n),id};DWORD wrote=0;
    if(!WriteFile(s->to,&head,sizeof head,&wrote,NULL)||wrote!=sizeof head||
       !WriteFile(s->to,q,qsize,&wrote,NULL)||wrote!=qsize||
       (n&&(!WriteFile(s->to,bytes,(DWORD)n,&wrote,NULL)||wrote!=n))){err=ERROR_BROKEN_PIPE;goto broken;}
    /* Wake directly when a complete, validated reply arrives. A worker-scoped
     * reader removes a Windows timer tick from each sequential metadata RPC. */
    err=metadata_receive(&s->reader,reply_type,id,reply,reply_size,REQ_TIMEOUT_MS);
    if(err)goto broken;
    goto done;
broken:
    s->failed=err;metadata_disconnect(s);
done:
    if(!session)metadata_disconnect(s);
    fpe_free(bytes);return err;
}
static wslp_permission_state permission_expected(const fpe_permissions *s)
{
    return (wslp_permission_state){s->device,s->inode,s->ctime_sec,s->ctime_nsec,s->mode,s->uid,s->gid};
}
static fpe_err permission_unpack(const wslp_permission_result *r,fpe_permissions *out,int *changed)
{
    if(r->changed>1||r->owner[127]||r->group[127])return ERROR_INVALID_DATA;
    if(changed)*changed=(int)r->changed;
    out->device=r->state.device;out->inode=r->state.inode;out->ctime_sec=r->state.ctime_sec;out->ctime_nsec=r->state.ctime_nsec;
    out->mode=r->state.mode;out->uid=r->state.uid;out->gid=r->state.gid;
    uint32_t len=to_wide((uint8_t*)r->owner,(uint32_t)strlen(r->owner),out->owner,0);out->owner[len]=0;
    len=to_wide((uint8_t*)r->group,(uint32_t)strlen(r->group),out->group,0);out->group[len]=0;
    return from_errno(r->err);
}
static fpe_err metadata_permissions(fpe_wsl_metadata *session,const wchar_t *path,int as_root,
    const fpe_permissions *expected,uint32_t mode,fpe_permissions *out,int *changed)
{
    if(changed)*changed=0;if(!path||!out||mode>07777)return ERROR_INVALID_PARAMETER;
    memset(out,0,sizeof *out);wslp_permission_request q={.apply=expected!=NULL,.mode=mode};
    if(expected)q.expected=permission_expected(expected);wslp_permission_result r={0};
    fpe_err err=permission_wire(session,path,as_root,WSLP_PERMISSIONS,&q,sizeof q,&q.path_bytes,WSLP_PERMISSION_RESULT,&r,sizeof r);
    if(!err)err=permission_unpack(&r,out,changed);
    if(session&&err==ERROR_INVALID_DATA){session->failed=err;metadata_disconnect(session);}
    return err;
}
FPE_API fpe_err fpe_wsl_identity(const wchar_t *context,int as_root,
    const wchar_t *owner,const wchar_t *group,fpe_identity *out,uint32_t *bad_field)
{
    if(bad_field)*bad_field=0;if(!context||!out||!owner||!group)return ERROR_INVALID_PARAMETER;
    memset(out,0,sizeof *out);wslp_identity_request q={0};wslp_identity_result r={0};
    const wchar_t *inputs[]={owner,group};char *targets[]={q.owner,q.group};
    for(int i=0;i<2;i++)if(inputs[i][0]){
        if(wcslen(inputs[i])>127){if(bad_field)*bad_field=1u<<i;return ERROR_FILENAME_EXCED_RANGE;}
        int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,inputs[i],-1,targets[i],128,NULL,NULL);
        if(!bytes){if(bad_field)*bad_field=1u<<i;return ERROR_INVALID_PARAMETER;}q.fields|=1u<<i;
    }
    if(!q.fields)return ERROR_INVALID_PARAMETER;
    fpe_err err=permission_wire(NULL,context,as_root,WSLP_IDENTITY_RESOLVE,&q,sizeof q,NULL,WSLP_IDENTITY_RESULT,&r,sizeof r);
    if(err)return err;
    if(r.fields!=q.fields||r.pad||r.bad_field>2||r.owner[127]||r.group[127]||
       (!r.err&&(((r.fields&FPE_ID_USER)&&r.uid==UINT32_MAX)||((r.fields&FPE_ID_GROUP)&&r.gid==UINT32_MAX))))return ERROR_INVALID_DATA;
    if(bad_field)*bad_field=r.bad_field;if(r.err)return from_errno(r.err);
    out->fields=r.fields;out->uid=r.uid;out->gid=r.gid;
    uint32_t len=to_wide((uint8_t*)r.owner,(uint32_t)strlen(r.owner),out->owner,0);out->owner[len]=0;
    len=to_wide((uint8_t*)r.group,(uint32_t)strlen(r.group),out->group,0);out->group[len]=0;return 0;
}
static fpe_err metadata_ownership(fpe_wsl_metadata *session,const wchar_t *path,int as_root,const fpe_permissions *expected,
    const fpe_identity *identity,fpe_permissions *out,int *changed)
{
    if(changed)*changed=0;
    if(!path||!expected||!identity||!out||!identity->fields||(identity->fields&~3u)||
       ((identity->fields&FPE_ID_USER)&&identity->uid==UINT32_MAX)||
       ((identity->fields&FPE_ID_GROUP)&&identity->gid==UINT32_MAX))return ERROR_INVALID_PARAMETER;
    memset(out,0,sizeof *out);
    wslp_ownership_request q={.fields=identity->fields,.uid=identity->fields&FPE_ID_USER?identity->uid:0,
        .gid=identity->fields&FPE_ID_GROUP?identity->gid:0,.expected=permission_expected(expected)};
    wslp_permission_result r={0};
    fpe_err err=permission_wire(session,path,as_root,WSLP_OWNERSHIP,&q,sizeof q,&q.path_bytes,WSLP_OWNERSHIP_RESULT,&r,sizeof r);
    if(!err)err=permission_unpack(&r,out,changed);
    if(session&&err==ERROR_INVALID_DATA){session->failed=err;metadata_disconnect(session);}
    return err;
}

FPE_API fpe_err fpe_wsl_permissions(const wchar_t *path,int as_root,
    const fpe_permissions *expected,uint32_t mode,fpe_permissions *out,int *changed)
{
    return metadata_permissions(NULL,path,as_root,expected,mode,out,changed);
}
FPE_API fpe_err fpe_wsl_metadata_permissions(fpe_wsl_metadata *session,const wchar_t *path,
    const fpe_permissions *expected,uint32_t mode,fpe_permissions *out,int *changed)
{
    if(!session)return ERROR_INVALID_PARAMETER;
    return metadata_permissions(session,path,0,expected,mode,out,changed);
}
FPE_API fpe_err fpe_wsl_ownership(const wchar_t *path,int as_root,const fpe_permissions *expected,
    const fpe_identity *identity,fpe_permissions *out,int *changed)
{
    return metadata_ownership(NULL,path,as_root,expected,identity,out,changed);
}
FPE_API fpe_err fpe_wsl_metadata_ownership(fpe_wsl_metadata *session,const wchar_t *path,
    const fpe_permissions *expected,const fpe_identity *identity,fpe_permissions *out,int *changed)
{
    if(!session)return ERROR_INVALID_PARAMETER;
    return metadata_ownership(session,path,0,expected,identity,out,changed);
}

/* Isolated read-only scan, separate from browsing/watch and mutation helpers.
 * Poll small fixed frames so cancellation also works during helper startup. */
fpe_err fpe_wsl_size(const wchar_t *path,int as_root,fpe_size_cb cb,void *user,fpe_size_result *out)
{
    host h={0};char *bytes=NULL;HANDLE to=NULL,from=NULL,proc=NULL;
    fpe_err err=0;int greeted=0;uint64_t received=GetTickCount64(),notified=0;
    memset(out,0,sizeof *out);
    if(cb&&!cb(user,out)){err=ERROR_CANCELLED;goto done;}
    if(!fpe_wsl_native(path)||!have_helper()||!split(path,h.name,&bytes)){err=ERROR_NOT_SUPPORTED;goto done;}
    size_t n=strlen(bytes);if(!n||n>16384){err=ERROR_FILENAME_EXCED_RANGE;goto done;}
    if(!launch(&h,as_root,&to,&from,&proc,L" --sizes")){err=ERROR_NOT_SUPPORTED;goto done;}
    for(;;){
        uint64_t now=GetTickCount64();
        if(now-notified>=50){notified=now;if(cb&&!cb(user,out)){err=ERROR_CANCELLED;break;}}
        DWORD available=0;if(!PeekNamedPipe(from,NULL,0,NULL,&available,NULL)){err=ERROR_BROKEN_PIPE;break;}
        if(available>=sizeof(wslp_head)){
            wslp_head head;DWORD got=0;
            if(!PeekNamedPipe(from,&head,sizeof head,&got,NULL,NULL)||got!=sizeof head){err=ERROR_BROKEN_PIPE;break;}
            uint32_t expected=greeted?sizeof(wslp_size):sizeof(wslp_hello);
            if(head.type!=(greeted?WSLP_SIZE_RESULT:WSLP_HELLO)||head.size!=expected||head.id!=(greeted?1u:0u)){err=ERROR_INVALID_DATA;break;}
            if(available>=sizeof head+expected){
                read_all(from,&head,sizeof head);received=now;
                if(!greeted){
                    wslp_hello hi;if(!read_all(from,&hi,sizeof hi)||hi.magic!=WSLP_MAGIC||hi.version!=WSLP_VERSION){err=ERROR_INVALID_DATA;break;}
                    wslp_head q={WSLP_SIZE,(uint32_t)n,1};DWORD wrote=0;
                    if(!WriteFile(to,&q,sizeof q,&wrote,NULL)||wrote!=sizeof q||!WriteFile(to,bytes,(DWORD)n,&wrote,NULL)||wrote!=n){err=ERROR_BROKEN_PIPE;break;}
                    greeted=1;
                }else{
                    wslp_size r;if(!read_all(from,&r,sizeof r)||r.state>FPE_SIZE_CANCELLED||
                       r.bytes<out->bytes||r.files<out->files||r.folders<out->folders||r.skipped<out->skipped||r.errors<out->errors||
                       (!!r.errors!=!!r.first_error)||(r.state==FPE_SIZE_COMPLETE&&r.errors)){err=ERROR_INVALID_DATA;break;}
                    out->bytes=r.bytes;out->files=r.files;out->folders=r.folders;out->skipped=r.skipped;out->errors=r.errors;
                    out->first_error=from_errno(r.first_error);out->state=r.state;
                    if(r.state!=FPE_SIZE_RUNNING){err=r.state==FPE_SIZE_CANCELLED?ERROR_CANCELLED:out->first_error;break;}
                }
                continue;
            }
        }
        if(now-received>=30000){err=ERROR_TIMEOUT;break;}
        /* Check the pipe again after process exit: its last frame may still
         * be buffered, but a truncated frame must never be treated as complete. */
        if(WaitForSingleObject(proc,20)==WAIT_OBJECT_0){
            DWORD left=0;if(!PeekNamedPipe(from,NULL,0,NULL,&left,NULL)||left<=available){err=ERROR_BROKEN_PIPE;break;}
        }
    }
done:
    if(err==ERROR_CANCELLED){out->state=FPE_SIZE_CANCELLED;}
    else if(err&&out->state==FPE_SIZE_RUNNING){out->state=FPE_SIZE_PARTIAL;out->errors++;if(!out->first_error)out->first_error=err;}
    if(to){if(err==ERROR_CANCELLED){wslp_head q={WSLP_SIZE_CANCEL,0,1};DWORD wrote;WriteFile(to,&q,sizeof q,&wrote,NULL);}CloseHandle(to);}
    if(from)CloseHandle(from);
    if(proc){if(WaitForSingleObject(proc,250)==WAIT_TIMEOUT)TerminateProcess(proc,1);CloseHandle(proc);}
    fpe_free(bytes);if(cb)cb(user,out);return err;
}

/* Isolated native search: fixed bounded frames, no per-entry 9P requests. */
fpe_err fpe_wsl_search(const wchar_t *path,fpe_compare_cb pulse,fpe_search_entry_cb hit,void *user,uint32_t *skipped,uint32_t *errors)
{
    host h={0};char *root=NULL;HANDLE to=NULL,from=NULL,proc=NULL;fpe_err err=0;
    uint8_t buffer[sizeof(wslp_search_entry)+16384];wchar_t relative[16385];
    uint64_t received=GetTickCount64();int greeted=0;uint32_t count=0;*skipped=*errors=0;
    if(pulse&&!pulse(user))return ERROR_CANCELLED;
    if(!fpe_wsl_native(path)||!split(path,h.name,&root))return ERROR_NOT_SUPPORTED;
    size_t bytes=strlen(root);if(!bytes||bytes>16384){err=ERROR_FILENAME_EXCED_RANGE;goto done;}
    if(!launch(&h,fpe_wsl_root(),&to,&from,&proc,L" --search")){err=ERROR_NOT_SUPPORTED;goto done;}
    for(;;){
        if(pulse&&!pulse(user)){err=ERROR_CANCELLED;break;}
        DWORD available=0;if(!PeekNamedPipe(from,NULL,0,NULL,&available,NULL)){err=ERROR_BROKEN_PIPE;break;}
        if(available>=sizeof(wslp_head)){
            wslp_head head;DWORD got=0;if(!PeekNamedPipe(from,&head,sizeof head,&got,NULL,NULL)||got!=sizeof head){err=ERROR_BROKEN_PIPE;break;}
            int valid=!greeted?head.type==WSLP_HELLO&&head.id==0&&head.size==sizeof(wslp_hello):
                head.id==1&&((head.type==WSLP_SEARCH_END&&head.size==sizeof(wslp_search_end))||
                (head.type==WSLP_SEARCH_ENTRY&&head.size>sizeof(wslp_search_entry)&&head.size<=sizeof buffer));
            if(!valid){err=ERROR_INVALID_DATA;break;}
            if(available>=sizeof head+head.size){
                if(!read_all(from,&head,sizeof head)||!read_all(from,buffer,head.size)){err=ERROR_BROKEN_PIPE;break;}received=GetTickCount64();
                if(!greeted){wslp_hello hi;memcpy(&hi,buffer,sizeof hi);if(hi.magic!=WSLP_MAGIC||hi.version!=WSLP_VERSION){err=ERROR_INVALID_DATA;break;}
                    wslp_head q={WSLP_SEARCH,(uint32_t)bytes,1};DWORD wrote;
                    if(!WriteFile(to,&q,sizeof q,&wrote,NULL)||wrote!=sizeof q||!WriteFile(to,root,(DWORD)bytes,&wrote,NULL)||wrote!=bytes){err=ERROR_BROKEN_PIPE;break;}greeted=1;
                }else if(head.type==WSLP_SEARCH_END){
                    wslp_search_end end;memcpy(&end,buffer,sizeof end);
                    if(end.scanned<count||end.scanned>100000||end.skipped>end.scanned||end.errors>100128||!!end.error!=!!end.errors){err=ERROR_INVALID_DATA;break;}
                    *skipped=end.skipped;*errors=end.errors;err=end.error==75?ERROR_BUFFER_OVERFLOW:from_errno(end.error);break;
                }else{
                    wslp_search_entry e;memcpy(&e,buffer,sizeof e);uint8_t *raw=buffer+sizeof e;
                    if(++count>100000||e.path_bytes!=head.size-sizeof e||memchr(raw,0,e.path_bytes)||raw[0]=='/'||raw[e.path_bytes-1]=='/'||e.attrs&~0x490u){err=ERROR_INVALID_DATA;break;}
                    uint32_t start=0,n=0;
                    for(uint32_t i=0;i<=e.path_bytes;i++)if(i==e.path_bytes||raw[i]=='/'){
                        uint32_t length=i-start;if(!length||(length==1&&raw[start]=='.')||(length==2&&raw[start]=='.'&&raw[start+1]=='.')){err=ERROR_INVALID_DATA;break;}
                        if(start)relative[n++]=L'\\';n+=to_wide(raw+start,length,relative+n,1);start=i+1;
                    }
                    if(err)break;relative[n]=0;fpe_entry entry={.size=e.size,.mtime=e.mtime,.attrs=e.attrs};
                    if(hit&&!hit(user,relative,&entry)){err=ERROR_CANCELLED;break;}
                }
                continue;
            }
        }
        if(GetTickCount64()-received>=30000){err=ERROR_TIMEOUT;break;}
        if(WaitForSingleObject(proc,10)==WAIT_OBJECT_0){DWORD left=0;if(!PeekNamedPipe(from,NULL,0,NULL,&left,NULL)||left<=available){err=ERROR_BROKEN_PIPE;break;}}
    }
done:
    if(to)CloseHandle(to);if(from)CloseHandle(from);
    if(proc){if(WaitForSingleObject(proc,250)==WAIT_TIMEOUT)TerminateProcess(proc,1);CloseHandle(proc);}
    fpe_free(root);return err;
}

static int rename_write_all(HANDLE pipe,const void *data,DWORD size)
{
    const BYTE *p=data;while(size){DWORD wrote=0,n=size>32768?32768:size;
        if(!WriteFile(pipe,p,n,&wrote,NULL)||!wrote)return 0;p+=wrote;size-=wrote;}return 1;
}

/* The optional index has its own session and lock. Only search workers call
 * it; browsing/listing never acquires this lock or starts this process. One
 * selected watched root is retained, and replaced on demand for another root.
 * Linux owns persistent selections and scopes them by the actual helper UID. */
static SRWLOCK index_lock=SRWLOCK_INIT;
static struct {
    host h;HANDLE to,from,proc;int as_root;LONG epoch;uint32_t uid,sequence;
    char selected[16385];fpe_index_status status;
} index_session;
static volatile LONG index_active;
static volatile LONG index_epoch;
static int index_write(const void *data,DWORD n){return rename_write_all(index_session.to,data,n);}
static void index_close(void)
{
    if(index_session.to)CloseHandle(index_session.to);
    if(index_session.from)CloseHandle(index_session.from);
    if(index_session.proc){if(WaitForSingleObject(index_session.proc,250)==WAIT_TIMEOUT)TerminateProcess(index_session.proc,1);CloseHandle(index_session.proc);}
    memset(&index_session,0,sizeof index_session);InterlockedExchange(&index_active,0);
}
FPE_API void fpe_search_index_shutdown(void)
{AcquireSRWLockExclusive(&index_lock);index_close();ReleaseSRWLockExclusive(&index_lock);}
static DWORD WINAPI index_shutdown_worker(void *value)
{LONG epoch=(LONG)(INT_PTR)value;AcquireSRWLockExclusive(&index_lock);if(index_session.proc&&index_session.epoch<epoch)index_close();ReleaseSRWLockExclusive(&index_lock);return 0;}
static void index_shutdown_async(void)
{LONG epoch=InterlockedIncrement(&index_epoch);if(InterlockedCompareExchange(&index_active,0,0)){HANDLE t=CreateThread(NULL,0,index_shutdown_worker,(void*)(INT_PTR)epoch,0,NULL);if(t)CloseHandle(t);}}
static int index_take_lock(fpe_compare_cb pulse,void *user)
{while(!TryAcquireSRWLockExclusive(&index_lock)){if(pulse&&!pulse(user))return 0;Sleep(10);}return 1;}
static fpe_err index_read(uint32_t id,wslp_head *head,uint8_t *data,uint32_t cap,fpe_compare_cb pulse,void *user)
{
    uint64_t start=GetTickCount64();
    for(;;){
        if(index_session.epoch!=InterlockedCompareExchange(&index_epoch,0,0))return ERROR_CANCELLED;
        if(pulse&&!pulse(user))return ERROR_CANCELLED;
        DWORD available=0,got=0;if(!PeekNamedPipe(index_session.from,NULL,0,NULL,&available,NULL))return ERROR_BROKEN_PIPE;
        if(available>=sizeof *head){
            if(!PeekNamedPipe(index_session.from,head,sizeof *head,&got,NULL,NULL)||got!=sizeof *head)return ERROR_BROKEN_PIPE;
            if(head->id!=id||head->size>cap)return ERROR_INVALID_DATA;
            if(available>=sizeof *head+head->size){if(!read_all(index_session.from,head,sizeof *head)||!read_all(index_session.from,data,head->size))return ERROR_BROKEN_PIPE;return 0;}
        }
        if(GetTickCount64()-start>=30000)return ERROR_TIMEOUT;
        if(WaitForSingleObject(index_session.proc,10)==WAIT_OBJECT_0&&!available)return ERROR_BROKEN_PIPE;
    }
}
static fpe_err index_status_frame(const wslp_head *head,const uint8_t *data)
{
    if(head->type!=WSLP_INDEX_STATUS||head->size<sizeof(wslp_index_status))return ERROR_INVALID_DATA;
    wslp_index_status s;memcpy(&s,data,sizeof s);
    if(s.state>FPE_INDEX_ERROR||s.uid!=index_session.uid||s.enabled>1||s.pad||s.root_bytes>16384||
       head->size!=sizeof s+s.root_bytes||s.entries>1000000||s.scanned>1000000||s.errors>1000128||s.skipped>1000000)return ERROR_INVALID_DATA;
    const uint8_t *raw=data+sizeof s;
    if(s.root_bytes&&(raw[0]!='/'||memchr(raw,0,s.root_bytes)))return ERROR_INVALID_DATA;
    fpe_index_status *out=&index_session.status;memset(out,0,sizeof *out);
    out->state=s.state;out->error=from_errno(s.error);out->uid=s.uid;out->enabled=s.enabled;
    out->entries=s.entries;out->scanned=s.scanned;out->errors=s.errors;out->skipped=s.skipped;out->updated=s.updated;
    memcpy(index_session.selected,raw,s.root_bytes);index_session.selected[s.root_bytes]=0;
    if(s.root_bytes){
        int n=_snwprintf(out->root,32768,L"\\\\wsl.localhost\\%ls",index_session.h.name);if(n<0)return ERROR_INVALID_DATA;
        uint32_t start=1;
        for(uint32_t i=1;i<=s.root_bytes;i++)if(i==s.root_bytes||raw[i]=='/'){
            uint32_t length=i-start;if(!length&&s.root_bytes!=1)return ERROR_INVALID_DATA;
            if((length==1&&raw[start]=='.')||(length==2&&raw[start]=='.'&&raw[start+1]=='.'))return ERROR_INVALID_DATA;
            if(length){out->root[n++]=L'\\';n+=to_wide(raw+start,length,out->root+n,1);}start=i+1;
        }
        out->root[n]=0;
    }return 0;
}
static fpe_err index_open(const wchar_t *folder,uint32_t action,fpe_compare_cb pulse,void *user,int *opened)
{
    host h={0};char *root=NULL;*opened=0;LONG epoch=InterlockedCompareExchange(&index_epoch,0,0);
    if(!fpe_wsl_native(folder)||!split(folder,h.name,&root)){index_close();return ERROR_NOT_SUPPORTED;}
    size_t n=strlen(root);while(n>1&&root[n-1]=='/')root[--n]=0;
    if(n>16384){fpe_free(root);return ERROR_FILENAME_EXCED_RANGE;}
    int as_root=fpe_wsl_root();size_t selected=strlen(index_session.selected);
    if(index_session.proc&&index_session.epoch==epoch&&as_root==index_session.as_root&&!wcscmp(h.name,index_session.h.name)&&selected&&
       !strncmp(root,index_session.selected,selected)&&(selected==1||!root[selected]||root[selected]=='/')&&
       action!=FPE_INDEX_REBUILD&&action!=FPE_INDEX_REMOVE&&
       (action!=FPE_INDEX_ENABLE||!strcmp(root,index_session.selected))&&WaitForSingleObject(index_session.proc,0)==WAIT_TIMEOUT){fpe_free(root);return 0;}
    index_close();index_session.h=h;index_session.as_root=as_root;index_session.epoch=epoch;
    if(!launch(&index_session.h,as_root,&index_session.to,&index_session.from,&index_session.proc,L" --index")){fpe_free(root);index_close();return ERROR_NOT_SUPPORTED;}
    InterlockedExchange(&index_active,1);
    uint8_t data[sizeof(wslp_index_status)+16384];wslp_head head;fpe_err err=index_read(0,&head,data,sizeof data,pulse,user);
    if(!err){wslp_hello hi;if(head.type==WSLP_HELLO)err=ERROR_NOT_SUPPORTED;
        else if(head.type!=WSLP_INDEX_HELLO||head.size!=sizeof hi)err=ERROR_INVALID_DATA;
        else{memcpy(&hi,data,sizeof hi);if(hi.magic!=WSLP_MAGIC||hi.version!=WSLP_VERSION)err=ERROR_INVALID_DATA;else index_session.uid=hi.uid;}}
    if(!err){wslp_index_open open={action,(uint32_t)n};wslp_head q={WSLP_INDEX_OPEN,(uint32_t)(sizeof open+n),1};
        if(!index_write(&q,sizeof q)||!index_write(&open,sizeof open)||!index_write(root,(DWORD)n))err=ERROR_BROKEN_PIPE;}
    if(!err)err=index_read(1,&head,data,sizeof data,pulse,user);
    if(!err)err=index_status_frame(&head,data);
    fpe_free(root);if(err){index_close();return err;}index_session.sequence=1;*opened=1;return 0;
}
typedef struct index_control_pulse {fpe_index_cb cb;void *user;const fpe_index_status *view;} index_control_pulse;
static int index_control_keep(void *user)
{index_control_pulse *p=user;return !p->cb||p->cb(p->user,p->view);}
FPE_API fpe_err fpe_search_index_control(const wchar_t *folder,uint32_t action,fpe_index_cb cb,void *user,fpe_index_status *out)
{
    if(!folder||!out||action>FPE_INDEX_REMOVE)return ERROR_INVALID_PARAMETER;memset(out,0,sizeof *out);
    index_control_pulse p={cb,user,out};if(!index_take_lock(index_control_keep,&p))return ERROR_CANCELLED;p.view=&index_session.status;
    int opened=0;fpe_err err=index_open(folder,action,index_control_keep,&p,&opened);
    if(!err&&!opened){uint32_t id=++index_session.sequence;wslp_head q={WSLP_INDEX_CONTROL,4,id},head;uint8_t data[sizeof(wslp_index_status)+16384];
        if(!index_write(&q,sizeof q)||!index_write(&action,4))err=ERROR_BROKEN_PIPE;
        if(!err)err=index_read(id,&head,data,sizeof data,index_control_keep,&p);
        if(!err)err=index_status_frame(&head,data);
    }
    if(!err){*out=index_session.status;err=out->error;
        if(out->state==FPE_INDEX_NONE||out->state==FPE_INDEX_PAUSED||(out->state==FPE_INDEX_ERROR&&!out->enabled)||action==FPE_INDEX_REMOVE)index_close();}
    else{index_close();out->state=FPE_INDEX_ERROR;out->error=err;}
    ReleaseSRWLockExclusive(&index_lock);return err;
}
typedef struct index_query_pulse {fpe_compare_cb pulse;void *user;uint32_t id;int aborted;uint64_t abort_at;} index_query_pulse;
static int index_query_keep(void *user)
{
    index_query_pulse *p=user;
    if(!p->aborted&&p->pulse&&!p->pulse(p->user)){
        wslp_head cancel={WSLP_INDEX_ABORT,0,p->id};if(!index_write(&cancel,sizeof cancel))return 0;
        p->aborted=1;p->abort_at=GetTickCount64();
    }return !p->aborted||GetTickCount64()-p->abort_at<=2000;
}
fpe_err fpe_wsl_index_search(const wchar_t *folder,const char *glob,fpe_compare_cb pulse,fpe_search_entry_cb hit,void *user,uint32_t *skipped,uint32_t *errors,fpe_index_status *out)
{
    *skipped=*errors=0;memset(out,0,sizeof *out);if(!index_take_lock(pulse,user))return ERROR_CANCELLED;
    int opened=0;fpe_err err=index_open(folder,FPE_INDEX_STATUS,pulse,user,&opened);uint32_t id=0,count=0;int saw_status=0;
    index_query_pulse keep={.pulse=pulse,.user=user};
    char *root=NULL;host h={0};uint8_t data[sizeof(wslp_index_status)+16384];wslp_head head;
    if(err)goto done;
    *out=index_session.status;
    if(!out->enabled||out->state==FPE_INDEX_PAUSED||out->state==FPE_INDEX_ERROR){err=ERROR_RETRY;goto done;}
    if(!split(folder,h.name,&root)){err=ERROR_INVALID_PARAMETER;goto done;}
    wslp_index_query q={(uint32_t)strlen(root),(uint32_t)strlen(glob)};id=++index_session.sequence;
    keep.id=id;
    wslp_head request={WSLP_INDEX_QUERY,(uint32_t)(sizeof q+q.path_bytes+q.glob_bytes),id};
    if(!index_write(&request,sizeof request)||!index_write(&q,sizeof q)||!index_write(root,q.path_bytes)||!index_write(glob,q.glob_bytes)){err=ERROR_BROKEN_PIPE;goto done;}
    for(;;){
        if(!index_query_keep(&keep)){err=ERROR_CANCELLED;break;}
        err=index_read(id,&head,data,sizeof data,index_query_keep,&keep);if(err)break;
        if(head.type==WSLP_INDEX_STATUS){err=index_status_frame(&head,data);if(err)break;*out=index_session.status;saw_status=1;}
        else if(head.type==WSLP_SEARCH_END){
            wslp_search_end end;if(head.size!=sizeof end||!saw_status){err=ERROR_INVALID_DATA;break;}memcpy(&end,data,sizeof end);
            if(end.scanned<count||end.scanned>100000||end.skipped>1000000||!!end.error!=!!end.errors){err=ERROR_INVALID_DATA;break;}
            *skipped=end.skipped;*errors=end.errors;
            err=out->state!=FPE_INDEX_CURRENT||end.error==11?ERROR_RETRY:end.error==75?ERROR_BUFFER_OVERFLOW:from_errno(end.error);
            if(keep.aborted&&err!=ERROR_RETRY)err=ERROR_CANCELLED;break;
        }else if(head.type==WSLP_SEARCH_ENTRY){
            if(saw_status||head.size<=sizeof(wslp_search_entry)){err=ERROR_INVALID_DATA;break;}
            wslp_search_entry e;memcpy(&e,data,sizeof e);uint8_t *raw=data+sizeof e;
            if(++count>100000||e.path_bytes!=head.size-sizeof e||memchr(raw,0,e.path_bytes)||raw[0]=='/'||raw[e.path_bytes-1]=='/'||e.attrs&~0x490u){err=ERROR_INVALID_DATA;break;}
            if(!keep.aborted){wchar_t relative[16385];uint32_t start=0,n=0;
                for(uint32_t i=0;i<=e.path_bytes;i++)if(i==e.path_bytes||raw[i]=='/'){
                    uint32_t length=i-start;if(!length||(length==1&&raw[start]=='.')||(length==2&&raw[start]=='.'&&raw[start+1]=='.')){err=ERROR_INVALID_DATA;break;}
                    if(start)relative[n++]=L'\\';n+=to_wide(raw+start,length,relative+n,1);start=i+1;
                }
                if(err)break;relative[n]=0;fpe_entry entry={.size=e.size,.mtime=e.mtime,.attrs=e.attrs};
                if(hit&&!hit(user,relative,&entry)){wslp_head cancel={WSLP_INDEX_ABORT,0,id};if(!index_write(&cancel,sizeof cancel)){err=ERROR_BROKEN_PIPE;break;}keep.aborted=1;keep.abort_at=GetTickCount64();}
            }
        }else{err=ERROR_INVALID_DATA;break;}
    }
done:
    fpe_free(root);
    if(err&&err!=ERROR_RETRY&&err!=ERROR_BUFFER_OVERFLOW&&err!=ERROR_CANCELLED){out->state=FPE_INDEX_ERROR;out->error=err;}
    if(err&&err!=ERROR_RETRY&&err!=ERROR_BUFFER_OVERFLOW&&!(err==ERROR_CANCELLED&&saw_status))index_close();
    else if(!index_session.status.enabled||index_session.status.state==FPE_INDEX_PAUSED)index_close();
    ReleaseSRWLockExclusive(&index_lock);return err;
}

/* A batch uses one isolated helper. Keep its final per-item reply even after
 * Cancel; abandoning an in-flight mutation must never look like a safe retry. */
fpe_err fpe_wsl_rename_batch(const wchar_t *folder,int as_root,fpe_rename_item *items,
    uint32_t count,int apply,fpe_rename_cb cb,void *user)
{
    host h={0};char *root=NULL;HANDLE to=NULL,from=NULL,proc=NULL;
    uint8_t *request=NULL;size_t request_bytes=0;fpe_err err=0;int sent=0,cancelled=0,finished=0,greeted=0;
    uint64_t received=GetTickCount64(),notified=0;uint32_t completed=0,phase=0;
    if(cb&&!cb(user,0,0)){cancelled=1;goto done;}
    if(!fpe_wsl_native(folder)||!have_helper()||!split(folder,h.name,&root)){err=ERROR_NOT_SUPPORTED;goto done;}
    size_t rn=strlen(root);if(!rn||rn>16384){err=ERROR_FILENAME_EXCED_RANGE;goto done;}
    request=fpe_alloc(WSLP_MAX);if(!request){err=ERROR_NOT_ENOUGH_MEMORY;goto done;}
    wslp_rename_request q={(uint32_t)apply,count,(uint32_t)rn,0};memcpy(request,&q,sizeof q);request_bytes=sizeof q;
    memcpy(request+request_bytes,root,rn);request_bytes+=rn;
    for(uint32_t i=0;i<count;i++){
        char *names[2]={0};const wchar_t *in[]={items[i].from,items[i].to};
        for(int j=0;j<2;j++){
            size_t fn=wcslen(folder),nn=wcslen(in[j]);wchar_t *full=fpe_alloc((fn+nn+2)*sizeof(wchar_t));
            if(!full){err=ERROR_NOT_ENOUGH_MEMORY;break;}
            memcpy(full,folder,fn*sizeof(wchar_t));full[fn]=L'\\';memcpy(full+fn+1,in[j],(nn+1)*sizeof(wchar_t));
            wchar_t distro[64];char *native=NULL;
            if(!split(full,distro,&native)){err=ERROR_INVALID_NAME;fpe_free(full);break;}fpe_free(full);
            char *tail=strrchr(native,'/');size_t n=tail?strlen(tail+1):0;
            if(!n||n>255){fpe_free(native);err=ERROR_INVALID_NAME;break;}
            names[j]=fpe_alloc(n+1);if(names[j])memcpy(names[j],tail+1,n+1);else err=ERROR_NOT_ENOUGH_MEMORY;
            fpe_free(native);if(err)break;
        }
        if(!err){
            wslp_rename_item record={0};memcpy(&record.identity,&items[i].identity,sizeof record.identity);
            record.from_bytes=(uint32_t)strlen(names[0]);record.to_bytes=(uint32_t)strlen(names[1]);
            memcpy(request+request_bytes,&record,sizeof record);request_bytes+=sizeof record;
            memcpy(request+request_bytes,names[0],record.from_bytes);request_bytes+=record.from_bytes;
            memcpy(request+request_bytes,names[1],record.to_bytes);request_bytes+=record.to_bytes;
        }
        fpe_free(names[0]);fpe_free(names[1]);if(err)goto done;
    }
    if(!launch(&h,as_root,&to,&from,&proc,L" --rename")){err=ERROR_NOT_SUPPORTED;goto done;}
    for(;;){
        uint64_t now=GetTickCount64();
        if(now-notified>=50){
            notified=now;
            if(!cancelled&&cb&&!cb(user,completed,phase)){
                cancelled=1;if(!sent)break;
                wslp_head stop={WSLP_RENAME_CANCEL,0,1};DWORD wrote;
                if(!WriteFile(to,&stop,sizeof stop,&wrote,NULL)||wrote!=sizeof stop){err=ERROR_BROKEN_PIPE;break;}
            }
        }
        DWORD available=0;if(!PeekNamedPipe(from,NULL,0,NULL,&available,NULL)){err=ERROR_BROKEN_PIPE;break;}
        if(available>=sizeof(wslp_head)){
            wslp_head head;DWORD got=0;if(!PeekNamedPipe(from,&head,sizeof head,&got,NULL,NULL)||got!=sizeof head){err=ERROR_BROKEN_PIPE;break;}
            int valid=!greeted?head.type==WSLP_HELLO&&head.size==sizeof(wslp_hello)&&head.id==0:
                head.id==1&&((head.type==WSLP_RENAME_PROGRESS&&head.size==sizeof(wslp_rename_progress))||
                    (head.type==WSLP_RENAME_RESULT&&head.size==sizeof(wslp_rename_end)+count*sizeof(wslp_rename_result)));
            if(!valid){err=ERROR_INVALID_DATA;break;}
            if(available>=sizeof head+head.size){
                if(!read_all(from,&head,sizeof head)){err=ERROR_BROKEN_PIPE;break;}received=now;
                if(!greeted){
                    wslp_hello hi;if(!read_all(from,&hi,sizeof hi)||hi.magic!=WSLP_MAGIC||hi.version!=WSLP_VERSION){err=ERROR_INVALID_DATA;break;}
                    greeted=1;wslp_head start={WSLP_RENAME_BATCH,(uint32_t)request_bytes,1};
                    /* A failed pipe write can race a helper which already read
                     * the request. Treat any submission attempt as ambiguous
                     * until its complete per-item reply has been validated. */
                    sent=1;FPE_PROF_COUNT("rename.request_attempted");
                    if(!rename_write_all(to,&start,sizeof start)||!rename_write_all(to,request,(DWORD)request_bytes)){err=ERROR_BROKEN_PIPE;break;}
                }else if(head.type==WSLP_RENAME_PROGRESS){
                    wslp_rename_progress p;if(!read_all(from,&p,sizeof p)||p.completed>count||p.apply>(uint32_t)apply){err=ERROR_INVALID_DATA;break;}
                    completed=p.completed;phase=p.apply;
                    if(!cancelled&&cb&&!cb(user,completed,phase)){
                        cancelled=1;wslp_head stop={WSLP_RENAME_CANCEL,0,1};DWORD wrote;
                        if(!WriteFile(to,&stop,sizeof stop,&wrote,NULL)||wrote!=sizeof stop){err=ERROR_BROKEN_PIPE;break;}
                    }
                }else{
                    wslp_rename_end end;wslp_rename_result results[FPE_RENAME_MAX];
                    if(!read_all(from,&end,sizeof end)||end.count!=count||end.stopped>1||!read_all(from,results,count*sizeof *results)){err=ERROR_INVALID_DATA;break;}
                    int valid_results=1;
                    for(uint32_t i=0;i<count;i++)if(results[i].state<FPE_RENAME_READY||results[i].state>FPE_RENAME_UNCERTAIN||
                        (!apply&&(results[i].state==FPE_RENAME_RENAMED||results[i].state==FPE_RENAME_UNCERTAIN))||
                        ((results[i].state==FPE_RENAME_ERROR||results[i].state==FPE_RENAME_UNCERTAIN)!=!!results[i].error)){valid_results=0;break;}
                    if(!valid_results){err=ERROR_INVALID_DATA;break;}
                    for(uint32_t i=0;i<count;i++){
                        memcpy(&items[i].identity,&results[i].identity,sizeof items[i].identity);
                        items[i].state=results[i].state;items[i].error=from_errno(results[i].error);if(items[i].error&&!err)err=items[i].error;
                    }
                    if(end.stopped&&!err)err=ERROR_CANCELLED;finished=1;FPE_PROF_COUNT("rename.reply_received");break;
                }
                continue;
            }
        }
        if(now-received>=30000){err=ERROR_TIMEOUT;break;}
        if(WaitForSingleObject(proc,10)==WAIT_OBJECT_0){DWORD left=0;if(!PeekNamedPipe(from,NULL,0,NULL,&left,NULL)||left<=available){err=ERROR_BROKEN_PIPE;break;}}
    }
done:
    if(!finished&&sent&&apply)FPE_PROF_COUNT("rename.reply_lost");
    if(!finished)for(uint32_t i=0;i<count;i++){
        items[i].state=sent&&apply?FPE_RENAME_UNCERTAIN:cancelled?FPE_RENAME_STOPPED:FPE_RENAME_ERROR;
        items[i].error=items[i].state==FPE_RENAME_STOPPED?0:err?err:ERROR_GEN_FAILURE;
    }
    if(to)CloseHandle(to);if(from)CloseHandle(from);
    if(proc){if(WaitForSingleObject(proc,250)==WAIT_TIMEOUT)TerminateProcess(proc,1);CloseHandle(proc);}
    fpe_free(root);fpe_free(request);return err?err:cancelled&&!finished?ERROR_CANCELLED:0;
}

/* Read-only isolated hash transport. Abandoning it cannot mutate a source. */
fpe_err fpe_wsl_hash(const wchar_t *path,int as_root,fpe_compare_cb cb,void *user,fpe_hash *out)
{
    host h={0};char *bytes=NULL;HANDLE to=NULL,from=NULL,proc=NULL;fpe_err err=0;
    int greeted=0;uint64_t received=GetTickCount64(),notified=0;memset(out,0,sizeof *out);
    if(cb&&!cb(user)){err=ERROR_CANCELLED;goto done;}
    if(!fpe_wsl_native(path)||!have_helper()||!split(path,h.name,&bytes)){err=ERROR_NOT_SUPPORTED;goto done;}
    size_t n=strlen(bytes);if(!n||n>16384){err=ERROR_FILENAME_EXCED_RANGE;goto done;}
    if(!launch(&h,as_root,&to,&from,&proc,L" --hash")){err=ERROR_NOT_SUPPORTED;goto done;}
    for(;;){
        uint64_t now=GetTickCount64();if(now-notified>=50){notified=now;if(cb&&!cb(user)){err=ERROR_CANCELLED;break;}}
        DWORD available=0;if(!PeekNamedPipe(from,NULL,0,NULL,&available,NULL)){err=ERROR_BROKEN_PIPE;break;}
        if(available>=sizeof(wslp_head)){
            wslp_head head;DWORD got=0;if(!PeekNamedPipe(from,&head,sizeof head,&got,NULL,NULL)||got!=sizeof head){err=ERROR_BROKEN_PIPE;break;}
            uint32_t expected=greeted?sizeof(wslp_hash):sizeof(wslp_hello);
            if(head.type!=(greeted?WSLP_HASH_RESULT:WSLP_HELLO)||head.size!=expected||head.id!=(greeted?1u:0u)){err=ERROR_INVALID_DATA;break;}
            if(available>=sizeof head+expected){
                if(!read_all(from,&head,sizeof head)){err=ERROR_BROKEN_PIPE;break;}received=now;
                if(!greeted){
                    wslp_hello hi;if(!read_all(from,&hi,sizeof hi)||hi.magic!=WSLP_MAGIC||hi.version!=WSLP_VERSION){err=ERROR_INVALID_DATA;break;}
                    wslp_head q={WSLP_HASH,(uint32_t)n,1};greeted=1;
                    if(!rename_write_all(to,&q,sizeof q)||!rename_write_all(to,bytes,(DWORD)n)){err=ERROR_BROKEN_PIPE;break;}
                }else{
                    wslp_hash r;if(!read_all(from,&r,sizeof r)||r.done>1||(!r.done&&r.err)){err=ERROR_INVALID_DATA;break;}
                    if(r.done){err=from_errno(r.err);if(!err){out->size=r.size;out->mtime=r.mtime;memcpy(out->sha256,r.sha256,32);}break;}
                }
                continue;
            }
        }
        if(now-received>=30000){err=ERROR_TIMEOUT;break;}
        if(WaitForSingleObject(proc,20)==WAIT_OBJECT_0){DWORD left=0;if(!PeekNamedPipe(from,NULL,0,NULL,&left,NULL)||left<=available){err=ERROR_BROKEN_PIPE;break;}}
    }
done:
    if(to)CloseHandle(to);if(from)CloseHandle(from);
    if(proc){if(WaitForSingleObject(proc,250)==WAIT_TIMEOUT)TerminateProcess(proc,1);CloseHandle(proc);}
    fpe_free(bytes);if(err)memset(out,0,sizeof *out);return err;
}

/* Live native directory anchor. Closing stdin releases the descriptor. */
int fpe_wsl_root(void){AcquireSRWLockShared(&g_lock);int root=g_cfg.as_root;ReleaseSRWLockShared(&g_lock);return root;}
static fpe_err wsl_guard_barrier(fpe_path_guard *g,uint32_t type,fpe_compare_cb cb,void *user)
{
    if(cb&&!cb(user))return ERROR_CANCELLED;
    wslp_head request={type,0,1};
    if(!rename_write_all(g->to,&request,sizeof request))return ERROR_BROKEN_PIPE;
    uint64_t started=GetTickCount64();
    for(;;){
        DWORD available=0;if(!PeekNamedPipe(g->from,NULL,0,NULL,&available,NULL))return ERROR_BROKEN_PIPE;
        if(available>=sizeof(wslp_head)){
            wslp_head h;DWORD got=0;
            if(!PeekNamedPipe(g->from,&h,sizeof h,&got,NULL,NULL)||got!=sizeof h)return ERROR_BROKEN_PIPE;
            if(h.type!=WSLP_GUARD_ACK||h.size!=sizeof(int32_t)||h.id!=1)return ERROR_INVALID_DATA;
            if(available>=sizeof h+sizeof(int32_t)){
                int32_t error;if(!read_all(g->from,&h,sizeof h)||!read_all(g->from,&error,sizeof error))return ERROR_BROKEN_PIPE;
                if(error<0)return ERROR_INVALID_DATA;
                fpe_err err=from_errno(error);return err?err:cb&&!cb(user)?ERROR_CANCELLED:0;
            }
        }
        if(cb&&!cb(user))return ERROR_CANCELLED;
        if(GetTickCount64()-started>=30000)return ERROR_TIMEOUT;
        if(WaitForSingleObject(g->process,20)==WAIT_OBJECT_0)return ERROR_BROKEN_PIPE;
    }
}
fpe_err fpe_wsl_guard_sync(fpe_path_guard *g,fpe_compare_cb cb,void *user)
{return wsl_guard_barrier(g,WSLP_GUARD_SYNC,cb,user);}
fpe_err fpe_wsl_guard_ancestry(fpe_path_guard *g,fpe_compare_cb cb,void *user)
{return wsl_guard_barrier(g,WSLP_GUARD_ANCESTRY,cb,user);}
fpe_err fpe_wsl_guard(const wchar_t *path,int anchored,uint64_t device,int create,int as_root,fpe_compare_cb cb,void *user,fpe_path_guard *out)
{
    host h={0};char *bytes=NULL;HANDLE to=NULL,from=NULL,proc=NULL;fpe_err err=0;
    int greeted=0;uint64_t received=GetTickCount64(),notified=0;
    if(!fpe_wsl_native(path)||!have_helper()||!split(path,h.name,&bytes)){err=ERROR_NOT_SUPPORTED;goto done;}
    size_t n=strlen(bytes);if(!n||n>16384){err=ERROR_FILENAME_EXCED_RANGE;goto done;}
    if(!launch(&h,as_root,&to,&from,&proc,L" --guard")){err=ERROR_NOT_SUPPORTED;goto done;}
    for(;;){
        uint64_t now=GetTickCount64();if(now-notified>=50){notified=now;if(cb&&!cb(user)){err=ERROR_CANCELLED;break;}}
        DWORD available=0;if(!PeekNamedPipe(from,NULL,0,NULL,&available,NULL)){err=ERROR_BROKEN_PIPE;break;}
        if(available>=sizeof(wslp_head)){
            wslp_head head;DWORD got=0;if(!PeekNamedPipe(from,&head,sizeof head,&got,NULL,NULL)||got!=sizeof head){err=ERROR_BROKEN_PIPE;break;}
            uint32_t expected=greeted?sizeof(wslp_guard_result):sizeof(wslp_hello);
            if(head.type!=(greeted?WSLP_GUARD_RESULT:WSLP_HELLO)||head.size!=expected||head.id!=(greeted?1u:0u)){err=ERROR_INVALID_DATA;break;}
            if(available>=sizeof head+expected){
                if(!read_all(from,&head,sizeof head)){err=ERROR_BROKEN_PIPE;break;}received=now;
                if(!greeted){
                    wslp_hello hi;if(!read_all(from,&hi,sizeof hi)||hi.magic!=WSLP_MAGIC||hi.version!=WSLP_VERSION){err=ERROR_INVALID_DATA;break;}
                    wslp_guard_request q={device,!!anchored,!!create};wslp_head start={WSLP_GUARD,(uint32_t)(sizeof q+n),1};greeted=1;
                    if(!rename_write_all(to,&start,sizeof start)||!rename_write_all(to,&q,sizeof q)||!rename_write_all(to,bytes,(DWORD)n)){err=ERROR_BROKEN_PIPE;break;}
                }else{
                    wslp_guard_result v;if(!read_all(from,&v,sizeof v)||v.pad){err=ERROR_INVALID_DATA;break;}
                    err=from_errno(v.error);if(err)break;if(!v.pid||!v.fd||!v.device){err=ERROR_INVALID_DATA;break;}
                    wchar_t anchor[256];swprintf(anchor,256,L"\\\\wsl.localhost\\%ls\\proc\\%u\\fd\\%u",h.name,v.pid,v.fd);
                    out->path=fpe_wcsdup(anchor);if(!out->path){err=ERROR_NOT_ENOUGH_MEMORY;break;}
                    out->linux=1;out->device=v.device;out->to=to;out->from=from;out->process=proc;to=from=proc=NULL;break;
                }
                continue;
            }
        }
        if(now-received>=30000){err=ERROR_TIMEOUT;break;}
        if(WaitForSingleObject(proc,20)==WAIT_OBJECT_0){DWORD left=0;if(!PeekNamedPipe(from,NULL,0,NULL,&left,NULL)||left<=available){err=ERROR_BROKEN_PIPE;break;}}
    }
done:
    if(to)CloseHandle(to);if(from)CloseHandle(from);
    if(proc){if(WaitForSingleObject(proc,250)==WAIT_TIMEOUT)TerminateProcess(proc,1);CloseHandle(proc);}
    fpe_free(bytes);return err;
}
