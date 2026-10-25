#ifndef OPENPILOT_CONTENTWIRE_H
#define OPENPILOT_CONTENTWIRE_H
#include <stdint.h>
#define CF_MAGIC 0x43464e44u
#define CF_VERSION 1u
#define CF_QUERY 128u
#define CF_EXCERPT 256u
#define CF_RESULTS 10000u
#define CF_FILES 100000u
#define CF_FILE_BYTES (2u*1024u*1024u)
#define CF_TOTAL_BYTES (1024ull*1024*1024)
#define CF_NAMES_BYTES (32u*1024u*1024u)
#define CF_RECURSE 1u
#define CF_CASE 2u
#define CF_WORD 4u
#define CF_MATCH 1u
#define CF_PROGRESS 2u
#define CF_END 3u
#define CF_RUNNING 0u
#define CF_COMPLETE 1u
#define CF_PARTIAL 2u
#define CF_STOPPED 3u
typedef struct cf_options {uint32_t flags;uint16_t query[CF_QUERY],pattern[CF_QUERY];} cf_options;
typedef struct cf_hit {
 uint32_t line,column,length,encoding,flags,excerpt_column,highlight;
 uint16_t matched[CF_QUERY],excerpt[CF_EXCERPT];
} cf_hit;
typedef struct cf_summary {
 uint64_t scanned,files,matches,excluded,binary,large,errors,bytes;
 uint32_t state,error;
} cf_summary;
typedef struct cf_record {
 uint32_t magic,type,path_chars,reserved;
 uint64_t bytes,mtime;
 cf_hit hit;cf_summary summary;
} cf_record;
_Static_assert(sizeof(cf_options)==516,"content options layout");
_Static_assert(sizeof(cf_record)==904,"content stream layout");
#endif
