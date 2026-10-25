#include "internal.h"

/* ---- heap ---------------------------------------------------------------- */

void *fpe_alloc(size_t n) { return HeapAlloc(GetProcessHeap(), 0, n ? n : 1); }
void *fpe_zalloc(size_t n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1); }

void *fpe_realloc(void *p, size_t n)
{
    if (!p) return fpe_alloc(n);
    return HeapReAlloc(GetProcessHeap(), 0, p, n ? n : 1);
}

void fpe_free(void *p)
{
    if (p) HeapFree(GetProcessHeap(), 0, p);
}

wchar_t *fpe_wcsdup(const wchar_t *s)
{
    size_t n = wcslen(s) + 1;
    wchar_t *d = fpe_alloc(n * sizeof(wchar_t));
    if (d) memcpy(d, s, n * sizeof(wchar_t));
    return d;
}

/* ---- vbuf ---------------------------------------------------------------- */

#define VB_PAGE  4096u
#define VB_GRAIN (64u * 1024u)
#define VB_MAX_STEP (1024u * 1024u)

static size_t round_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

int fpe_vbuf_init(fpe_vbuf *b, size_t reserve)
{
    reserve = round_up(reserve ? reserve : VB_GRAIN, VB_GRAIN);
    b->base = VirtualAlloc(NULL, reserve, MEM_RESERVE, PAGE_READWRITE);
    b->used = 0;
    b->committed = 0;
    b->reserved = b->base ? reserve : 0;
    return b->base != NULL;
}

void *fpe_vbuf_push(fpe_vbuf *b, size_t n)
{
    size_t need = b->used + n;
    if (need < b->used) return NULL;

    if (need > b->committed) {
        /* Grow commits geometrically (capped) so big folders need few
         * VirtualAlloc calls while small ones stay at a page or two. */
        size_t step = b->committed < VB_PAGE ? VB_PAGE : b->committed;
        if (step > VB_MAX_STEP) step = VB_MAX_STEP;
        size_t want = round_up(need > b->committed + step ? need : b->committed + step, VB_PAGE);

        if (want > b->reserved) {
            size_t nr = b->reserved ? b->reserved : VB_GRAIN;
            while (nr < want) nr *= 2;
            uint8_t *nb = VirtualAlloc(NULL, nr, MEM_RESERVE, PAGE_READWRITE);
            if (!nb) return NULL;
            if (!VirtualAlloc(nb, want, MEM_COMMIT, PAGE_READWRITE)) {
                VirtualFree(nb, 0, MEM_RELEASE);
                return NULL;
            }
            if (b->base) {
                memcpy(nb, b->base, b->used);
                VirtualFree(b->base, 0, MEM_RELEASE);
            }
            b->base = nb;
            b->reserved = nr;
        } else if (!VirtualAlloc(b->base + b->committed, want - b->committed, MEM_COMMIT, PAGE_READWRITE)) {
            return NULL;
        }
        b->committed = want;
    }

    void *p = b->base + b->used;
    b->used = need;
    return p;
}

void fpe_vbuf_trim(fpe_vbuf *b)
{
    size_t keep = round_up(b->used ? b->used : 1, VB_PAGE);
    if (b->base && keep < b->committed) {
        VirtualFree(b->base + keep, b->committed - keep, MEM_DECOMMIT);
        b->committed = keep;
    }
}

void fpe_vbuf_free(fpe_vbuf *b)
{
    if (b->base) VirtualFree(b->base, 0, MEM_RELEASE);
    memset(b, 0, sizeof *b);
}

/* ---- paths --------------------------------------------------------------- */

static int is_sep(wchar_t c) { return c == L'\\' || c == L'/'; }

