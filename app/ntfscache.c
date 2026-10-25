/* NTFS model engine. Only the on-demand fpntfs worker links this module. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <inttypes.h>
#include "ntfscache.h"
#define READ_CHUNK (4u*1024u*1024u)
/* Raw MFT scans amortize volume-read overhead; cache/journal IO stays bounded
 * to READ_CHUNK. Keep the smaller allocation available under memory pressure. */
#define BASELINE_READ_CHUNK (32u*1024u*1024u)
#define MAX_SLOTS 16000000u
static double frequency;
static uint64_t ticks(void){LARGE_INTEGER n;QueryPerformanceCounter(&n);return n.QuadPart;}
static double ms(uint64_t n){return n*1000.0/frequency;}
static int read_at(HANDLE h,uint64_t offset,void *p,DWORD size){LARGE_INTEGER at;at.QuadPart=(LONGLONG)offset;DWORD done=0;if(offset>INT64_MAX||!SetFilePointerEx(h,at,NULL,FILE_BEGIN)||!ReadFile(h,p,size,&done,NULL)||done!=size)return 0;return 1;}
static int bootstrap(HANDLE volume,NTFS_VOLUME_DATA_BUFFER *geometry,mft_run *runs,uint32_t *count){
 BYTE data[sizeof(NTFS_VOLUME_DATA_BUFFER)+64];DWORD returned=0;
 if(!DeviceIoControl(volume,FSCTL_GET_NTFS_VOLUME_DATA,NULL,0,data,sizeof data,&returned,NULL)||returned<sizeof *geometry)return 0;memcpy(geometry,data,sizeof *geometry);
 if(returned<sizeof *geometry+8||mft_u16(data+sizeof *geometry+4)!=3||mft_u16(data+sizeof *geometry+6)>1){SetLastError(ERROR_NOT_SUPPORTED);return 0;}
 uint32_t size=geometry->BytesPerFileRecordSegment,sector=geometry->BytesPerSector,cluster=geometry->BytesPerCluster;
 if(size<512||size>65536||(size&(size-1))||sector<256||cluster<sector||size%sector||geometry->MftValidDataLength.QuadPart<=0||geometry->MftValidDataLength.QuadPart%size||geometry->MftStartLcn.QuadPart<0){SetLastError(ERROR_INVALID_DATA);return 0;}
 uint8_t *record=malloc(size);if(!record)return 0;int ok=read_at(volume,(uint64_t)geometry->MftStartLcn.QuadPart*cluster,record,size)&&mft_fixup(record,size,sector);uint32_t used=ok?mft_u32(record+24):0,at=ok?mft_u16(record+20):0;int found=0;
 if(used>size||at<48||at>used)ok=0;
 while(ok&&at+16<=used){uint8_t *a=record+at;uint32_t type=mft_u32(a),length=mft_u32(a+4);if(type==UINT32_MAX)break;if(length<24||length>used-at||length%8){ok=0;break;}
  /* This bounded first prototype rejects a fragmented bootstrap requiring an
   * attribute-list chase instead of silently missing any part of the MFT. */
  if(type==0x20){ok=0;SetLastError(ERROR_NOT_SUPPORTED);break;}
  if(type==0x80&&!a[9]){if(found||!a[8]||mft_u64(a+16)||mft_u16(a+12)||!mft_runs(a,length,runs,count,4096,(uint64_t)geometry->TotalClusters.QuadPart)){ok=0;break;}found=1;}
  at+=length;
 }
 uint64_t expected=0;for(uint32_t i=0;ok&&i<*count;i++){if(runs[i].vcn!=expected||expected>UINT64_MAX-runs[i].clusters)ok=0;expected+=runs[i].clusters;}
 if(!found||expected<((uint64_t)geometry->MftValidDataLength.QuadPart+cluster-1)/cluster)ok=0;
 free(record);if(!ok&&GetLastError()==ERROR_SUCCESS)SetLastError(ERROR_INVALID_DATA);return ok;
}
#include <bcrypt.h>
#include <math.h>
#define CACHE_VERSION 2
#define HOT_MAX 4096u
#define CHANGE_MAX 2000000u
#define LIST_MAX (1024u*1024u)
typedef struct hot { uint64_t ref,parent,seen,sampled; double heat; uint32_t events,pad; } hot;
typedef struct cache_header {
 char magic[8]; uint32_t version,header_size,slots,names,text,hot_count,cluster,record;
 uint64_t serial,journal,cursor,generation; uint8_t digest[32];
} cache_header;
typedef struct name_link {uint32_t child_next,parent_next,parent_previous;uint32_t unresolved;} name_link;
typedef struct parent_link {uint32_t key,first;} parent_link;
typedef struct change {uint64_t ref,parent,ordinal;double priority;} change;
struct ntfs_cache {
 mft_model m;uint32_t *all_heads;name_link *edges;parent_link *parents;uint32_t head_slots,edge_capacity,parent_capacity,parent_count;int indexed;uint64_t unresolved,sequence_errors,dead_names,dead_text,index_builds,compactions,relinked_slots,relinked_names,metadata_updates,namespace_updates; cache_header h; hot activity[HOT_MAX]; uint32_t hot_count;
 change *queue;uint32_t count,capacity;uint32_t *dirty_ids;uint8_t *dirty;uint32_t dirty_count,dirty_slots;int mutated,save_needed,save_prepared;uint32_t prepared_slots,prepared_names,prepared_text;uint64_t fast_saves,saved_cursor;uint64_t own_refs[2];HANDLE owner,lock;wchar_t *path;ntfs_alive alive;void *user;ntfs_progress progress; NTFS_VOLUME_DATA_BUFFER geometry; HANDLE volume;
 uint64_t events,duplicates,refreshed,deleted,failed,sampled,journal_bytes,mft_bytes,cycles;
 double load_ms,baseline_ms,catchup_ms,aggregate_ms,save_ms; int rebuilt,partial; const char *reason;
};
typedef struct ntfs_cache cached;
static _Thread_local cached *active_cache;
static int available(cached *c){if(c->alive&&!c->alive(c->user)){SetLastError(ERROR_CANCELLED);return 0;}return 1;}
static int owner_begin(cached *c){return !c->owner||ImpersonateLoggedOnUser(c->owner);}
static void owner_end(cached *c){if(c->owner)RevertToSelf();}
static int mark_dirty(cached *c,uint32_t id){
 if(c->m.slots>c->dirty_slots){uint32_t n=c->m.slots;uint8_t *p=calloc(n,1);uint32_t *ids=malloc((size_t)n*4);if(!p||!ids){free(p);free(ids);return 0;}if(c->dirty_count)memcpy(ids,c->dirty_ids,c->dirty_count*4);if(c->dirty_slots)memcpy(p,c->dirty,c->dirty_slots);free(c->dirty);free(c->dirty_ids);c->dirty=p;c->dirty_ids=ids;c->dirty_slots=n;}
 c->save_needed=1;if(!c->dirty[id]){c->dirty[id]=1;c->dirty_ids[c->dirty_count++]=id;}return 1;
}
/* Runtime-only indexes. Persisted v2 names stay unchanged; checkpoints rebuild
 * these indexes after compaction, never during an ordinary small refresh. */
