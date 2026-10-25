/* Immutable shared text snapshots. A bank stays owned by the viewer until
 * replaced; the helper may only write a free bank named by a request. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#define TEXTP_BANK_BYTES (48u * 1024u * 1024u)
#define TEXTP_BANKS 2
#define TEXTP_MAX_CHARS (2u * 1024u * 1024u)
#define TEXTP_PATH_MAX 32768u
#define TEXTP_MATCH_MAX 10000u
#define TEXTP_QUERY_MAX 256u
#define TEXTP_PARTIAL 1u
#define TEXTP_REPLACED 2u
#define TEXTP_GUESSED 4u
#define TEXTP_BINARY 8u
#define TEXTP_ENCODING_CHANGED 16u
/* All paths/text are UTF-16. Requests are one head followed by size bytes. */
enum {
    TEXTP_OPEN = 1,
    TEXTP_CLOSE,
    TEXTP_LAYOUT,
    TEXTP_FIND,
    TEXTP_COPY,
    TEXTP_QUIT,
    TEXTP_READY = 101,
    TEXTP_FOUND,
    TEXTP_COPIED
};
typedef struct textp_head {
    uint32_t type, size, doc, ticket;
} textp_head;
typedef struct textp_open {
    uint32_t bank, limit_mb, columns, encoding, language; /* path follows */
} textp_open;
typedef struct textp_layout {
    uint32_t bank, columns; /* 0: no wrap */
} textp_layout;
typedef struct textp_info {
    int32_t err;
    uint32_t bank, chars, rows, lines, max_columns, columns, encoding, language, flags;
    uint64_t bytes, read_bytes;
} textp_info;
/* Bank: chars+1 UTF-16 units, then chars style bytes, then aligned row records. */
typedef struct textp_row {
    uint32_t start, end, line;
} textp_row;
static inline size_t textp_style_offset(uint32_t n) { return ((size_t)n + 1) * 2; }
static inline size_t textp_columns_offset(uint32_t n) { return (textp_style_offset(n) + n + 3) & ~(size_t)3; }
static inline size_t textp_rows_offset(uint32_t n) { return textp_columns_offset(n) + ((size_t)n + 1) * 4; }
static inline size_t textp_snapshot_bytes(uint32_t n, uint32_t rows)
{
    return textp_rows_offset(n) + (size_t)rows * sizeof(textp_row);
}
typedef struct textp_find {
    uint32_t gen, chars; /* query follows, including NUL */
} textp_find;
typedef struct textp_found {
    int32_t err;
    uint32_t gen, count, more, chars; /* offsets follow */
} textp_found;
typedef struct textp_copy {
    uint32_t bank, start, end;
} textp_copy;
typedef struct textp_copied {
    int32_t err;
    uint32_t bank, bytes; /* bank contains NUL-terminated clipboard UTF-16 */
} textp_copied;
