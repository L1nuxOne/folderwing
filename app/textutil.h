#pragma once
#include <stdint.h>
#include <wchar.h>
#include "textproto.h"
enum { TEXT_AUTO, TEXT_UTF8, TEXT_UTF16LE, TEXT_UTF16BE, TEXT_UTF32LE, TEXT_UTF32BE, TEXT_1252 };
enum {
    TEXT_PLAIN,
    TEXT_C,
    TEXT_PYTHON,
    TEXT_SCRIPT,
    TEXT_JSON,
    TEXT_CONFIG,
    TEXT_XML,
    TEXT_SQL,
    TEXT_MARKDOWN
};
enum { TEXT_NORMAL, TEXT_KEYWORD, TEXT_NUMBER, TEXT_STRING, TEXT_COMMENT, TEXT_LABEL };
typedef struct text_data {
    wchar_t *text;
    uint8_t *style;
    uint32_t chars, encoding, flags, language;
} text_data;
/* -1 allocation failure, -2 binary. prefix is true when more bytes follow. */
int text_decode(const uint8_t *bytes, size_t count, int prefix, uint32_t encoding, text_data *out);
void text_data_free(text_data *d);
int text_file_language(const wchar_t *name); /* -1 unsupported */
const wchar_t *text_language_name(uint32_t language);
const wchar_t *text_encoding_name(uint32_t encoding);
void text_highlight(text_data *d, uint32_t language);
uint32_t text_cell(const wchar_t *s, uint32_t at, uint32_t end, uint32_t column, uint32_t *units);
void text_columns(const wchar_t *text, uint32_t n, uint32_t *columns);
uint32_t text_layout(const wchar_t *text, uint32_t n, uint32_t columns, textp_row *rows, uint32_t capacity,
                     uint32_t *lines, uint32_t *max_columns);
/* Bounded KMP: the caller supplies folded strings for case-insensitive search. */
uint32_t text_matches(const wchar_t *text, uint32_t n, const wchar_t *query, uint32_t q, uint32_t *hits,
                      uint32_t capacity, uint32_t *more);