static void index_clear(cached *c){c->save_prepared=0;free(c->all_heads);free(c->edges);free(c->parents);c->all_heads=NULL;c->edges=NULL;c->parents=NULL;c->head_slots=c->edge_capacity=c->parent_capacity=c->parent_count=0;c->indexed=0;c->unresolved=c->sequence_errors=c->dead_names=c->dead_text=0;}
static uint32_t parent_hash(uint32_t id){uint32_t h=id*0x9e3779b9u;return h^(h>>16);}
static parent_link *parent_find(cached *c,uint32_t id,int create){
 uint32_t key=id+1;if(c->parent_capacity){uint32_t at=parent_hash(id)&(c->parent_capacity-1);while(c->parents[at].key&&c->parents[at].key!=key)at=(at+1)&(c->parent_capacity-1);if(c->parents[at].key)return c->parents+at;}if(!create)return NULL;
 if(!c->parent_capacity||c->parent_count>=c->parent_capacity/2){uint32_t cap=c->parent_capacity?c->parent_capacity*2:1024;parent_link *p=calloc(cap,sizeof *p);if(!p){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return NULL;}for(uint32_t i=0;i<c->parent_capacity;i++){if(!(i&4095)&&!available(c)){free(p);return NULL;}if(c->parents[i].key){parent_link x=c->parents[i];uint32_t at=parent_hash(x.key-1)&(cap-1);while(p[at].key)at=(at+1)&(cap-1);p[at]=x;}}free(c->parents);c->parents=p;c->parent_capacity=cap;}
 uint32_t at=parent_hash(id)&(c->parent_capacity-1);while(c->parents[at].key)at=(at+1)&(c->parent_capacity-1);c->parents[at]=(parent_link){key,MFT_NONE};c->parent_count++;return c->parents+at;
}
static int index_reserve(cached *c){
 if(c->head_slots<c->m.slots){uint32_t *p=realloc(c->all_heads,(size_t)c->m.slots*4);if(!p){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return 0;}memset(p+c->head_slots,255,(size_t)(c->m.slots-c->head_slots)*4);c->all_heads=p;c->head_slots=c->m.slots;}
 if(c->edge_capacity<c->m.name_capacity){name_link *p=realloc(c->edges,(size_t)c->m.name_capacity*sizeof *p);if(!p){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return 0;}c->edges=p;c->edge_capacity=c->m.name_capacity;}return 1;
}
static int bad_reference(const mft_model *m,const mft_name *n){uint64_t child=n->child&MFT_INDEX_MASK,parent=n->parent&MFT_INDEX_MASK;return child>=m->slots||parent>=m->slots||m->nodes[child].reference!=n->child||m->nodes[parent].reference!=n->parent||!(m->nodes[child].flags&MF_ACTIVE)||!(m->nodes[parent].flags&MF_DIRECTORY);}
static void index_insert(cached *c,uint32_t at,parent_link *parent){mft_name *n=c->m.names+at;uint32_t child=(uint32_t)(n->child&MFT_INDEX_MASK);name_link *edge=c->edges+at;*edge=(name_link){.child_next=c->all_heads[child],.parent_next=parent->first,.parent_previous=MFT_NONE};if(parent->first!=MFT_NONE)c->edges[parent->first].parent_previous=at;parent->first=at;c->all_heads[child]=at;}
static int index_build(cached *c){index_clear(c);if(!index_reserve(c))return 0;for(uint32_t i=0;i<c->m.name_count;i++){if(!(i&4095)&&!available(c))return 0;mft_name *n=c->m.names+i;if(!n->child){c->dead_names++;c->dead_text+=n->length+1;continue;}uint64_t child=n->child&MFT_INDEX_MASK,parent=n->parent&MFT_INDEX_MASK;if(child>=c->m.slots||parent>=MAX_SLOTS){SetLastError(ERROR_INVALID_DATA);return 0;}parent_link *p=parent_find(c,(uint32_t)parent,1);if(!p)return 0;index_insert(c,i,p);if(bad_reference(&c->m,n)){c->edges[i].unresolved=1;c->unresolved++;}}c->sequence_errors=c->m.bad_sequence>c->unresolved?c->m.bad_sequence-c->unresolved:0;c->m.bad_sequence=c->unresolved+c->sequence_errors;c->indexed=1;c->index_builds++;return 1;}
static int names_equal(const cached *c,uint32_t id,const mft_model *newer){
 const mft_model *m=&c->m;uint32_t count=0;for(uint32_t i=c->all_heads[id];i!=MFT_NONE;i=c->edges[i].child_next){const mft_name *a=m->names+i;if(a->child!=m->nodes[id].reference)return 0;uint32_t old_count=0,new_count=0;for(uint32_t j=c->all_heads[id];j!=MFT_NONE;j=c->edges[j].child_next){const mft_name *b=m->names+j;if(a->parent==b->parent&&a->length==b->length&&a->space==b->space&&!memcmp(m->text+a->offset,m->text+b->offset,a->length*2))old_count++;}for(uint32_t j=0;j<newer->name_count;j++){const mft_name *b=newer->names+j;if(a->parent==b->parent&&a->length==b->length&&a->space==b->space&&!memcmp(m->text+a->offset,newer->text+b->offset,a->length*2))new_count++;}if(old_count!=new_count)return 0;count++;}return count==newer->name_count;
}
static int metadata_equal(const mft_node *a,const mft_node *b){return a->reference==b->reference&&a->logical==b->logical&&a->allocated==b->allocated&&a->all_logical==b->all_logical&&a->all_allocated==b->all_allocated&&a->flags==b->flags&&a->attributes==b->attributes&&a->links==b->links&&a->streams==b->streams;}
static void index_retire(cached *c,uint32_t id){c->save_prepared=0;for(uint32_t i=c->all_heads[id];i!=MFT_NONE;){mft_name *n=c->m.names+i;name_link edge=c->edges[i];parent_link *p=parent_find(c,(uint32_t)(n->parent&MFT_INDEX_MASK),0);if(edge.parent_previous!=MFT_NONE)c->edges[edge.parent_previous].parent_next=edge.parent_next;else if(p)p->first=edge.parent_next;if(edge.parent_next!=MFT_NONE)c->edges[edge.parent_next].parent_previous=edge.parent_previous;if(edge.unresolved)c->unresolved--;c->dead_names++;c->dead_text+=n->length+1;n->child=0;n->valid=0;n->next=MFT_NONE;i=edge.child_next;}c->all_heads[id]=MFT_NONE;}
static int relink_dirty(cached *c){if(!c->mutated)return 1;c->save_prepared=0;for(uint32_t k=0;k<c->dirty_count;k++){if(!(k&255)&&!available(c))return 0;uint32_t id=c->dirty_ids[k];if(id>=c->m.slots)continue;mft_node *node=c->m.nodes+id;node->first=MFT_NONE;c->relinked_slots++;
  for(uint32_t i=c->all_heads[id];i!=MFT_NONE;i=c->edges[i].child_next){mft_name *n=c->m.names+i;name_link *edge=c->edges+i;if(edge->unresolved)c->unresolved--;edge->unresolved=0;n->valid=0;n->next=MFT_NONE;c->relinked_names++;if(bad_reference(&c->m,n)){edge->unresolved=1;c->unresolved++;continue;}int duplicate=0;for(uint32_t j=node->first;j!=MFT_NONE;j=c->m.names[j].next){mft_name *prior=c->m.names+j;if(prior->parent==n->parent&&prior->length==n->length&&!memcmp(c->m.text+prior->offset,c->m.text+n->offset,n->length*2)){duplicate=1;break;}}if(!duplicate){n->valid=1;n->next=node->first;node->first=i;}}
 }c->m.bad_sequence=c->unresolved+c->sequence_errors;c->mutated=0;return 1;
}
static int own_ref(const cached *c,uint64_t ref){return ref&&(ref==c->own_refs[0]||ref==c->own_refs[1]);}
static void capture_own(cached *c){BY_HANDLE_FILE_INFORMATION i;HANDLE h=CreateFileW(c->path,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);if(h!=INVALID_HANDLE_VALUE){if(GetFileInformationByHandle(h,&i))c->own_refs[0]=((uint64_t)i.nFileIndexHigh<<32)|i.nFileIndexLow;CloseHandle(h);}if(c->lock&&c->lock!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(c->lock,&i))c->own_refs[1]=((uint64_t)i.nFileIndexHigh<<32)|i.nFileIndexLow;}
static uint64_t now_seconds(void){FILETIME t;GetSystemTimeAsFileTime(&t);return (((uint64_t)t.dwHighDateTime<<32)|t.dwLowDateTime)/10000000;}
static int hash_begin(BCRYPT_ALG_HANDLE *a,BCRYPT_HASH_HANDLE *h){return BCryptOpenAlgorithmProvider(a,BCRYPT_SHA256_ALGORITHM,NULL,0)>=0&&BCryptCreateHash(*a,h,NULL,0,NULL,0,0)>=0;}
static int hash_add(BCRYPT_HASH_HANDLE h,const void *p,size_t n){while(n){if(active_cache&&!available(active_cache))return 0;ULONG k=(ULONG)(n>READ_CHUNK?READ_CHUNK:n);if(BCryptHashData(h,(PUCHAR)p,k,0)<0)return 0;p=(const uint8_t*)p+k;n-=k;}return 1;}
static int digest_model(cache_header h,const cached *c,uint8_t out[32]){
 BCRYPT_ALG_HANDLE a=NULL;BCRYPT_HASH_HANDLE b=NULL;memset(h.digest,0,32);int ok=hash_begin(&a,&b)&&hash_add(b,&h,sizeof h)&&hash_add(b,c->m.nodes,(size_t)h.slots*sizeof(mft_node))&&hash_add(b,c->m.names,(size_t)h.names*sizeof(mft_name))&&hash_add(b,c->m.text,(size_t)h.text*2)&&hash_add(b,c->activity,(size_t)h.hot_count*sizeof(hot))&&BCryptFinishHash(b,out,32,0)>=0;if(b)BCryptDestroyHash(b);if(a)BCryptCloseAlgorithmProvider(a,0);return ok;
}
/* A save may reuse preparation only while the fully resolved packed layout
 * remains unchanged. Namespace/reference/type changes and index resets revoke
 * this proof; scalar metadata, activity and journal-cursor updates preserve it. */
