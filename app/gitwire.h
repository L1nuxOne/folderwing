/* Private read-only native Git bridge. Commands are fixed operations. */
#pragma once
#include <stdint.h>
#define GR_MAGIC 0x47545231u
#define GR_VERSION 1u
#define GR_STATUS 1u
#define GR_BLOB 2u
#define GR_WORK 3u
#define GR_STATUS_LIMIT (2u*1024u*1024u)
#define GR_BLOB_LIMIT (16u*1024u*1024u)
typedef struct gr_request {uint32_t magic,version,op,root_bytes,arg_bytes,reserved;} gr_request;
typedef struct gr_reply {uint32_t magic,error,code,root_bytes,bytes,reserved;} gr_reply;
