#pragma once
#include <wchar.h>
#include <stdint.h>
#define RENAME_TEXT 128
typedef struct rename_rules {
    wchar_t find[RENAME_TEXT],replace[RENAME_TEXT],prefix[RENAME_TEXT],suffix[RENAME_TEXT];
    uint32_t start,digits;int numbering,keep_extension;
} rename_rules;
/* Names enter/leave in filesystem display form. Literal case-sensitive replace,
 * prefix + body + suffix + optional _number + preserved last extension. */
int rename_transform(const wchar_t *from,int directory,int linux,uint32_t index,
                     const rename_rules *rules,wchar_t out[260]);
int rename_decimal(const wchar_t *text,uint32_t min,uint32_t max,uint32_t *out);
void rename_display(const wchar_t *name,int linux,wchar_t *out,size_t cap);
