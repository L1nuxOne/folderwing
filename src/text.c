#include "internal.h"

uint16_t fpe_fold_table[65536];

/* Rank of each printable ASCII char under the user's collation (case
 * folded), so the fast path orders punctuation, digits and letters the way
 * Explorer does without calling into NLS for every comparison. */
static uint16_t g_ascii_weight[128];
static INIT_ONCE g_text_once = INIT_ONCE_STATIC_INIT;

static void fold_range(uint32_t lo, uint32_t hi)
{
    int n = (int)(hi - lo);
    wchar_t *src = fpe_alloc((size_t)n * sizeof(wchar_t));
    wchar_t *dst = fpe_alloc((size_t)n * sizeof(wchar_t));
    if (src && dst) {
        for (int i = 0; i < n; i++) src[i] = (wchar_t)(lo + (uint32_t)i);
        int r = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, src, n, dst, n, NULL, NULL, 0);
        if (r == n)
            for (int i = 0; i < n; i++) fpe_fold_table[lo + (uint32_t)i] = (uint16_t)dst[i];
    }
    fpe_free(src);
    fpe_free(dst);
}

static int nls_cmp_char(wchar_t a, wchar_t b)
{
    int r = CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, &a, 1, &b, 1, NULL, NULL, 0);
    return r ? r - CSTR_EQUAL : (a > b) - (a < b);
}

static BOOL CALLBACK text_init_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    int64_t t = fpe_prof_begin();
    for (uint32_t i = 0; i < 65536; i++) fpe_fold_table[i] = (uint16_t)i;
    fold_range(0x0000, 0xD800);
    fold_range(0xE000, 0x10000);

    wchar_t order[128];
    int n = 0;
    for (wchar_t c = 0x20; c < 0x7F; c++) order[n++] = c;
    for (int i = 1; i < n; i++) {
        wchar_t v = order[i];
        int j = i;
        while (j > 0 && nls_cmp_char(order[j - 1], v) > 0) { order[j] = order[j - 1]; j--; }
        order[j] = v;
    }
    uint16_t rank = 1;
    for (int i = 0; i < n; i++) {
        if (i > 0 && nls_cmp_char(order[i - 1], order[i]) != 0) rank++;
        g_ascii_weight[(int)order[i]] = rank;
    }
    FPE_PROF_END("text.init", t);
    return TRUE;
}

void fpe_text_init(void)
{
    InitOnceExecuteOnce(&g_text_once, text_init_once, NULL, NULL);
}

int fpe_is_ascii(const wchar_t *s, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if ((uint16_t)s[i] >= 0x80) return 0;
    return 1;
}

static inline int is_digit(wchar_t c) { return c >= L'0' && c <= L'9'; }

static int ordinal_cmp(const wchar_t *a, uint32_t na, const wchar_t *b, uint32_t nb)
{
    uint32_t n = na < nb ? na : nb;
    for (uint32_t i = 0; i < n; i++)
        if (a[i] != b[i]) return (uint16_t)a[i] < (uint16_t)b[i] ? -1 : 1;
    return (na > nb) - (na < nb);
}

int fpe_name_cmp(const wchar_t *a, uint32_t na, int a_ascii,
                 const wchar_t *b, uint32_t nb, int b_ascii)
{
    if (!(a_ascii && b_ascii)) {
        /* Counted, not timed: this runs inside sorts, millions of times. */
        FPE_PROF_COUNT("text.nls_compare");
        int r = CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS,
                                a, (int)na, b, (int)nb, NULL, NULL, 0);
        if (r) return r != CSTR_EQUAL ? r - CSTR_EQUAL : ordinal_cmp(a, na, b, nb);
    }

    uint32_t i = 0, j = 0;
    int zeros = 0;
    while (i < na && j < nb) {
        wchar_t ca = a[i], cb = b[j];
        if (is_digit(ca) && is_digit(cb)) {
            uint32_t za = i, zb = j;
            while (i < na && a[i] == L'0') i++;
            while (j < nb && b[j] == L'0') j++;
            uint32_t sa = i, sb = j;
            while (i < na && is_digit(a[i])) i++;
            while (j < nb && is_digit(b[j])) j++;
            uint32_t la = i - sa, lb = j - sb;
            if (la != lb) return la < lb ? -1 : 1;
            for (uint32_t k = 0; k < la; k++)
                if (a[sa + k] != b[sb + k]) return a[sa + k] < b[sb + k] ? -1 : 1;
            if (!zeros && (sa - za) != (sb - zb)) zeros = (sa - za) < (sb - zb) ? -1 : 1;
            continue;
        }
        uint16_t wa = (uint16_t)ca < 128 ? g_ascii_weight[(uint16_t)ca] : (uint16_t)ca;
        uint16_t wb = (uint16_t)cb < 128 ? g_ascii_weight[(uint16_t)cb] : (uint16_t)cb;
        if (wa != wb) return wa < wb ? -1 : 1;
        i++;
        j++;
    }
    if (i < na) return 1;
    if (j < nb) return -1;
    if (zeros) return zeros;
    return ordinal_cmp(a, na, b, nb);
}

int fpe_equal_nocase(const wchar_t *a, uint32_t na, const wchar_t *b, uint32_t nb)
{
    if (na != nb) return 0;
    for (uint32_t i = 0; i < na; i++)
        if (a[i] != b[i] && fpe_fold(a[i]) != fpe_fold(b[i])) return 0;
    return 1;
}

int fpe_contains_folded(const wchar_t *hay, uint32_t nh, const wchar_t *needle, uint32_t nn)
{
    if (nn == 0) return 1;
    if (nn > nh) return 0;
    wchar_t first = needle[0];
    for (uint32_t i = 0; i + nn <= nh; i++) {
        if (fpe_fold(hay[i]) != first) continue;
        uint32_t k = 1;
        while (k < nn && fpe_fold(hay[i + k]) == needle[k]) k++;
        if (k == nn) return 1;
    }
    return 0;
}

FPE_API int fpe_compare_names(const wchar_t *a, const wchar_t *b)
{
    fpe_text_init();
    uint32_t na = (uint32_t)wcslen(a), nb = (uint32_t)wcslen(b);
    return fpe_name_cmp(a, na, fpe_is_ascii(a, na), b, nb, fpe_is_ascii(b, nb));
}
