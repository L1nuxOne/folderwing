/* Private archive helper wire: names are counted UTF-16; data never names an output path. */
#pragma once
#include <stdint.h>
#define ARCP_MAGIC 0x41524350u
#define ARCP_VERSION 1u
#define ARCP_ENTRIES 100000u
#define ARCP_NAME 4096u
#define ARCP_NAMES_BYTES (32u*1024u*1024u)
#define ARCP_CHUNK 65536u
#define ARCP_EXTRACT_BYTES (16ull*1024ull*1024ull*1024ull)
#define ARCP_PREVIEW_BYTES (64ull*1024ull*1024ull)
enum {ARCP_OPEN=1,ARCP_READ,ARCP_QUIT,ARCP_INFO=100,ARCP_ENTRY,ARCP_LIST_END,ARCP_DATA,ARCP_READ_END};
enum {ARCP_DIR=1,ARCP_SPECIAL=2,ARCP_ENCRYPTED=4,ARCP_UNSUPPORTED=8,ARCP_UNKNOWN_SIZE=16,ARCP_UNSAFE=32};
typedef struct arcp_head {uint32_t magic,version,type,size;} arcp_head;
typedef struct arcp_id {uint64_t volume,file,size,mtime;} arcp_id;
typedef struct arcp_info {uint32_t error,backend;arcp_id id;} arcp_info;
typedef struct arcp_entry {uint32_t index,flags,chars,reserved;uint64_t size,packed;} arcp_entry;
typedef struct arcp_read {uint32_t index,reserved;uint64_t limit;} arcp_read;
typedef struct arcp_end {uint32_t error,count;uint64_t bytes;} arcp_end;
