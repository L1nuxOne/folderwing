#pragma once
#include <wchar.h>
#include <stddef.h>
#include "contentwire.h"
int content_options_valid(const cf_options *options);
int content_name_matches(const wchar_t *name,const uint16_t pattern[CF_QUERY]);
/* Decodes with the same codec as the text preview. 1 text, 0 binary, -1 allocation.
 * Results use normalized UTF-16 line/column positions, one row per occurrence. */
int content_match_bytes(const unsigned char *bytes,size_t size,const cf_options *options,
 uint32_t capacity,cf_hit **hits,uint32_t *count,int *more);
static inline int content_relative_safe(const wchar_t *p){if(!p||!*p||*p=='\\'||*p=='/')return 0;const wchar_t *part=p;for(;;p++){if(*p=='/'||*p==':'||(*p&&*p<32))return 0;if(!*p||*p=='\\'){size_t n=(size_t)(p-part);if(!n||(n==1&&part[0]=='.')||(n==2&&part[0]=='.'&&part[1]=='.'))return 0;if(!*p)return 1;part=p+1;}}}

