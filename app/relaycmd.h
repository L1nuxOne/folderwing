/* relaycmd.h - the parts of relays that need no window: command lines,
 * file types and menu texts. Shared by the app, folderwing-shell.exe and the tests. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

/* A relay's command line with its blanks filled in:
 *   %1 %L %V  the item (item < 0: the first one; the folder when there are
 *             none), in quotes unless the command line already has them there
 *   %*        every item, each in quotes
 *   %D %W     the folder the items are in
 *   %%        a %
 *   %NAME%    an environment variable (left as it is when not set)
 * Returns the length, or -1 when it doesn't fit cap. */
int relay_expand(const wchar_t *cmd, const wchar_t *const *paths, uint32_t n, int item, const wchar_t *dir,
                 wchar_t *out, size_t cap);
/* Has %*: one run takes every item (otherwise one run per item). */
int relay_takes_all(const wchar_t *cmd);
/* Splits a command line into the program (unquoted) and the rest. */
void relay_split(const wchar_t *cmd, wchar_t *program, size_t pcap, const wchar_t **rest);
/* Does path's extension fit types ("txt md", ".TXT, .md")? Empty types fit everything. */
int relay_type_ok(const wchar_t *path, const char *types);
/* Menu text the way relays compare it: no & marks (&& stays &), nothing
 * after a tab, no trailing "..." or "…", spaces trimmed, lower case. */
void relay_norm(const wchar_t *in, wchar_t *out, size_t cap);
/* The next part of "7-Zip > Add to archive..." (normalized) into seg;
 * returns what follows, or NULL after the last part. */
const wchar_t *relay_segment(const wchar_t *path, wchar_t *seg, size_t cap);
