/* Private metadata-only analyzer stream, shared with the native Linux helper. */
#ifndef OPENPILOT_SPACEWIRE_H
#define OPENPILOT_SPACEWIRE_H
#include <stdint.h>
#define SP_MAGIC 0x53504345u
#define SP_VERSION 1u
#define SP_LIMIT 500000u
#define SP_NTFS_LIMIT 4000000u
#define SP_NAMES_LIMIT (64u*1024u*1024u)
#define SP_NTFS_NAMES_LIMIT (512u*1024u*1024u)
#define SP_SUMMARY_LIMIT 1000000u
#define SP_SUMMARY_NAMES_LIMIT (128u*1024u*1024u)
#define SP_ANALYSIS_LIMIT 8000000u
#define SP_ANALYSIS_NAMES_LIMIT (512u*1024u*1024u)
#define SP_NONE UINT32_MAX
#define SP_DIR 1u
#define SP_SKIP 2u
#define SP_FAILED 4u
#define SP_ALLOC_UNKNOWN 8u
#define SP_HARDLINK 16u
#define SP_DONE 32u
#define SP_REMOVED 64u
#define SP_LAZY 128u
#define SP_TOTAL_DONE 256u
#define SP_ENTRY 1u
#define SP_LEAVE 2u
#define SP_END 3u
#define SP_RESET 4u
#define SP_UPDATE 5u
#define SP_REMOVE 6u
#define SP_STATUS 7u
#define SP_TOTAL 8u
#define SP_PAGE 9u
#define SP_PAGE_MORE 1u
#define SP_PAGE_FILES 4096u
#define SP_PAGE_NAMES (4u*1024u*1024u)
/* Page command alone appends this request after the ordinary path request.
 * page is a zero-based partition in directory enumeration order. Each partition
 * is bounded by rows AND worst-case decoded name bytes. Zero times start a new
 * listing; subsequent requests bind to the same directory identity and dates.
 * SP_PAGE returns logical=page, allocated=first file ordinal, reserved=row count,
 * device/identity=mtime/ctime seconds, id/parent=their nanoseconds, flags=MORE.
 * A directory membership change fails the entire page with ESTALE. */
typedef struct sp_page_request {
 uint64_t page,device,identity,mtime,ctime;
 uint32_t mtime_ns,ctime_ns;
} sp_page_request;
_Static_assert(sizeof(sp_page_request)==48,"analyzer page request layout");
#define SP_BACKEND_NTFS 1u
#define SP_BACKEND_SUMMARY 2u
#define SP_BACKEND_ANALYSIS 3u
#define SP_RUNNING 0u
#define SP_COMPLETE 1u
#define SP_PARTIAL 2u
#define SP_STOPPED 3u
/* ENTRY names are one UTF-8/raw Linux component; root has an empty name.
 * In direct-list mode reserved=1 hints that file allocation is shared by links.
 * TOTAL is summary-only: direct logical/allocated bytes; device=file count;
 * parent=excluded; error=failed files/enumerations; flags=unknown allocations;
 * reserved=hardlinks. It is applied once per folder, independently of listings.
 * END carries state in flags and first stream error in error. */
typedef struct sp_record {
 uint32_t magic,type,id,parent,flags,error,chars,reserved;
 uint64_t logical,allocated,device,identity;
} sp_record;
_Static_assert(sizeof(sp_record)==64,"analyzer wire layout");
#endif