static void remember_save_layout(cached *c){c->save_prepared=c->indexed&&!c->mutated&&!c->m.pending_count&&!c->m.bad_sequence&&!c->unresolved&&!c->sequence_errors&&!c->dead_names&&!c->dead_text;c->prepared_slots=c->m.slots;c->prepared_names=c->m.name_count;c->prepared_text=c->m.text_count;}
/* Tombstones are removed before mft_resolve; pending extension DATA is one-shot. */
static int compact(cached *c){SetLastError(ERROR_SUCCESS);c->compactions++;index_clear(c);mft_model *m=&c->m;uint32_t n=0,t=0;for(uint32_t i=0;i<m->name_count;i++){mft_name a=m->names[i];if(!a.child)continue;uint64_t child=a.child&MFT_INDEX_MASK;if(child>=m->slots||((m->nodes[child].reserved&1)&&a.valid!=2))continue;if(a.offset>m->text_count||a.length>=m->text_count-a.offset){SetLastError(ERROR_INVALID_DATA);return 0;}memmove(m->text+t,m->text+a.offset,((size_t)a.length+1)*2);a.offset=t;t+=a.length+1;a.valid=0;a.next=MFT_NONE;m->names[n++]=a;}m->name_count=n;m->text_count=t;m->bad_sequence=0;for(uint32_t i=0;i<c->dirty_count;i++)if(c->dirty_ids[i]<m->slots)m->nodes[c->dirty_ids[i]].reserved=0;mft_resolve(m);m->pending_count=0;int ok=index_build(c);if(ok)remember_save_layout(c);return ok;}
static int checkpoint_ok(const cache_header *h,uint64_t serial,const USN_JOURNAL_DATA *j){return h->serial==serial&&h->journal==j->UsnJournalID&&h->cursor>=(uint64_t)j->FirstUsn&&h->cursor>=(uint64_t)j->LowestValidUsn&&h->cursor<=(uint64_t)j->NextUsn;}
static int query_journal(cached *c,USN_JOURNAL_DATA *j){DWORD n=0;return DeviceIoControl(c->volume,FSCTL_QUERY_USN_JOURNAL,NULL,0,j,sizeof *j,&n,NULL)&&n>=sizeof *j;}
/* Hash the exact persisted bytes while they are still hot from IO. No graph
 * field is trusted or changed until the entire checksum has matched. */