static fpe_err path_display(const wchar_t *in, wchar_t **out, int operand)
{
    *out = NULL;
    if (!in || !in[0]) return ERROR_INVALID_PARAMETER;
    int64_t t = fpe_prof_begin();

    wchar_t *src = NULL;
    size_t n = wcslen(in);

    if (n >= 8 && wcsncmp(in, L"\\\\?\\UNC\\", 8) == 0) {
        src = fpe_alloc((n - 6 + 1) * sizeof(wchar_t));
        if (!src) return ERROR_NOT_ENOUGH_MEMORY;
        src[0] = L'\\';
        memcpy(src + 1, in + 7, (n - 7 + 1) * sizeof(wchar_t));
    } else if (n >= 4 && wcsncmp(in, L"\\\\?\\", 4) == 0) {
        src = fpe_wcsdup(in + 4);
    } else if (n == 2 && in[1] == L':') {
        /* "C:" alone means "current dir on C:" to Win32; users mean the root. */
        src = fpe_alloc(4 * sizeof(wchar_t));
        if (src) { src[0] = in[0]; src[1] = L':'; src[2] = L'\\'; src[3] = 0; }
    } else {
        src = fpe_wcsdup(in);
    }
    if (!src) return ERROR_NOT_ENOUGH_MEMORY;

    /* Linux names may end in dots/spaces, including the literal " . ".
     * Win32 canonicalization would silently turn that file into its parent.
     * Keep absolute WSL paths in their native namespace; Linux resolves .. . */
    for(wchar_t *p=src;*p;p++)if(*p==L'/')*p=L'\\';
    int absolute=(src[0]==L'\\'&&src[1]==L'\\')||(src[0]&&src[1]==L':'&&src[2]==L'\\');
    if(absolute&&fpe_wsl_path_kind(src)){
        size_t len=wcslen(src);while(len>3&&is_sep(src[len-1]))src[--len]=0;
        *out=src;FPE_PROF_END("path.normalize",t);return 0;
    }

    /* Job operands must not silently become a different Windows entry.
     * Reject literal suffixes before GetFullPathName strips them, including
     * intermediate components and extended-path input. Typed . and .. retain
     * their navigation meaning. Linux operands above keep their exact names. */
    if (operand) {
        const wchar_t *part = src;
        for (const wchar_t *p = src;; ++p) {
            if (!*p || is_sep(*p)) {
                size_t chars = (size_t)(p - part);
                int navigation = (chars == 1 && part[0] == L'.') ||
                                 (chars == 2 && part[0] == L'.' && part[1] == L'.');
                if (chars && !navigation && (p[-1] == L'.' || p[-1] == L' ')) {
                    fpe_free(src);
                    return ERROR_INVALID_NAME;
                }
                if (!*p) break;
                part = p + 1;
            }
        }
    }

    DWORD need = GetFullPathNameW(src, 0, NULL, NULL);
    if (!need) { DWORD err = GetLastError(); fpe_free(src); return err; }
    wchar_t *full = fpe_alloc((size_t)need * sizeof(wchar_t));
    if (!full) { fpe_free(src); return ERROR_NOT_ENOUGH_MEMORY; }
    DWORD len = GetFullPathNameW(src, need, full, NULL);
    fpe_free(src);
    if (!len || len >= need) { fpe_free(full); return len ? ERROR_BUFFER_OVERFLOW : GetLastError(); }

    for (DWORD i = 0; i < len; i++) if (full[i] == L'/') full[i] = L'\\';

    /* Keep "C:\" but drop separators everywhere else. */
    while (len > 3 && is_sep(full[len - 1])) full[--len] = 0;
    *out = full;
    FPE_PROF_END("path.normalize", t);
    return 0;
}

fpe_err fpe_path_display(const wchar_t *in, wchar_t **out)
{ return path_display(in, out, 0); }

fpe_err fpe_path_operand(const wchar_t *in, wchar_t **out)
{ return path_display(in, out, 1); }

fpe_err fpe_path_nt(const wchar_t *display, wchar_t **out)
{
    *out=NULL;
    if(fpe_wsl_requires_native(display)&&!fpe_wsl_native(display))return ERROR_NOT_SUPPORTED;
    size_t n = wcslen(display);
    const wchar_t *prefix;
    const wchar_t *rest;

    if (n >= 4 && (wcsncmp(display, L"\\\\?\\", 4) == 0 || wcsncmp(display, L"\\\\.\\", 4) == 0)) {
        prefix = L"";
        rest = display;
    } else if (n >= 2 && display[0] == L'\\' && display[1] == L'\\') {
        prefix = L"\\\\?\\UNC\\";
        rest = display + 2;
    } else {
        prefix = L"\\\\?\\";
        rest = display;
    }

    size_t pl = wcslen(prefix), rl = wcslen(rest);
    /* +2: a root like "\\?\C:" needs its trailing "\" back; plus NUL. */
    wchar_t *p = fpe_alloc((pl + rl + 2) * sizeof(wchar_t));
    if (!p) return ERROR_NOT_ENOUGH_MEMORY;
    memcpy(p, prefix, pl * sizeof(wchar_t));
    memcpy(p + pl, rest, (rl + 1) * sizeof(wchar_t));
    *out = p;
    return 0;
}

/* WSL server/distro names are insensitive; Linux path components are exact. */
static size_t path_sensitive_at(const wchar_t *s)
{
    size_t prefix=0;
    if(!_wcsnicmp(s,L"\\\\wsl.localhost\\",16))prefix=16;
    else if(!_wcsnicmp(s,L"\\\\wsl$\\",7))prefix=7;
    else if(s[0]&&s[1]==L':'&&fpe_wsl_path_kind(s))return 2;
    else return wcslen(s);
    const wchar_t *p=wcschr(s+prefix,L'\\');return p?(size_t)(p-s):wcslen(s);
}
FPE_API int fpe_path_equal(const wchar_t *a,const wchar_t *b)
{
    if(!a||!b)return 0;
    fpe_text_init();
    size_t at=path_sensitive_at(a),bt=path_sensitive_at(b),i=0;
    for(;a[i]&&b[i];i++)if((i<at?fpe_fold(a[i]):a[i])!=(i<bt?fpe_fold(b[i]):b[i]))return 0;
    return a[i]==b[i];
}
uint32_t fpe_path_hash(const wchar_t *s)
{
    fpe_text_init();
    uint32_t h=2166136261u;size_t at=path_sensitive_at(s);
    for(size_t i=0;s[i];i++){h^=(uint16_t)(i<at?fpe_fold(s[i]):s[i]);h*=16777619u;}return h;
}

/* ---- time ---------------------------------------------------------------- */

uint64_t fpe_now_us(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart / freq.QuadPart) * 1000000 +
                      (c.QuadPart % freq.QuadPart) * 1000000 / freq.QuadPart);
}
