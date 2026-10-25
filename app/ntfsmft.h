/* NTFS metadata parser. Private to the on-demand analyzer worker. NTFS 3.x only. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#define MFT_NONE UINT32_MAX
#define MFT_INDEX_MASK UINT64_C(0x0000ffffffffffff)
enum {MF_ACTIVE=1,MF_DIRECTORY=2,MF_DATA=4,MF_ATTRLIST=8,MF_BAD=16};
typedef struct mft_run {uint64_t vcn,lcn,clusters;} mft_run;
typedef struct mft_name {uint64_t child,parent;uint32_t next,offset;uint16_t length,space;uint32_t valid;} mft_name;
typedef struct mft_node {uint64_t reference,logical,allocated,all_logical,all_allocated;uint32_t first,attributes;uint16_t flags,links,streams,reserved;} mft_node;
typedef struct mft_data {uint64_t child,logical,allocated;int unnamed;} mft_data;
typedef struct mft_model {
 mft_node *nodes;uint32_t slots,cluster_size;
 mft_name *names;uint32_t name_count,name_capacity;
 uint16_t *text;uint32_t text_count,text_capacity;
 mft_data *pending;uint32_t pending_count,pending_capacity;
 uint64_t records,active,extensions,unused,bad_fixup,bad_attributes,bad_sequence,dos_names,attribute_lists;
} mft_model;
uint16_t mft_u16(const void *p);
uint32_t mft_u32(const void *p);
uint64_t mft_u64(const void *p);
int mft_fixup(uint8_t *p,size_t size,uint32_t sector);
int mft_runs(const uint8_t *attribute,size_t length,mft_run *runs,uint32_t *count,uint32_t capacity,uint64_t total_clusters);
int mft_record(mft_model *m,const uint8_t *p,size_t size,uint32_t index);
void mft_resolve(mft_model *m);
void mft_free(mft_model *m);

