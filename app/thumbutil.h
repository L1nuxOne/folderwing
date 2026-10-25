/* Thumbnail geometry and a byte-bounded LRU. No filesystem work. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>
#define THUMB_MAX 512
#define THUMB_WANTS 256
#define THUMB_EDGE_MAX 512

typedef struct thumb_grid {
    int columns, cell_w, cell_h, rows;
} thumb_grid;
thumb_grid thumb_grid_make(int width, int height, int edge, int gap, int label_h);
int thumb_grid_top(thumb_grid g, uint32_t count, int top);
int thumb_grid_hit(thumb_grid g, uint32_t count, int top, int x, int y);
void thumb_fit(double w, double h, uint32_t edge, int enlarge, uint32_t *ow, uint32_t *oh);

typedef struct thumb_entry {
    wchar_t *path;
    uint64_t size, used;
    int64_t mtime;
    uint32_t edge, w, h;
    int error, pinned;
    size_t bytes;
    void *data;
} thumb_entry;
typedef struct thumb_cache {
    thumb_entry entries[THUMB_MAX];
    size_t bytes, limit;
    uint64_t clock;
    void (*dispose)(void *);
} thumb_cache;
void thumb_cache_init(thumb_cache *c, size_t limit, void (*dispose)(void *));
void thumb_cache_clear(thumb_cache *c);
thumb_entry *thumb_cache_find(thumb_cache *c, const wchar_t *path, uint64_t size, int64_t mtime,
                              uint32_t edge);
/* Takes data only on success. Pinned visible entries are never evicted for look-ahead. */
thumb_entry *thumb_cache_put(thumb_cache *c, const wchar_t *path, uint64_t size, int64_t mtime, uint32_t edge,
                             uint32_t w, uint32_t h, int error, void *data, size_t bytes, int pinned);