static int read_hashed(cached *c,FILE *f,BCRYPT_HASH_HANDLE hash,void *data,size_t size){uint8_t *p=data;while(size){if(!available(c))return 0;size_t n=size>READ_CHUNK?READ_CHUNK:size;if(fread(p,1,n,f)!=n||!hash_add(hash,p,n))return 0;p+=n;size-=n;}return 1;}
static int cache_load(cached *c,const wchar_t *path){uint64_t start=ticks();FILE *f=_wfopen(path,L"rb");if(!f){c->reason="cache-missing";return 0;}cache_header h={0};int ok=fread(&h,1,sizeof h,f)==sizeof h&&!memcmp(h.magic,"OPNTFC2",8)&&h.version==CACHE_VERSION&&h.header_size==sizeof h&&h.slots>5&&h.slots<=MAX_SLOTS&&h.names<=MAX_SLOTS*8ull&&h.text<=MAX_SLOTS*128ull&&h.hot_count<=HOT_MAX&&h.cluster==c->geometry.BytesPerCluster&&h.record==c->geometry.BytesPerFileRecordSegment;
 uint64_t expected=sizeof h+(uint64_t)h.slots*sizeof(mft_node)+(uint64_t)h.names*sizeof(mft_name)+(uint64_t)h.text*2+(uint64_t)h.hot_count*sizeof(hot);if(ok){_fseeki64(f,0,SEEK_END);ok=_ftelli64(f)==(int64_t)expected&&expected<UINT64_C(2147483648);_fseeki64(f,sizeof h,SEEK_SET);}
 mft_model *m=&c->m;int unavailable=0;BCRYPT_ALG_HANDLE algorithm=NULL;BCRYPT_HASH_HANDLE hash=NULL;
 if(ok){m->slots=h.slots;m->cluster_size=h.cluster;m->name_capacity=m->name_count=h.names;m->text_capacity=m->text_count=h.text;m->nodes=malloc((size_t)h.slots*sizeof *m->nodes);m->names=malloc((size_t)(h.names?h.names:1)*sizeof *m->names);m->text=malloc((size_t)(h.text?h.text:1)*2);ok=m->nodes&&m->names&&m->text;if(!ok)unavailable=1;}
 if(ok){cache_header hashed=h;memset(hashed.digest,0,sizeof hashed.digest);ok=hash_begin(&algorithm,&hash);if(!ok)unavailable=1;else ok=hash_add(hash,&hashed,sizeof hashed)&&read_hashed(c,f,hash,m->nodes,(size_t)h.slots*sizeof *m->nodes)&&read_hashed(c,f,hash,m->names,(size_t)h.names*sizeof *m->names)&&read_hashed(c,f,hash,m->text,(size_t)h.text*2)&&read_hashed(c,f,hash,c->activity,(size_t)h.hot_count*sizeof(hot));
  uint8_t digest[32];if(ok)ok=BCryptFinishHash(hash,digest,sizeof digest,0)>=0&&!memcmp(digest,h.digest,sizeof digest);
 }
 if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);fclose(f);
 /* Preserve every structural check, even for a correctly checksummed file. */
 for(uint32_t i=0;ok&&i<m->slots;i++){mft_node *n=&m->nodes[i];if(n->reserved||n->flags&~(MF_ACTIVE|MF_DIRECTORY|MF_DATA|MF_ATTRLIST|MF_BAD))ok=0;if((n->flags&MF_ACTIVE)&&((n->reference&MFT_INDEX_MASK)!=i||!n->reference||(n->first!=MFT_NONE&&n->first>=m->name_count)))ok=0;}
 uint32_t text_end=0;int packed=1;for(uint32_t i=0;ok&&i<m->name_count;i++){mft_name *n=&m->names[i];if(n->offset<text_end)ok=0;if(n->offset!=text_end||!n->child||(n->child&MFT_INDEX_MASK)>=m->slots)packed=0;text_end=n->offset+n->length+1;if(!n->length||n->length>255||n->offset>=m->text_count||n->length>=m->text_count-n->offset||m->text[n->offset+n->length]||n->space>3||n->space==2||(n->next!=MFT_NONE&&n->next>=m->name_count))ok=0;for(uint32_t k=0;ok&&k<n->length;k++)if(!m->text[n->offset+k]||m->text[n->offset+k]=='/'||m->text[n->offset+k]=='\\')ok=0;
  /* Persisted link fields are validated above, then discarded. Rebuild them
   * from full references rather than accepting the saved projection graph. */
  n->valid=0;n->next=MFT_NONE;
 }
 if(text_end!=m->text_count)packed=0;
 for(uint32_t i=0;ok&&i<h.hot_count;i++)if(c->activity[i].pad>1||!isfinite(c->activity[i].heat)||c->activity[i].heat<0||c->activity[i].heat>1e9)ok=0;
 if(!ok){mft_free(m);c->hot_count=0;c->reason=unavailable?"cache-index-unavailable":"cache-invalid";return 0;}
 c->h=h;c->saved_cursor=h.cursor;c->save_needed=0;c->hot_count=h.hot_count;
 /* Saves already pack names/text. Legacy gaps/tombstones still use the full
  * compactor; both paths retain the exact same resolver and index validator. */
 if(packed){m->bad_sequence=0;mft_resolve(m);m->pending_count=0;ok=index_build(c);}else ok=compact(c);
 if(!ok){DWORD e=GetLastError();mft_free(m);c->hot_count=0;c->reason=e==ERROR_NOT_ENOUGH_MEMORY?"cache-index-unavailable":"cache-invalid";return 0;}remember_save_layout(c);c->load_ms=ms(ticks()-start);return 1;
}
static int prepare_save(cached *c){if(!available(c))return 0;
 if(c->save_prepared&&c->indexed&&!c->mutated&&!c->m.pending_count&&!c->m.bad_sequence&&!c->unresolved&&!c->sequence_errors&&!c->dead_names&&!c->dead_text&&c->m.slots==c->prepared_slots&&c->m.name_count==c->prepared_names&&c->m.text_count==c->prepared_text&&c->head_slots>=c->m.slots&&c->edge_capacity>=c->m.name_capacity){c->fast_saves++;return 1;}
 return compact(c);
}
static int cache_save(cached *c,const wchar_t *path,int interrupt){uint64_t start=ticks();if(!prepare_save(c))return 0;cache_header h=c->h;memcpy(h.magic,"OPNTFC2",8);h.version=CACHE_VERSION;h.header_size=sizeof h;h.slots=c->m.slots;h.names=c->m.name_count;h.text=c->m.text_count;h.hot_count=c->hot_count;h.cluster=c->m.cluster_size;h.record=c->geometry.BytesPerFileRecordSegment;h.generation++;if(!digest_model(h,c,h.digest))return 0;
 wchar_t temp[32768];if(swprintf(temp,32768,L"%ls.pending-%lu-%llu",path,GetCurrentProcessId(),(unsigned long long)GetTickCount64())<0)return 0;HANDLE f=CreateFileW(temp,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);if(f==INVALID_HANDLE_VALUE)return 0;const void *parts[]={&h,c->m.nodes,c->m.names,c->m.text,c->activity};size_t sizes[]={sizeof h,(size_t)h.slots*sizeof(mft_node),(size_t)h.names*sizeof(mft_name),(size_t)h.text*2,(size_t)h.hot_count*sizeof(hot)};int ok=1;
 for(int i=0;i<5&&ok;i++){const uint8_t *p=parts[i];size_t left=sizes[i];while(left){if(!available(c)){ok=0;break;}DWORD n=(DWORD)(left>READ_CHUNK?READ_CHUNK:left),done=0;if(!WriteFile(f,p,n,&done,NULL)||done!=n){ok=0;break;}p+=n;left-=n;}if(interrupt&&i==0){CloseHandle(f);c->save_ms=ms(ticks()-start);return 0;}}
 if(ok)ok=FlushFileBuffers(f);CloseHandle(f);if(ok)ok=MoveFileExW(temp,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);/* Failed candidates are retained for review. */if(ok)c->h=h;c->save_ms=ms(ticks()-start);return ok;
}
static int baseline(cached *c){uint64_t start=ticks();USN_JOURNAL_DATA j;if(!query_journal(c,&j))return 0;index_clear(c);mft_free(&c->m);c->hot_count=0;c->h=(cache_header){.serial=(uint64_t)c->geometry.VolumeSerialNumber.QuadPart,.journal=j.UsnJournalID,.cursor=(uint64_t)j.NextUsn};mft_run runs[4096];uint32_t count=0;if(!bootstrap(c->volume,&c->geometry,runs,&count))return 0;uint64_t bytes=(uint64_t)c->geometry.MftValidDataLength.QuadPart,slots=bytes/c->geometry.BytesPerFileRecordSegment;if(slots<=5||slots>MAX_SLOTS)return 0;
 mft_model *m=&c->m;m->slots=(uint32_t)slots;m->cluster_size=c->geometry.BytesPerCluster;m->nodes=calloc(m->slots,sizeof *m->nodes);DWORD chunk=BASELINE_READ_CHUNK;uint8_t *b=malloc(chunk);if(!b){chunk=READ_CHUNK;b=malloc(chunk);}if(!m->nodes||!b){free(b);return 0;}uint64_t scanned=0,deadline=GetTickCount64()+180000;int ok=1;
 for(uint32_t r=0;r<count&&scanned<bytes&&ok;r++){uint64_t left=runs[r].clusters*m->cluster_size;if(left>bytes-scanned)left=bytes-scanned;uint64_t offset=runs[r].lcn*m->cluster_size;while(left&&ok){if(!available(c)){ok=0;break;}DWORD n=(DWORD)(left>chunk?chunk:left);ok=read_at(c->volume,offset,b,n);if(!ok)break;c->mft_bytes+=n;for(uint32_t at=0;at<n;at+=c->geometry.BytesPerFileRecordSegment){uint8_t *p=b+at;if(memcmp(p,"FILE",4))continue;uint32_t id=(uint32_t)((scanned+at)/c->geometry.BytesPerFileRecordSegment);if(!mft_fixup(p,c->geometry.BytesPerFileRecordSegment,c->geometry.BytesPerSector)||!mft_record(m,p,c->geometry.BytesPerFileRecordSegment,id)){fprintf(stderr,"baseline raw-record invalid id=%u\n",id);ok=0;break;}}left-=n;scanned+=n;offset+=n;if(GetTickCount64()>deadline)ok=0;}}
 free(b);if(ok)ok=compact(c)&&!m->bad_attributes&&!m->bad_sequence;if(!ok)fprintf(stderr,"baseline incomplete records=%llu attrs=%llu seq=%llu\n",(unsigned long long)m->records,(unsigned long long)m->bad_attributes,(unsigned long long)m->bad_sequence);c->baseline_ms+=ms(ticks()-start);c->rebuilt=1;c->save_needed=1;return ok;
}
static int get_record(cached *c,uint64_t id,uint8_t *record,uint64_t *reference){uint32_t size=c->geometry.BytesPerFileRecordSegment;NTFS_FILE_RECORD_INPUT_BUFFER in;in.FileReferenceNumber.QuadPart=(LONGLONG)id;uint8_t out[65536+32];DWORD got=0;
 if(!DeviceIoControl(c->volume,FSCTL_GET_NTFS_FILE_RECORD,&in,sizeof in,out,sizeof out,&got,NULL))return -1;
 if(got<12||mft_u32(out+8)!=size||got<12+size){SetLastError(10001);return -1;}uint64_t actual=mft_u64(out)&MFT_INDEX_MASK;if(actual!=id)return 0;memcpy(record,out+12,size);if(memcmp(record,"FILE",4)||!(mft_u16(record+22)&1)){SetLastError(10002);return -1;}
 /* FSCTL exposes the kernel's decoded record, not raw sectors. Its retained
    USA array can be stale after in-memory changes; do not apply raw-disk fixups.
    mft_record validates the decoded attribute layout before model mutation. */
 *reference=((uint64_t)mft_u16(record+16)<<48)|id;return 1;
}
/* Read only bounded ATTRIBUTE_LIST metadata, never arbitrary file payload. */
static int list_refs(cached *c,const uint8_t *record,uint64_t *refs,uint32_t *count){uint32_t size=c->geometry.BytesPerFileRecordSegment,used=mft_u32(record+24),at=mft_u16(record+20);if(used>size||at<48||at>used)return 0;while(at+4<=used){const uint8_t *a=record+at;uint32_t type=mft_u32(a);if(type==UINT32_MAX)return 1;if(used-at<24)return 0;uint32_t len=mft_u32(a+4);if(len<24||len%8||len>used-at)return 0;if(type==0x20){const uint8_t *data=NULL;uint8_t *owned=NULL;uint64_t length=0;if(!a[8]){uint32_t off=mft_u16(a+20);length=mft_u32(a+16);if(off<24||off>len||length>len-off)return 0;data=a+off;}else{if(len<64||mft_u64(a+16)||mft_u16(a+12))return 0;length=mft_u64(a+48);if(!length||length>LIST_MAX)return 0;mft_run runs[4096];uint32_t nr=0;if(!mft_runs(a,len,runs,&nr,4096,(uint64_t)c->geometry.TotalClusters.QuadPart)){fprintf(stderr,"list runlist invalid\n");return 0;}owned=malloc((size_t)length+c->geometry.BytesPerSector);if(!owned)return 0;uint64_t done=0;int ok=1;for(uint32_t k=0;k<nr&&done<length;k++){if(runs[k].vcn*c->m.cluster_size!=done){ok=0;break;}uint64_t n=runs[k].clusters*c->m.cluster_size;if(n>length-done)n=length-done;if(!read_at(c->volume,runs[k].lcn*c->m.cluster_size,owned+done,(DWORD)((n+c->geometry.BytesPerSector-1)/c->geometry.BytesPerSector*c->geometry.BytesPerSector))){ok=0;break;}done+=n;}if(!ok||done!=length){free(owned);return 0;}data=owned;}
 uint64_t pos=0;int ok=1;while(pos<length){if(length-pos<26){fprintf(stderr,"list short tail pos=%llu len=%llu\n",(unsigned long long)pos,(unsigned long long)length);ok=0;break;}const uint8_t *e=data+pos;uint16_t n=mft_u16(e+4);if(n<26||n>length-pos){fprintf(stderr,"list bad entry pos=%llu len=%llu type=%u entry=%u\n",(unsigned long long)pos,(unsigned long long)length,mft_u32(e),n);ok=0;break;}uint64_t ref=mft_u64(e+16);int exists=0;for(uint32_t k=0;k<*count;k++)exists|=refs[k]==ref;if(!exists){if(*count>=256){ok=0;break;}refs[(*count)++]=ref;}pos+=n;}free(owned);if(!ok)return 0;}at+=len;}return 0;}
