/* Private NTFS projection storage. IDs/pointers live until the view is reset;
 * names can be returned independently when links are renamed or retired. */
#ifndef FP_NTFS_POOL_H
#define FP_NTFS_POOL_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#ifndef NTFS_POOL_MALLOC
#define NTFS_POOL_MALLOC malloc
#endif
#ifndef NTFS_POOL_FREE
#define NTFS_POOL_FREE free
#endif
#define NTFS_POOL_LINKS 4096
#define NTFS_POOL_NAME_BYTES 65536
#define NTFS_POOL_BINS 16

typedef struct ntfs_pool_link {
 uint64_t ref;
 uint32_t id,slot,parent,first,last,next,previous,epoch;
 struct ntfs_pool_link *slot_next;
 wchar_t *name;
 int gone;
} ntfs_pool_link;
typedef struct ntfs_link_block {
 struct ntfs_link_block *next;
 uint32_t used;
 ntfs_pool_link items[NTFS_POOL_LINKS];
} ntfs_link_block;
typedef struct ntfs_name_block ntfs_name_block;
typedef struct ntfs_name_cell {ntfs_name_block *owner;wchar_t text[];} ntfs_name_cell;
struct ntfs_name_block {
 ntfs_name_block *next,*previous,*available_next,*available_previous;
 ntfs_name_cell *free;
 uint32_t live,bin;
 uintptr_t data[];
};
typedef struct ntfs_pool {
 ntfs_link_block *links;
 ntfs_name_block *names,*available[NTFS_POOL_BINS];
 size_t bytes;
 uint32_t link_blocks,name_blocks,live_names;
} ntfs_pool;
static const uint16_t ntfs_name_sizes[NTFS_POOL_BINS]={16,32,48,64,80,96,112,128,160,192,224,256,320,384,448,512};

static ntfs_pool_link *ntfs_pool_link_new(ntfs_pool *p){
 ntfs_link_block *b=p->links;
 if(!b||b->used==NTFS_POOL_LINKS){
  b=NTFS_POOL_MALLOC(sizeof *b);if(!b)return NULL;
  b->next=p->links;b->used=0;p->links=b;p->link_blocks++;p->bytes+=sizeof *b;
 }
 ntfs_pool_link *link=b->items+b->used++;memset(link,0,sizeof *link);return link;
}
static void ntfs_pool_available_remove(ntfs_pool *p,ntfs_name_block *b){
 if(b->available_previous)b->available_previous->available_next=b->available_next;
 else p->available[b->bin]=b->available_next;
 if(b->available_next)b->available_next->available_previous=b->available_previous;
 b->available_next=b->available_previous=NULL;
}
static void ntfs_pool_available_add(ntfs_pool *p,ntfs_name_block *b){
 b->available_previous=NULL;b->available_next=p->available[b->bin];
 if(b->available_next)b->available_next->available_previous=b;
 p->available[b->bin]=b;
}
static wchar_t *ntfs_pool_name_new(ntfs_pool *p,const wchar_t *name){
 size_t length=wcslen(name);if(length>255)return NULL;
 size_t bytes=(length+1)*sizeof(wchar_t);unsigned bin=0;
 while(bin<NTFS_POOL_BINS&&ntfs_name_sizes[bin]<bytes)bin++;
 if(bin==NTFS_POOL_BINS)return NULL;
 ntfs_name_block *b=p->available[bin];
 if(!b){
  b=NTFS_POOL_MALLOC(NTFS_POOL_NAME_BYTES);if(!b)return NULL;
  memset(b,0,sizeof *b);b->bin=bin;b->next=p->names;
  if(b->next)b->next->previous=b;p->names=b;
  size_t stride=sizeof(ntfs_name_cell)+ntfs_name_sizes[bin];
  size_t count=(NTFS_POOL_NAME_BYTES-sizeof *b)/stride;
  for(size_t i=0;i<count;i++){
   ntfs_name_cell *c=(ntfs_name_cell*)((uint8_t*)b->data+i*stride);
   c->owner=b;memcpy(c->text,&b->free,sizeof b->free);b->free=c;
  }
  ntfs_pool_available_add(p,b);p->name_blocks++;p->bytes+=NTFS_POOL_NAME_BYTES;
 }
 ntfs_name_cell *c=b->free;memcpy(&b->free,c->text,sizeof b->free);
 if(!b->free)ntfs_pool_available_remove(p,b);
 b->live++;p->live_names++;memcpy(c->text,name,bytes);return c->text;
}
static void ntfs_pool_name_free(ntfs_pool *p,wchar_t *name){
 if(!name)return;
 ntfs_name_cell *c=(ntfs_name_cell*)((uint8_t*)name-offsetof(ntfs_name_cell,text));
 ntfs_name_block *b=c->owner;int was_full=!b->free;
 memcpy(c->text,&b->free,sizeof b->free);b->free=c;b->live--;p->live_names--;
 if(!b->live){
  if(!was_full)ntfs_pool_available_remove(p,b);
  if(b->previous)b->previous->next=b->next;else p->names=b->next;
  if(b->next)b->next->previous=b->previous;
  p->name_blocks--;p->bytes-=NTFS_POOL_NAME_BYTES;NTFS_POOL_FREE(b);
 }else if(was_full)ntfs_pool_available_add(p,b);
}
static void ntfs_pool_clear(ntfs_pool *p){
 while(p->names){ntfs_name_block *next=p->names->next;NTFS_POOL_FREE(p->names);p->names=next;}
 while(p->links){ntfs_link_block *next=p->links->next;NTFS_POOL_FREE(p->links);p->links=next;}
 memset(p,0,sizeof *p);
}
#endif
