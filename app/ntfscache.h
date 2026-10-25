#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "ntfsmft.h"
typedef struct ntfs_cache ntfs_cache;
typedef int (*ntfs_alive)(void *);
typedef void (*ntfs_progress)(void *,unsigned);
enum {NTFS_LOADING=1,NTFS_CACHED=2,NTFS_CATCHUP=3,NTFS_BASELINE=4,NTFS_LIVE=5,NTFS_PAUSED=6,NTFS_FALLBACK=7,NTFS_INCOMPLETE=8};
/* Caller retains read-only volume and impersonation token; cache owns no authority
 * to write a volume. Cache-file IO runs using the unelevated owner's token. */
DWORD ntfs_cache_open(HANDLE volume,HANDLE owner,const wchar_t *path,ntfs_alive alive,ntfs_progress progress,void *user,ntfs_cache **out);
DWORD ntfs_cache_poll(ntfs_cache *c);
DWORD ntfs_cache_save(ntfs_cache *c);
void ntfs_cache_close(ntfs_cache *c);
const mft_model *ntfs_cache_model(const ntfs_cache *c);
const uint32_t *ntfs_cache_changes(ntfs_cache *c,uint32_t *count,int *reset);
void ntfs_cache_ack_changes(ntfs_cache *c);
uint64_t ntfs_cache_generation(const ntfs_cache *c);