static void set64(uint8_t *p,uint64_t x){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(x>>(i*8));}
static int ensure_slots(cached *c,uint64_t id){if(id>=MAX_SLOTS)return 0;if(id<c->m.slots)return 1;uint32_t n=(uint32_t)id+1024;if(n>MAX_SLOTS)n=MAX_SLOTS;mft_node *p=realloc(c->m.nodes,(size_t)n*sizeof *p);if(!p)return 0;memset(p+c->m.slots,0,(size_t)(n-c->m.slots)*sizeof *p);for(uint32_t i=c->m.slots;i<n;i++)p[i].first=MFT_NONE;c->m.nodes=p;c->m.slots=n;c->save_prepared=0;return 1;}
static int reserve_names(mft_model *m,uint32_t n,uint32_t t){if(n>UINT32_MAX-m->name_count||t>UINT32_MAX-m->text_count)return 0;uint32_t names=m->name_count+n,text=m->text_count+t;if(names>m->name_capacity){uint32_t extra=m->name_capacity/8;if(extra<1024)extra=1024;uint32_t cap=names<=UINT32_MAX-extra?names+extra:names;mft_name *p=realloc(m->names,(size_t)cap*sizeof *p);if(!p)return 0;m->names=p;m->name_capacity=cap;}if(text>m->text_capacity){uint32_t extra=m->text_capacity/8;if(extra<4096)extra=4096;uint32_t cap=text<=UINT32_MAX-extra?text+extra:text;uint16_t *p=realloc(m->text,(size_t)cap*2);if(!p)return 0;m->text=p;m->text_capacity=cap;}return 1;}
/* Parsing/metadata reads finish before this function. Reserve all fallible
 * storage first, then replace only this identity and its incoming dependencies. */
