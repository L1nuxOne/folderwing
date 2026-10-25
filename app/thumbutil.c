#include "thumbutil.h"
#include "fpe.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
thumb_grid thumb_grid_make(int width, int height, int edge, int gap, int label_h)
{
    if (edge < 1)
        edge = 1;
    if (gap < 0)
        gap = 0;
    if (label_h < 1)
        label_h = 1;
    int cw = edge + 2 * gap, ch = edge + 2 * gap + label_h;
    int cols = width / cw;
    if (cols < 1)
        cols = 1;
    /* Spread the spare width over columns; a narrow pane clips one tile. */
    if (width >= cw)
        cw = width / cols;
    int rows = height / ch;
    return (thumb_grid){cols, cw, ch, rows > 0 ? rows : 1};
}
int thumb_grid_top(thumb_grid g, uint32_t count, int top)
{
    int last = count ? (int)((count - 1) / (uint32_t)g.columns) - g.rows + 1 : 0;
    if (last < 0)
        last = 0;
    top = top > 0 ? top / g.columns : 0;
    if (top > last)
        top = last;
    return top * g.columns;
}
int thumb_grid_hit(thumb_grid g, uint32_t count, int top, int x, int y)
{
    if (x < 0 || y < 0 || x >= g.cell_w * g.columns)
        return -1;
    uint64_t r = (uint64_t)top + (uint32_t)(y / g.cell_h) * g.columns + x / g.cell_w;
    return r < count ? (int)r : -1;
}
void thumb_fit(double w, double h, uint32_t edge, int enlarge, uint32_t *ow, uint32_t *oh)
{
    *ow = *oh = 0;
    if (!isfinite(w) || !isfinite(h) || w <= 0 || h <= 0 || !edge || edge > THUMB_EDGE_MAX)
        return;
    double s = edge / (w > h ? w : h);
    if (!enlarge && s > 1)
        s = 1;
    *ow = (uint32_t)(w * s + 0.5);
    *oh = (uint32_t)(h * s + 0.5);
    if (!*ow)
        *ow = 1;
    if (!*oh)
        *oh = 1;
    if (*ow > edge)
        *ow = edge;
    if (*oh > edge)
        *oh = edge;
}
static void drop(thumb_cache *c, thumb_entry *e)
{
    c->bytes -= e->bytes;
    if (e->data && c->dispose)
        c->dispose(e->data);
    free(e->path);
    memset(e, 0, sizeof *e);
}
void thumb_cache_init(thumb_cache *c, size_t limit, void (*dispose)(void *))
{
    memset(c, 0, sizeof *c);
    c->limit = limit;
    c->dispose = dispose;
}
void thumb_cache_clear(thumb_cache *c)
{
    for (unsigned i = 0; i < THUMB_MAX; ++i)
        drop(c, &c->entries[i]);
}
thumb_entry *thumb_cache_find(thumb_cache *c, const wchar_t *path, uint64_t size, int64_t mtime,
                              uint32_t edge)
{
    thumb_entry *found = NULL;
    for (unsigned i = 0; i < THUMB_MAX; ++i) {
        thumb_entry *e = &c->entries[i];
        if (!e->path || !fpe_path_equal(e->path, path))
            continue;
        if (e->size != size || e->mtime != mtime) {
            drop(c, e);
            continue;
        }
        if (e->edge == edge)
            found = e;
    }
    if (found)
        found->used = ++c->clock;
    return found;
}
thumb_entry *thumb_cache_put(thumb_cache *c, const wchar_t *path, uint64_t size, int64_t mtime, uint32_t edge,
                             uint32_t w, uint32_t h, int error, void *data, size_t bytes, int pinned)
{
    if (bytes > c->limit)
        return NULL;
    thumb_entry *old = thumb_cache_find(c, path, size, mtime, edge);
    if (old)
        drop(c, old);
    thumb_entry *slot = NULL;
    for (;;) {
        thumb_entry *lru = NULL;
        slot = NULL;
        for (unsigned i = 0; i < THUMB_MAX; ++i) {
            thumb_entry *e = &c->entries[i];
            if (!e->path)
                slot = e;
            else if (!e->pinned && (!lru || e->used < lru->used))
                lru = e;
        }
        if (slot && c->bytes <= c->limit - bytes)
            break;
        if (!lru)
            return NULL;
        drop(c, lru);
    }
    size_t n = (wcslen(path) + 1) * sizeof(wchar_t);
    wchar_t *copy = malloc(n);
    if (!copy)
        return NULL;
    memcpy(copy, path, n);
    *slot = (thumb_entry){.path = copy,
                          .size = size,
                          .mtime = mtime,
                          .edge = edge,
                          .w = w,
                          .h = h,
                          .error = error,
                          .data = data,
                          .bytes = bytes,
                          .used = ++c->clock,
                          .pinned = pinned};
    c->bytes += bytes;
    return slot;
}
