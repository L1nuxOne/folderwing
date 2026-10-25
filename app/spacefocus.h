/* Disposable direct-file listings. Directory identities and totals stay in A.model. */
#pragma once
#define SPACE_DETAIL_BIT 0x80000000u
#define SPACE_CACHE_SLOTS 8u
#define SPACE_CACHE_FILES 65536u
#define SPACE_CACHE_NAMES (8u*1024u*1024u)
#define SPACE_LIST_FILES SP_PAGE_FILES
#define SPACE_LIST_NAMES SP_PAGE_NAMES
typedef struct focus_entry {uint32_t scope,count;uint64_t names,used;DWORD error;int valid,more,pending,fresh;uint64_t first,target;sp_page_request page;} focus_entry;
typedef struct focus_job {HANDLE thread,cancel;sp_model model;wchar_t *path;uint64_t epoch,device,identity;uint32_t scope;int wsl,as_root,more,page_seen;DWORD error;uint64_t first;sp_page_request request,result;} focus_job;
static struct {focus_entry entries[SPACE_CACHE_SLOTS];focus_job *job;uint64_t clock,names;uint32_t files;DWORD error;} F;
static void focus_reset(void);
static int focus_tick(void);
static int focus_shutdown(void);
static sp_node *display_node(uint32_t id){if(id&SPACE_DETAIL_BIT){id&=~SPACE_DETAIL_BIT;return id<A.details.count?A.details.nodes+id:NULL;}return id<A.model.count?A.model.nodes+id:NULL;}
static wchar_t *display_path(uint32_t id){return id&SPACE_DETAIL_BIT?space_model_path(&A.details,id&~SPACE_DETAIL_BIT,A.details_root):space_model_path(&A.model,id,A.root);}