static int apply_record(cached *c,uint32_t id,int got,uint64_t actual,const mft_model *temp){
 if(!c->indexed&&!index_build(c))return 0;if(!index_reserve(c))return 0;mft_node prior=c->m.nodes[id],next=got?temp->nodes[0]:(mft_node){0};next.reference=got?actual:0;next.first=MFT_NONE;next.reserved=0;
 if(!got&&!(prior.flags&MF_ACTIVE)&&c->all_heads[id]==MFT_NONE)return 1;int keep_names=got&&prior.reference==actual&&names_equal(c,id,temp);
 if(keep_names&&metadata_equal(&prior,&next)){c->refreshed++;return 1;}
 if(!keep_names){if(!reserve_names(&c->m,temp->name_count,temp->text_count)||!index_reserve(c))return 0;for(uint32_t i=0;i<temp->name_count;i++){uint64_t parent=temp->names[i].parent&MFT_INDEX_MASK;if(parent>=MAX_SLOTS||!parent_find(c,(uint32_t)parent,1))return 0;}}
 if(!mark_dirty(c,id))return 0;int dependencies=prior.reference!=next.reference||((prior.flags^next.flags)&(MF_ACTIVE|MF_DIRECTORY))||((prior.attributes^next.attributes)&FILE_ATTRIBUTE_REPARSE_POINT);
 if(!keep_names||dependencies)c->save_prepared=0;
 if(dependencies){parent_link *parent=parent_find(c,id,0);for(uint32_t i=parent?parent->first:MFT_NONE;i!=MFT_NONE;i=c->edges[i].parent_next)if(!mark_dirty(c,(uint32_t)(c->m.names[i].child&MFT_INDEX_MASK)))return 0;}
 if(keep_names){next.first=prior.first;c->metadata_updates++;}
 else{index_retire(c,id);c->namespace_updates++;uint32_t text_offset=c->m.text_count;for(uint32_t i=0;i<temp->name_count;i++){mft_name n=temp->names[i];n.child=actual;n.offset+=text_offset;n.next=MFT_NONE;n.valid=0;uint32_t at=c->m.name_count++;c->m.names[at]=n;index_insert(c,at,parent_find(c,(uint32_t)(n.parent&MFT_INDEX_MASK),0));}if(temp->text_count)memcpy(c->m.text+text_offset,temp->text,(size_t)temp->text_count*2);c->m.text_count+=temp->text_count;c->mutated=1;}
 c->m.nodes[id]=next;if(dependencies)c->mutated=1;if(!got)c->deleted++;c->refreshed++;return 1;
}
static int finish_refresh(cached *c){if(!relink_dirty(c))return 0;/* Reclaim only substantial churn; ordinary changes never scan the arena. */if(c->dead_names>=65536&&(c->dead_names*4>=c->m.name_count||c->dead_text*4>=c->m.text_count))return compact(c);return 1;}
static int refresh(cached *c,uint64_t requested){uint64_t id=requested&MFT_INDEX_MASK;if(!ensure_slots(c,id))return 0;uint8_t record[65536];uint64_t actual=0;int got=get_record(c,id,record,&actual);if(got<0){fprintf(stderr,"refresh get-record id=%llu error=%lu\n",(unsigned long long)id,GetLastError());return 0;}mft_model temp={.slots=1,.cluster_size=c->m.cluster_size};temp.nodes=calloc(1,sizeof(mft_node));if(!temp.nodes)return 0;int ok=1;
 if(got&&mft_u64(record+32))got=0;
 if(got){/* A reused slot is refreshed to its current sequence, never stale event data. */uint64_t refs[256]={actual};uint32_t count=1;ok=list_refs(c,record,refs,&count);if(!ok)fprintf(stderr,"refresh attribute-list id=%llu\n",(unsigned long long)id);if(ok){ok=mft_record(&temp,record,c->geometry.BytesPerFileRecordSegment,0);if(!ok)fprintf(stderr,"refresh parse-base id=%llu\n",(unsigned long long)id);}for(uint32_t k=1;ok&&k<count;k++){uint64_t ref;int r=get_record(c,refs[k]&MFT_INDEX_MASK,record,&ref);if(r!=1||ref!=refs[k]||mft_u64(record+32)!=actual){fprintf(stderr,"refresh extension-ref id=%llu target=%llu returned=%llu status=%d\n",(unsigned long long)id,(unsigned long long)refs[k],(unsigned long long)ref,r);ok=0;break;}set64(record+32,actual&~MFT_INDEX_MASK);ok=list_refs(c,record,refs,&count);if(!ok)fprintf(stderr,"refresh attribute-list id=%llu\n",(unsigned long long)id);if(ok){ok=mft_record(&temp,record,c->geometry.BytesPerFileRecordSegment,0);if(!ok)fprintf(stderr,"refresh parse-base id=%llu\n",(unsigned long long)id);}}
 if(ok){uint32_t names=temp.name_count;temp.name_count=0;mft_resolve(&temp);temp.name_count=names;ok=!temp.bad_attributes&&!temp.bad_sequence;if(!ok)fprintf(stderr,"refresh resolve id=%llu bad=%llu sequence=%llu\n",(unsigned long long)id,(unsigned long long)temp.bad_attributes,(unsigned long long)temp.bad_sequence);temp.nodes[0].reference=actual;
 /* Current default EOF for open writers, retaining MFT physical resident semantics. */
 FILE_ID_DESCRIPTOR d={.dwSize=sizeof d,.Type=FileIdType};d.FileId.QuadPart=(LONGLONG)actual;HANDLE h=OpenFileById(c->volume,&d,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_OPEN_NO_RECALL);if(h!=INVALID_HANDLE_VALUE){FILE_STANDARD_INFO s;if(!(temp.nodes[0].flags&MF_DIRECTORY)&&GetFileInformationByHandleEx(h,FileStandardInfo,&s,sizeof s)&&s.EndOfFile.QuadPart>=0){mft_node *n=&temp.nodes[0];n->all_logical=n->all_logical-n->logical+(uint64_t)s.EndOfFile.QuadPart;n->logical=(uint64_t)s.EndOfFile.QuadPart;/* Physical allocation stays from the same parsed metadata, not rounded handle allocation. */}CloseHandle(h);}
 }}
 if(ok)ok=apply_record(c,(uint32_t)id,got,actual,&temp);mft_free(&temp);return ok;
}
static double heat_at(const hot *h,uint64_t now){uint64_t age=now>h->seen?now-h->seen:0;return h->heat*exp2(-(double)age/60.0);}
static hot *learn(cached *c,uint64_t ref,uint64_t parent,uint64_t now){uint32_t at=c->hot_count;for(uint32_t i=0;i<c->hot_count;i++)if(c->activity[i].ref==ref){at=i;break;}if(at==c->hot_count){if(at<HOT_MAX)c->hot_count++;else{at=0;for(uint32_t i=1;i<HOT_MAX;i++)if(heat_at(&c->activity[i],now)<heat_at(&c->activity[at],now))at=i;}c->activity[at]=(hot){.ref=ref,.parent=parent};}hot *h=&c->activity[at];h->heat=fmin(10000,heat_at(h,now)+1);h->seen=now;h->parent=parent;h->events++;return h;}
static double folder_heat(cached *c,uint64_t parent,uint64_t now){double sum=0;for(uint32_t i=0;i<c->hot_count;i++)if(c->activity[i].parent==parent)sum+=heat_at(&c->activity[i],now);return fmin(sum,10000);}
static int enqueue(cached *c,uint64_t ref,uint64_t parent,uint64_t ordinal){if(c->count==CHANGE_MAX)return 0;if(c->count==c->capacity){uint32_t cap=c->capacity?c->capacity*2:4096;change *p=realloc(c->queue,(size_t)cap*sizeof *p);if(!p)return 0;c->queue=p;c->capacity=cap;}c->queue[c->count++]=(change){ref,parent,ordinal,0};return 1;}
static int by_ref(const void *a,const void *b){const change *x=a,*y=b;uint64_t i=x->ref&MFT_INDEX_MASK,j=y->ref&MFT_INDEX_MASK;return i<j?-1:i>j?1:x->ordinal<y->ordinal?-1:x->ordinal>y->ordinal;}
static int by_priority(const void *a,const void *b){const change *x=a,*y=b;return x->priority>y->priority?-1:x->priority<y->priority?1:x->ordinal<y->ordinal?-1:x->ordinal>y->ordinal;}
static int journal_chunk(cached *c,uint8_t *b,uint32_t n,uint64_t at,uint64_t end,uint64_t *continuation){if(n<8)return 0;int ok=1;uint64_t next=mft_u64(b);*continuation=next;if(next<=at)return 0;uint32_t pos=8;uint64_t last=at;while(pos<n){if(n-pos<60){ok=0;break;}uint8_t *r=b+pos;uint32_t length=mft_u32(r);uint16_t major=mft_u16(r+4),name_len=mft_u16(r+56),name_off=mft_u16(r+58);uint64_t usn=mft_u64(r+24);if(length<60||length%8||length>n-pos||major!=2||name_off<60||name_off>length||name_len%2||name_len>length-name_off||usn<last||usn>=next){ok=0;break;}last=usn;if(usn>=end)break;uint64_t ref=mft_u64(r+8),parent=mft_u64(r+16);if((ref&MFT_INDEX_MASK)>=MAX_SLOTS||!enqueue(c,ref,parent,c->events)){ok=0;break;}c->events++;pos+=length;}return ok;}
static int collect(cached *c,uint64_t *end){USN_JOURNAL_DATA j;if(!query_journal(c,&j)||!checkpoint_ok(&c->h,(uint64_t)c->geometry.VolumeSerialNumber.QuadPart,&j)){c->reason="journal-gap-or-identity-change";return -1;}*end=(uint64_t)j.NextUsn;uint64_t at=c->h.cursor;uint8_t *b=malloc(READ_CHUNK);if(!b)return 0;int ok=1;uint64_t deadline=GetTickCount64()+60000;while(at<*end&&ok){if(!available(c)){ok=0;break;}READ_USN_JOURNAL_DATA in={0};in.StartUsn=(USN)at;in.ReasonMask=0xffffffff;in.UsnJournalID=j.UsnJournalID;DWORD n=0;if(!DeviceIoControl(c->volume,FSCTL_READ_USN_JOURNAL,&in,sizeof in,b,READ_CHUNK,&n,NULL)||n<8){if(GetLastError()==ERROR_JOURNAL_ENTRY_DELETED){free(b);c->reason="journal-gap-during-read";return -1;}ok=0;break;}c->journal_bytes+=n;uint64_t next=0;ok=journal_chunk(c,b,n,at,*end,&next);at=next;if(GetTickCount64()>deadline)ok=0;}free(b);return ok;}
static int catchup(cached *c){uint64_t start=ticks(),end=0;c->count=0;int status=collect(c,&end);if(status!=1)return status;qsort(c->queue,c->count,sizeof(change),by_ref);uint32_t n=0;uint64_t now=now_seconds();for(uint32_t i=0;i<c->count;i++){change q=c->queue[i];if(n&&((c->queue[n-1].ref^q.ref)&MFT_INDEX_MASK)==0){c->duplicates++;c->queue[n-1]=q;}else c->queue[n++]=q;}c->count=n;for(uint32_t i=0;i<n;i++){change *q=c->queue+i;if(own_ref(c,q->ref))q->priority=0;else{hot *h=learn(c,q->ref,q->parent,now);q->priority=heat_at(h,now)+folder_heat(c,q->parent,now);}}qsort(c->queue,n,sizeof(change),by_priority);
 int ok=1;/* Finite fence: every queued identity is attempted, including cold folders. */for(uint32_t i=0;i<n;i++){if(!available(c)){ok=0;break;}if(!refresh(c,c->queue[i].ref)){c->failed++;ok=0;}}
 if(!finish_refresh(c))ok=0;if(c->m.bad_sequence)ok=0;if(ok)c->h.cursor=end;else c->partial=1;c->cycles++;c->catchup_ms+=ms(ticks()-start);return ok;}
