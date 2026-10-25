/* Image helper protocol. Two shared pixel banks, each exclusively owned by
 * one request until its reply is consumed. No file or pixels on the UI pipe. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#define IMGP_PATH_MAX 32768
#define IMGP_EDGE 4096
#define IMGP_BANK_BYTES (IMGP_EDGE * IMGP_EDGE * 4u)
#define IMGP_BANKS 2
#define IMGP_MAX_DIM 262144u
#define IMGP_MAX_PIXELS (64u * 1024u * 1024u)
enum { IMGP_OPEN = 1, IMGP_CLOSE, IMGP_RENDER, IMGP_QUIT, IMGP_PAGE, IMGP_OPENED = 101, IMGP_RENDERED, IMGP_PAGED };
typedef struct imgp_head {
    uint32_t type, size, doc, ticket;
} imgp_head;
typedef struct imgp_open {
    uint32_t cache_mb; /* then NUL-terminated UTF-16 path */
} imgp_open;
typedef struct imgp_page { uint32_t page; } imgp_page;
typedef struct imgp_info {
    int32_t err;
    uint32_t w, h, orientation, frames, color; /* color: 0 absent, 1 managed, 2 unsupported */
    uint32_t animated, loops, partial; /* loops: total passes, 0 forever; partial SVG support */
    uint32_t pages, page; /* TIFF pages only; zero-based selected page */
    uint64_t bytes;
    wchar_t format[24], camera[96], date[64];
} imgp_info;
typedef struct imgp_render {
    uint32_t bank, rotation, flip, straight; /* user clockwise quarter turns, horizontal flip */
    uint32_t frame;
    uint32_t x, y, cw, ch, w, h;             /* crop in oriented coordinates, output size */
} imgp_render;
typedef struct imgp_rendered {
    uint32_t bank, w, h, delay_ms;
    int32_t err;
} imgp_rendered;
