/* fuzzy.h - ranking short lists (commands, folders) as the user types.
 *
 * A pattern is one or more space-separated terms; every term must appear in
 * the text as a subsequence, ignoring case. Matches score higher at word
 * starts (after a space, \, _, -, . or a lowercase-to-uppercase step), in a
 * run, and in the last path segment; gaps and long texts cost a little.
 * No allocation, UI thread only (one static table): cheap enough to rerun
 * over a few hundred items per key. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define FUZZY_TEXT_MAX 256 /* longer texts are matched on their first FUZZY_TEXT_MAX chars */
#define FUZZY_HITS_WORDS (FUZZY_TEXT_MAX / 32)

/* -1: no match. Otherwise a score, higher is better (an empty pattern
 * scores 0). hits, if given, gets one bit per matched char (FUZZY_HITS_WORDS words). */
int fuzzy_score(const wchar_t *pattern, const wchar_t *text, uint32_t *hits);

static inline int fuzzy_hit(const uint32_t *hits, size_t i)
{
    return i < FUZZY_HITS_WORDS * 32 && (hits[i >> 5] >> (i & 31)) & 1u;
}