static int sample_hot(cached *c){uint32_t keep=0;for(uint32_t i=0;i<c->hot_count;i++){uint64_t id=c->activity[i].ref&MFT_INDEX_MASK;if(!own_ref(c,c->activity[i].ref)&&id<c->m.slots&&(c->m.nodes[id].flags&MF_ACTIVE)&&c->m.nodes[id].reference==c->activity[i].ref)c->activity[keep++]=c->activity[i];}c->hot_count=keep;uint64_t now=now_seconds();uint32_t sampled=0;uint8_t attempted[HOT_MAX]={0};int ok=1;/* Oldest due sample first, so a very hot folder cannot starve other retained candidates. */for(uint32_t k=0;k<128;k++){if(!available(c)){ok=0;break;}uint32_t best=HOT_MAX;uint64_t oldest=UINT64_MAX;for(uint32_t i=0;i<c->hot_count;i++){hot *h=c->activity+i;double heat=heat_at(h,now);uint64_t age=now>h->seen?now-h->seen:0;uint64_t interval=age>86400?300:age>120?30:heat>=4?1:heat>=0.5?2:5;if(!attempted[i]&&(h->pad||(now<h->sampled||now-h->sampled>=interval))&&h->sampled<oldest){best=i;oldest=h->sampled;}}if(best==HOT_MAX)break;hot *h=c->activity+best;attempted[best]=1;h->sampled=now;uint64_t id=h->ref&MFT_INDEX_MASK,old_size=c->m.nodes[id].logical,old_alloc=c->m.nodes[id].all_allocated;if(!refresh(c,h->ref)){c->failed++;h->pad=1;ok=0;}else {h->pad=0;if(c->m.nodes[id].logical!=old_size||c->m.nodes[id].all_allocated!=old_alloc){h->heat=fmin(10000,heat_at(h,now)+1);h->seen=now;}}sampled++;}for(uint32_t i=0;i<c->hot_count;i++)if(c->activity[i].pad)ok=0;if(!finish_refresh(c))ok=0;c->sampled+=sampled;if(!ok)c->partial=1;return ok;}

