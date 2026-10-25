/* Folder overrides are cached in memory. Read once at startup and saved on
 * exit, atomically merging only this window's edits with the latest file.
 * Path identity follows the engine: Windows ignores case, Linux preserves it.
 * Call from one thread. No directory enumeration or file metadata is needed. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stddef.h>

#define FV_MAX 256
#define FV_PATH_MAX 4096
#define FV_FILE_MAX (4u << 20)
enum { FV_SORT = 1, FV_MODE = 2, FV_SIZE = 4, FV_ALL = 7 };
typedef struct fv_value {
    uint32_t mask, mode, size, sort, descending;
} fv_value;
typedef struct fv_entry {
    wchar_t *path;
    fv_value value;
    uint32_t changed;
} fv_entry;
typedef struct folderviews {
    fv_entry entries[FV_MAX]; /* most recently changed first */
    uint32_t count;
    wchar_t *removed[FV_MAX]; /* resets, including reset followed by a new edit */
    uint32_t nremoved;
} folderviews;

void fv_free(folderviews *s);
int fv_same_path(const wchar_t *a, const wchar_t *b);
int fv_get(const folderviews *s, const wchar_t *path, fv_value *out);
/* Fields selected by value.mask replace those fields; the others are retained.
 * Return 1 for a change, 0 for no change, -1 for invalid input/allocation. */
int fv_set(folderviews *s, const wchar_t *path, fv_value value);
int fv_reset(folderviews *s, const wchar_t *path);
int fv_parse(folderviews *s, const char *text, size_t len);
size_t fv_format(const folderviews *s, char *text, size_t cap);
DWORD fv_load(folderviews *s, const wchar_t *file);
/* Missing files are empty. Read/lock/write errors leave the old file and
 * pending edits intact. A per-file mutex serializes other OpenPilot windows. */
DWORD fv_save(folderviews *s, const wchar_t *file);
