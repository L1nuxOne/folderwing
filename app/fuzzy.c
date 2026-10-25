/* Fuzzy matching: see fuzzy.h.
 *
 * Each term is placed by dynamic programming over (term char, text
 * position): best[i][j] is the best score with term char i matched at text
 * position j, coming either straight from j-1 (a run, bonus) or from any
 * earlier position (a gap, penalty). That finds the best placement, not just
 * the first one: "ws" in "Windows System" takes the S of System. Cost is
 * terms x chars, on the stack. */
#include "fuzzy.h"

#include <string.h>
#include <wctype.h>

#define TERM_MAX 32
#define NEG      (-1000000)

#define MATCH  16
#define RUN    8  /* consecutive with the previous match */
#define GAP    3  /* any gap before this match */
#define START  6  /* the very first character */

static wchar_t low(wchar_t c) { return (wchar_t)towlower(c); }

static int is_sep(wchar_t c)
{
    return c == L' ' || c == L'\\' || c == L'/' || c == L'_' || c == L'-' || c == L'.' || c == L':' || c == L'(' ||
           c == L'[';
}

static int bonus_at(const wchar_t *t, size_t i, size_t last_seg)
{
    int b = 0;
    if (i == 0 || t[i - 1] == L'\\' || t[i - 1] == L'/') b = 10;
    else if (is_sep(t[i - 1])) b = 8;
    else if (iswlower(t[i - 1]) && iswupper(t[i])) b = 7;
    if (i >= last_seg) b += 3; /* the name, not its folders */
    return b;
}

/* Best score of one term (lowercase, length k) in t[0..n), or -1; its positions in pos. */
static int term_score(const wchar_t *term, size_t k, const wchar_t *t, const wchar_t *lt, size_t n, size_t last_seg,
                      size_t *pos)
{
    static int best[TERM_MAX][FUZZY_TEXT_MAX]; /* UI thread only */
    if (k > n) return -1;
    for (size_t i = 0; i < k; i++) {
        int run = NEG; /* best of row i-1 at positions <= j-2 */
        for (size_t j = 0; j < n; j++) {
            if (i && j >= 2 && best[i - 1][j - 2] > run) run = best[i - 1][j - 2];
            if (lt[j] != term[i] || j < i) {
                best[i][j] = NEG;
                continue;
            }
            int v;
            if (!i) {
                v = j == 0 ? START : 0;
            } else {
                int a = j >= 1 && best[i - 1][j - 1] > NEG ? best[i - 1][j - 1] + RUN : NEG;
                int b = run > NEG ? run - GAP : NEG;
                v = a > b ? a : b;
            }
            best[i][j] = v > NEG ? v + MATCH + bonus_at(t, j, last_seg) : NEG;
        }
    }
    size_t j = 0;
    for (size_t q = 1; q < n; q++)
        if (best[k - 1][q] > best[k - 1][j]) j = q;
    int score = best[k - 1][j];
    if (score <= NEG) return -1;

    /* walk back: which predecessor gave each cell its value */
    for (size_t i = k - 1; i > 0; i--) {
        pos[i] = j;
        int v = best[i][j] - MATCH - bonus_at(t, j, last_seg);
        if (j >= 1 && best[i - 1][j - 1] > NEG && best[i - 1][j - 1] + RUN == v) {
            j--;
            continue;
        }
        size_t from = j - 2;
        while (from > 0 && best[i - 1][from] - GAP != v) from--;
        j = from;
    }
    pos[0] = j;
    return score;
}

int fuzzy_score(const wchar_t *pattern, const wchar_t *text, uint32_t *hits)
{
    if (hits) memset(hits, 0, FUZZY_HITS_WORDS * sizeof *hits);
    size_t n = wcslen(text);
    if (n > FUZZY_TEXT_MAX) n = FUZZY_TEXT_MAX;
    wchar_t lt[FUZZY_TEXT_MAX];
    size_t last_seg = 0;
    for (size_t i = 0; i < n; i++) {
        lt[i] = low(text[i]);
        if ((text[i] == L'\\' || text[i] == L'/') && i + 1 < n) last_seg = i + 1;
    }

    int total = 0, terms = 0;
    const wchar_t *p = pattern;
    while (*p) {
        while (*p == L' ') p++;
        if (!*p) break;
        wchar_t term[TERM_MAX];
        size_t k = 0;
        for (; *p && *p != L' '; p++)
            if (k < TERM_MAX) term[k++] = low(*p);
        size_t pos[TERM_MAX];
        int sc = term_score(term, k, text, lt, n, last_seg, pos);
        if (sc < 0) return -1;
        total += sc;
        terms++;
        if (hits)
            for (size_t j = 0; j < k; j++) hits[pos[j] >> 5] |= 1u << (pos[j] & 31);
    }
    if (!terms) return 0;
    total -= (int)(n / 16); /* among equals, the shorter text */
    return total < 0 ? 0 : total;
}