static void progress(cached *c,unsigned phase){if(c->progress)c->progress(c->user,phase);}
static DWORD rebuild(cached *c){progress(c,NTFS_BASELINE);if(!baseline(c))return available(c)?ERROR_INVALID_DATA:ERROR_CANCELLED;return 0;}
DWORD ntfs_cache_open(HANDLE volume,HANDLE owner,const wchar_t *path,ntfs_alive alive,ntfs_progress notify,void *user,ntfs_cache **out){
 *out=NULL;LARGE_INTEGER f;QueryPerformanceFrequency(&f);frequency=(double)f.QuadPart;
 cached *c=calloc(1,sizeof *c);if(!c)return ERROR_NOT_ENOUGH_MEMORY;c->volume=volume;c->owner=owner;c->alive=alive;c->progress=notify;c->user=user;c->path=_wcsdup(path);c->lock=INVALID_HANDLE_VALUE;c->reason="cache-valid";active_cache=c;
 DWORD error=0,n;USN_JOURNAL_DATA journal;
 if(!c->path){error=ERROR_NOT_ENOUGH_MEMORY;goto fail;}
 if(!DeviceIoControl(volume,FSCTL_GET_NTFS_VOLUME_DATA,NULL,0,&c->geometry,sizeof c->geometry,&n,NULL)||n<sizeof c->geometry||!query_journal(c,&journal)){error=GetLastError();if(!error)error=ERROR_NOT_SUPPORTED;goto fail;}
 if(!owner_begin(c)){error=GetLastError();goto fail;}wchar_t lock_path[32768];int valid=swprintf(lock_path,32768,L"%ls.lock",path)>0;c->lock=valid?CreateFileW(lock_path,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL):INVALID_HANDLE_VALUE;error=c->lock==INVALID_HANDLE_VALUE?GetLastError():0;owner_end(c);if(error)goto fail;
 progress(c,NTFS_LOADING);if(!owner_begin(c)){error=GetLastError();goto fail;}int loaded=cache_load(c,path);capture_own(c);owner_end(c);if(!available(c)){error=ERROR_CANCELLED;goto fail;}if(!loaded&&!strcmp(c->reason,"cache-index-unavailable")){error=ERROR_NOT_ENOUGH_MEMORY;goto fail;}
 if(loaded&&!checkpoint_ok(&c->h,(uint64_t)c->geometry.VolumeSerialNumber.QuadPart,&journal)){loaded=0;c->reason="journal-gap-or-identity-change";}
 if(!loaded&&!strcmp(c->reason,"cache-invalid")){wchar_t rejected[32768];if(!owner_begin(c)){error=GetLastError();goto fail;}if(swprintf(rejected,32768,L"%ls.rejected-%lu-%llu",path,GetCurrentProcessId(),(unsigned long long)GetTickCount64())<0||!MoveFileExW(path,rejected,0))error=GetLastError();owner_end(c);if(error)goto fail;}
 if(!loaded){error=rebuild(c);if(error)goto fail;}else progress(c,NTFS_CACHED);
 *out=c;return 0;
fail:ntfs_cache_close(c);return error;
}
DWORD ntfs_cache_poll(ntfs_cache *c){
 active_cache=c;c->partial=0;if(!available(c))return ERROR_CANCELLED;progress(c,NTFS_CATCHUP);uint64_t before=c->h.cursor;int state=catchup(c);
 if(state<0){DWORD e=rebuild(c);if(e)return e;before=c->h.cursor;state=catchup(c);}
 if(state!=1)c->partial=1;if(!sample_hot(c))c->partial=1;if(c->m.bad_sequence){c->partial=1;c->h.cursor=before;}
 if(!available(c))return ERROR_CANCELLED;progress(c,c->partial?NTFS_INCOMPLETE:NTFS_LIVE);return c->partial?ERROR_RETRY:0;
}
DWORD ntfs_cache_save(ntfs_cache *c){active_cache=c;if(!available(c))return ERROR_CANCELLED;if(!c->save_needed&&c->h.cursor==c->saved_cursor)return 0;if(!owner_begin(c))return GetLastError();int ok=cache_save(c,c->path,0);if(ok){capture_own(c);c->saved_cursor=c->h.cursor;c->save_needed=0;}DWORD error=ok?0:GetLastError();owner_end(c);return ok?0:error?error:ERROR_WRITE_FAULT;}
void ntfs_cache_close(ntfs_cache *c){if(!c)return;if(c->lock!=INVALID_HANDLE_VALUE&&c->lock)CloseHandle(c->lock);index_clear(c);mft_free(&c->m);free(c->queue);free(c->dirty_ids);free(c->dirty);free(c->path);if(active_cache==c)active_cache=NULL;free(c);}
const mft_model *ntfs_cache_model(const ntfs_cache *c){return &c->m;}
const uint32_t *ntfs_cache_changes(ntfs_cache *c,uint32_t *count,int *reset){*count=c->dirty_count;*reset=c->rebuilt;return c->dirty_ids;}
void ntfs_cache_ack_changes(ntfs_cache *c){for(uint32_t i=0;i<c->dirty_count;i++)c->dirty[c->dirty_ids[i]]=0;c->dirty_count=0;c->rebuilt=0;}
uint64_t ntfs_cache_generation(const ntfs_cache *c){return c->h.generation;}
