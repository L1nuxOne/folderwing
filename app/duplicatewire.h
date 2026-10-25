/* Private rooted, read-only Linux channel. Never shared with transfer sessions. */
#pragma once
#include <stdint.h>
#define DU_MAGIC 0x44555031u
#define DU_VERSION 1u
#define DU_BLOCK 65536u
#define DU_OPEN 1u
#define DU_READ 2u
#define DU_HASH 3u
#define DU_PROGRESS 4u
/* Reply errors use native errno; the Windows client treats them as an
 * incomplete read, never as a successful equality result. */
typedef struct du_stamp {uint64_t size,device,identity,mtime,ctime;} du_stamp;
typedef struct du_request {uint32_t op,chars;du_stamp expected;} du_request;
typedef struct du_reply {uint32_t error,count;du_stamp stamp;uint8_t hash[32];} du_reply;
