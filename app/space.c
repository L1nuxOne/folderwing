#include "space.h"
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

static void names_release(sp_names *n){while(n&&InterlockedDecrement(&n->refs)==0){sp_names *next=n->next;free(n);n=next;}}
static uint32_t model_limit(const sp_model *m){return m->backend==SP_BACKEND_NTFS?SP_NTFS_LIMIT:m->backend==SP_BACKEND_SUMMARY?SP_SUMMARY_LIMIT:m->backend==SP_BACKEND_ANALYSIS?SP_ANALYSIS_LIMIT:SP_LIMIT;}
static uint64_t model_names_limit(const sp_model *m){return m->backend==SP_BACKEND_NTFS?SP_NTFS_NAMES_LIMIT:m->backend==SP_BACKEND_SUMMARY?SP_SUMMARY_NAMES_LIMIT:m->backend==SP_BACKEND_ANALYSIS?SP_ANALYSIS_NAMES_LIMIT:SP_NAMES_LIMIT;}
void space_model_free(sp_model *m){for(uint32_t i=m->count-m->lazy_slots;i<m->count;i++)if(m->nodes[i].flags&SP_LAZY)free(m->nodes[i].name);names_release(m->names);free(m->nodes);free(m->identities);free(m->stamps);memset(m,0,sizeof *m);}
static wchar_t *model_name(sp_model *m,const wchar_t *name){size_t n=wcslen(name)+1;if(m->name_bytes+n*2>model_names_limit(m))return NULL;sp_names *p=m->names;if(!p||n>p->capacity-p->used){size_t cap=n>32768?n:32768;p=malloc(sizeof *p+cap*2);if(!p)return NULL;*p=(sp_names){.next=m->names,.capacity=cap,.refs=1};m->names=p;}wchar_t *out=p->text+p->used;memcpy(out,name,n*2);p->used+=n;m->name_bytes+=n*2;return out;}
static uint32_t identity_hash(uint64_t device,uint64_t id,uint32_t mask){uint64_t h=(device*0x9e3779b97f4a7c15ull)^id;h^=h>>33;h*=0xff51afd7ed558ccdull;return (uint32_t)h&mask;}
static int grow_identities(sp_model *m){if(m->identity_capacity&&m->identity_count*2<m->identity_capacity)return 1;uint32_t cap=m->identity_capacity?m->identity_capacity*2:1024;if(cap>1u<<23)return 0;sp_identity *p=calloc(cap,sizeof *p);if(!p)return 0;for(uint32_t i=0;i<m->identity_capacity;i++)if(m->identities[i].owner){sp_identity x=m->identities[i];uint32_t at=identity_hash(x.device,x.id,cap-1);while(p[at].owner)at=(at+1)&(cap-1);p[at]=x;}free(m->identities);m->identities=p;m->identity_capacity=cap;return 1;}

static int problem(sp_model *m,DWORD error){m->error=error;m->state=SP_PARTIAL;return 0;}
static int adjust(sp_model *m,uint32_t id,const sp_totals *sum,int subtract){for(unsigned depth=0;id!=SP_NONE;depth++,id=m->nodes[id].parent){if(id>=m->count||depth>=130)return problem(m,ERROR_INVALID_DATA);uint64_t *to=(uint64_t*)&m->nodes[id].sum;const uint64_t *from=(const uint64_t*)sum;for(size_t k=0;k<sizeof *sum/8;k++){if(subtract?to[k]<from[k]:UINT64_MAX-to[k]<from[k])return problem(m,ERROR_ARITHMETIC_OVERFLOW);to[k]=subtract?to[k]-from[k]:to[k]+from[k];}}return 1;}
static int detach(sp_model *m,uint32_t id){sp_node *n=m->nodes+id;if(n->parent==SP_NONE)return id==0;sp_node *p=m->nodes+n->parent;uint32_t previous=SP_NONE,at=p->first;while(at!=SP_NONE&&at!=id){previous=at;at=m->nodes[at].next;}if(at==SP_NONE)return 0;if(previous==SP_NONE)p->first=n->next;else m->nodes[previous].next=n->next;if(p->last==id)p->last=previous;n->next=SP_NONE;return 1;}
static int update_record(sp_model *m,const sp_record *r,const wchar_t *name){
 if(m->backend!=SP_BACKEND_NTFS||!r->id||r->id>=m->count)return problem(m,ERROR_INVALID_DATA);sp_node *n=m->nodes+r->id;
 if(r->type==SP_REMOVE){if(n->flags&SP_REMOVED)return 1;sp_totals old=n->sum;if(n->first!=SP_NONE)return problem(m,ERROR_INVALID_DATA);if(!adjust(m,n->parent,&old,1)||!detach(m,r->id))return problem(m,ERROR_INVALID_DATA);n->flags|=SP_REMOVED;memset(&n->sum,0,sizeof n->sum);return 1;}
 if(n->flags&SP_REMOVED||r->parent>=m->count||!(m->nodes[r->parent].flags&SP_DIR)||m->nodes[r->parent].flags&SP_REMOVED||space_model_descendant(m,r->parent,r->id)||!name||!*name||wcschr(name,L'\\')||wcschr(name,L'/')||!wcscmp(name,L".")||!wcscmp(name,L"..")||r->flags&~(SP_DIR|SP_SKIP|SP_FAILED|SP_ALLOC_UNKNOWN|SP_HARDLINK)||(n->flags&SP_DIR)!=(r->flags&SP_DIR))return problem(m,ERROR_INVALID_DATA);
 wchar_t *copy=wcscmp(n->name,name)?model_name(m,name):n->name;if(!copy)return problem(m,ERROR_NOT_ENOUGH_MEMORY);sp_totals old=n->sum,next=(r->flags&SP_DIR)?old:(sp_totals){.logical=r->logical,.allocated=r->allocated,.files=!(r->flags&SP_SKIP),.skipped=!!(r->flags&SP_SKIP),.errors=!!(r->flags&SP_FAILED),.unknown=!!(r->flags&SP_ALLOC_UNKNOWN),.hardlinks=!!(r->flags&SP_HARDLINK)};
 if(!adjust(m,n->parent,&old,1))return 0;if(n->parent!=r->parent){if(!detach(m,r->id))return problem(m,ERROR_INVALID_DATA);sp_node *p=m->nodes+r->parent;if(p->last==SP_NONE)p->first=r->id;else m->nodes[p->last].next=r->id;p->last=r->id;n->parent=r->parent;}
 n->name=copy;n->sum=next;n->flags=r->flags|(r->flags&SP_DIR?SP_DONE:0);n->error=r->error;return adjust(m,n->parent,&next,0);
}
int space_model_record(sp_model *m,const sp_record *r,const wchar_t *name){
 if(r->magic!=SP_MAGIC)return problem(m,ERROR_INVALID_DATA);
 if(r->type==SP_ENTRY||r->type==SP_UPDATE||r->type==SP_REMOVE||r->type==SP_LEAVE)m->revision++;
 if(r->type==SP_RESET){space_model_free(m);m->backend=r->identity>=SP_BACKEND_NTFS&&r->identity<=SP_BACKEND_ANALYSIS?(uint32_t)r->identity:0;if(m->backend==SP_BACKEND_NTFS&&r->logical){uint32_t n=r->logical>=SP_NTFS_LIMIT?SP_NTFS_LIMIT:(uint32_t)r->logical+1;m->nodes=calloc(n,sizeof *m->nodes);if(!m->nodes)return problem(m,ERROR_NOT_ENOUGH_MEMORY);m->capacity=n;}return 1;}
 if(r->type==SP_STATUS){m->backend=r->identity==SP_BACKEND_NTFS?SP_BACKEND_NTFS:0;m->phase=r->flags;m->backend_error=r->error;if(r->device)m->backend_pid=(uint32_t)r->device;m->checked=r->logical;m->generation=r->allocated;return 1;}
 if(r->type==SP_TOTAL){if(m->backend!=SP_BACKEND_SUMMARY||r->id>=m->count||!(m->nodes[r->id].flags&SP_DIR)||m->nodes[r->id].flags&SP_TOTAL_DONE||r->flags>r->device||r->reserved>r->device)return problem(m,ERROR_INVALID_DATA);sp_totals direct={.logical=r->logical,.allocated=r->allocated,.files=r->device,.skipped=r->parent,.errors=r->error,.unknown=r->flags,.hardlinks=r->reserved};m->nodes[r->id].flags|=SP_TOTAL_DONE;m->revision++;return adjust(m,r->id,&direct,0);}
 if(r->type==SP_UPDATE||r->type==SP_REMOVE)return update_record(m,r,name);
 if(r->type==SP_END){if(r->flags<SP_COMPLETE||r->flags>SP_STOPPED)return problem(m,ERROR_INVALID_DATA);if(m->backend==SP_BACKEND_NTFS){m->state=r->flags;m->error=r->error;}else {if(!m->error)m->state=r->flags;if(r->error&&!m->error)m->error=r->error;}return 1;}
 if(r->type==SP_LEAVE){if(r->id>=m->count||!(m->nodes[r->id].flags&SP_DIR)||(m->backend==SP_BACKEND_SUMMARY&&!(m->nodes[r->id].flags&SP_TOTAL_DONE)))return problem(m,ERROR_INVALID_DATA);m->nodes[r->id].flags|=SP_DONE;m->nodes[r->id].end=m->count;return 1;}
 if(r->type!=SP_ENTRY||r->id!=m->count||r->flags&~(SP_DIR|SP_SKIP|SP_FAILED|SP_ALLOC_UNKNOWN|(m->backend==SP_BACKEND_NTFS?SP_HARDLINK:0))||!name||
  (r->id?(r->parent>=m->count||!(m->nodes[r->parent].flags&SP_DIR)||!name[0]||wcschr(name,L'\\')||wcschr(name,L'/')||!wcscmp(name,L".")||!wcscmp(name,L"..")):r->parent!=SP_NONE))return problem(m,ERROR_INVALID_DATA);
 size_t bytes=(wcslen(name)+1)*sizeof *name;
 uint32_t limit=model_limit(m);uint64_t names_limit=model_names_limit(m);
 if(m->backend==SP_BACKEND_SUMMARY&&(m->lazy_slots||!(r->flags&SP_DIR)||r->logical||r->allocated))return problem(m,ERROR_INVALID_DATA);
 if(m->count>=limit||m->name_bytes+bytes>names_limit)return problem(m,ERROR_BUFFER_OVERFLOW);
 if(m->count==m->capacity){uint32_t cap=m->capacity?m->capacity*2:256;if(cap>limit)cap=limit;sp_node *nodes=realloc(m->nodes,(size_t)cap*sizeof *nodes);if(!nodes)return problem(m,ERROR_NOT_ENOUGH_MEMORY);m->nodes=nodes;if(m->backend==SP_BACKEND_SUMMARY){sp_stamp *stamps=realloc(m->stamps,(size_t)cap*sizeof *stamps);if(!stamps)return problem(m,ERROR_NOT_ENOUGH_MEMORY);m->stamps=stamps;}m->capacity=cap;}
 wchar_t *copy=model_name(m,name);if(!copy)return problem(m,ERROR_NOT_ENOUGH_MEMORY);
 sp_totals delta={.logical=r->logical,.allocated=r->allocated,.files=!(r->flags&(SP_DIR|SP_SKIP)),.folders=!!(r->flags&SP_DIR),.skipped=!!(r->flags&SP_SKIP),.errors=!!(r->flags&SP_FAILED),.unknown=!!(r->flags&SP_ALLOC_UNKNOWN),.hardlinks=!!(r->flags&SP_HARDLINK)};
 uint32_t flags=r->flags;
 if(!(flags&(SP_DIR|SP_SKIP|SP_FAILED))&&r->identity&&m->backend!=SP_BACKEND_NTFS){
  if(!grow_identities(m))return problem(m,ERROR_NOT_ENOUGH_MEMORY);
  uint32_t at=identity_hash(r->device,r->identity,m->identity_capacity-1);
  while(m->identities[at].owner&&(m->identities[at].device!=r->device||m->identities[at].id!=r->identity))at=(at+1)&(m->identity_capacity-1);
  if(m->identities[at].owner){delta.allocated=0;delta.unknown=0;delta.hardlinks=1;flags|=SP_HARDLINK;}
  else {m->identities[at]=(sp_identity){r->device,r->identity,r->id+1};m->identity_count++;}
 }
 sp_node node={.name=copy,.parent=r->parent,.first=SP_NONE,.last=SP_NONE,.next=SP_NONE,.flags=flags,.error=r->error,.end=r->id+1};m->nodes[m->count++]=node;if(!r->id){m->root_device=r->device;m->root_identity=r->identity;}if(m->backend==SP_BACKEND_SUMMARY)m->stamps[r->id]=(sp_stamp){r->device,r->identity};
 if(r->parent!=SP_NONE){sp_node *parent=&m->nodes[r->parent];if(parent->last!=SP_NONE)m->nodes[parent->last].next=r->id;else parent->first=r->id;parent->last=r->id;}
 uint32_t id=r->id;unsigned depth=0;
 while(id!=SP_NONE){if(++depth>130)return problem(m,ERROR_STACK_OVERFLOW);sp_node *n=&m->nodes[id];uint64_t *to=(uint64_t*)&n->sum,*from=(uint64_t*)&delta;
  for(size_t i=0;i<sizeof delta/8;i++){if(UINT64_MAX-to[i]<from[i])return problem(m,ERROR_ARITHMETIC_OVERFLOW);to[i]+=from[i];}id=n->parent;}
 return 1;
}

/* Lazy leaves borrow no name slabs and never contribute to ancestor totals. */
uint32_t space_model_list_add(sp_model *m,uint32_t parent,const sp_node *file){
 if(m->backend!=SP_BACKEND_SUMMARY||m->state==SP_RUNNING||parent>=m->count||!(m->nodes[parent].flags&SP_DIR)||file->flags&SP_DIR)return SP_NONE;
 if(!m->lazy_free&&m->lazy_slots>=65536)return SP_NONE;
 wchar_t *name=_wcsdup(file->name);if(!name)return SP_NONE;uint32_t id;
 if(m->lazy_free){id=m->lazy_free-1;m->lazy_free=m->nodes[id].next;}else{if(m->count==m->capacity){uint32_t cap=m->capacity?m->capacity*2:256;if(cap>SP_SUMMARY_LIMIT+65536)cap=SP_SUMMARY_LIMIT+65536;sp_node *p=realloc(m->nodes,(size_t)cap*sizeof *p);if(!p){free(name);return SP_NONE;}m->nodes=p;m->capacity=cap;}id=m->count++;m->lazy_slots++;}
 m->nodes[id]=*file;m->nodes[id].name=name;m->nodes[id].parent=parent;m->nodes[id].first=m->nodes[id].last=m->nodes[id].next=SP_NONE;m->nodes[id].flags|=SP_LAZY;m->nodes[id].end=id+1;
 sp_node *p=m->nodes+parent;if(p->last!=SP_NONE)m->nodes[p->last].next=id;else p->first=id;p->last=id;m->revision++;return id;
}
void space_model_list_clear(sp_model *m,uint32_t parent){if(parent>=m->count)return;sp_node *p=m->nodes+parent;uint32_t at=p->first,previous=SP_NONE;
 while(at!=SP_NONE){sp_node *n=m->nodes+at;uint32_t next=n->next;if(n->flags&SP_LAZY){if(previous==SP_NONE)p->first=next;else m->nodes[previous].next=next;if(p->last==at)p->last=previous;free(n->name);n->name=NULL;n->flags=SP_LAZY|SP_REMOVED;n->next=m->lazy_free;m->lazy_free=at+1;}else previous=at;at=next;}m->revision++;
}
int space_model_descendant(const sp_model *m,uint32_t id,uint32_t root){for(unsigned d=0;id<m->count&&d<130;d++,id=m->nodes[id].parent)if(id==root)return 1;return 0;}
wchar_t *space_model_path(const sp_model *m,uint32_t id,const wchar_t *root){
 uint32_t stack[130],n=0;if(id>=m->count||!root||m->nodes[id].flags&SP_REMOVED)return NULL;size_t chars=wcslen(root)+1;
 while(id&&n<130){stack[n++]=id;chars+=wcslen(m->nodes[id].name)+1;id=m->nodes[id].parent;}if(id||chars>32768)return NULL;
 wchar_t *s=malloc(chars*sizeof *s);if(!s)return NULL;wcscpy(s,root);
 while(n){size_t at=wcslen(s);if(at&&s[at-1]!=L'\\')wcscat(s,L"\\");wcscat(s,m->nodes[stack[--n]].name);}return s;
}
static void extension_of(const wchar_t *name,wchar_t out[48]){const wchar_t *p=wcsrchr(name,L'.');if(!p||p==name||!p[1]){wcscpy(out,L"(no extension)");return;}if(wcslen(p)>=48){wcscpy(out,L"(long extension)");return;}size_t i=0;do{out[i]=towlower(p[i]);}while(p[i++]);}
static int contains(const wchar_t *name,const wchar_t *needle){if(!needle||!*needle)return 1;size_t n=wcslen(needle);for(;*name;name++)if(!_wcsnicmp(name,needle,n))return 1;return 0;}
/* Context is thread-local: panel queries and test workers never share sort state. */
static _Thread_local const sp_model *sort_model;
static _Thread_local int sort_order,sort_view;
static int row_cmp(const void *a_,const void *b_){const sp_row *a=a_,*b=b_;
 if(sort_view==0){int ad=!!(sort_model->nodes[a->id].flags&SP_DIR),bd=!!(sort_model->nodes[b->id].flags&SP_DIR);if(ad!=bd)return bd-ad;}
 int c=sort_order==2?0:(a->size>b->size?-1:a->size<b->size?1:0);if(sort_order==1)c=-c;
 if(!c)c=_wcsicmp(sort_view==2?a->type:sort_model->nodes[a->id].name,sort_view==2?b->type:sort_model->nodes[b->id].name);
 if(!c)c=(a->id>b->id)-(a->id<b->id);return c;}
static uint32_t type_hash(const wchar_t *s){uint32_t h=2166136261u;while(*s){h^=(uint16_t)*s++;h*=16777619u;}return h;}
/* Follow captured child/sibling links. Unlike an ID range or ancestor test,
 * this visits only the requested subtree, including entries moved into it. */
static uint32_t subtree_next(const sp_model *m,uint32_t root,uint32_t id){if(m->nodes[id].first!=SP_NONE)return m->nodes[id].first;while(id!=root){if(m->nodes[id].next!=SP_NONE)return m->nodes[id].next;id=m->nodes[id].parent;}return SP_NONE;}
static int grow_groups(sp_row **rows,uint32_t **table,uint32_t *capacity,uint32_t used){uint32_t cap=*capacity?*capacity*2:32;sp_row *p=realloc(*rows,(size_t)cap*sizeof *p);if(!p)return 0;*rows=p;uint32_t *slots=calloc((size_t)cap*2,sizeof *slots);if(!slots)return 0;for(uint32_t i=0;i<used;i++){uint32_t at=type_hash(p[i].type)&(cap*2-1);while(slots[at])at=(at+1)&(cap*2-1);slots[at]=i+1;}free(*table);*table=slots;*capacity=cap;return 1;}
static void tree_rows(const sp_model *m,uint32_t parent,uint32_t depth,int allocated,const wchar_t *filter,const unsigned char *expanded,sp_row *rows,uint32_t *used){
 uint32_t start=*used;
 for(uint32_t id=m->nodes[parent].first;id!=SP_NONE;id=m->nodes[id].next){const sp_node *n=&m->nodes[id];if(n->flags&SP_REMOVED||!contains(n->name,filter))continue;
  rows[(*used)++]=(sp_row){.id=id,.depth=depth,.size=allocated?n->sum.allocated:n->sum.logical,.logical=n->sum.logical,.allocated=n->sum.allocated,.files=n->sum.files};}
 qsort(rows+start,*used-start,sizeof *rows,row_cmp);
 /* Insert descendants after each expanded row, avoiding rescans/reordering of the model. */
 uint32_t children=*used-start;if(!children)return;sp_row *saved=malloc((size_t)children*sizeof *saved);if(!saved)return;memcpy(saved,rows+start,(size_t)children*sizeof *saved);*used=start;
 for(uint32_t i=0;i<children;i++){rows[(*used)++]=saved[i];uint32_t id=saved[i].id;if(expanded&&expanded[id]&&(m->nodes[id].flags&SP_DIR)&&depth<128)tree_rows(m,id,depth+1,allocated,filter,expanded,rows,used);}free(saved);
}
static uint32_t tree_capacity(const sp_model *m,uint32_t parent,const unsigned char *expanded,unsigned depth){if(depth>128)return 0;uint32_t n=0;for(uint32_t id=m->nodes[parent].first;id!=SP_NONE;id=m->nodes[id].next){if(m->nodes[id].flags&SP_REMOVED)continue;n++;if(expanded&&expanded[id]&&(m->nodes[id].flags&SP_DIR))n+=tree_capacity(m,id,expanded,depth+1);}return n;}
/* A query owns only immutable copied values and a lease on append-only names.
 * The caller holds its model lock for begin/capture, never for work/finish. */
#define QUERY_BATCH 256u
typedef struct query_file {uint64_t logical,allocated;const wchar_t *name;uint32_t id;} query_file;
struct sp_query {
 sp_names *names;uint64_t revision;uint32_t root,next,batch_count,count,capacity;int view,allocated,sort,extension_set;
 wchar_t *filter,*extension;query_file batch[QUERY_BATCH],*files;sp_row *groups;uint32_t *table;
};
static int query_cancelled(HANDLE cancel){return cancel&&WaitForSingleObject(cancel,0)==WAIT_OBJECT_0;}
sp_query *space_query_begin(const sp_model *m,uint32_t root,int view,int allocated,int sort,const wchar_t *filter,const wchar_t *extension){
 if(root>=m->count||view<1||view>3||sort<0||sort>2){SetLastError(ERROR_INVALID_PARAMETER);return NULL;}
 sp_query *q=calloc(1,sizeof *q);if(!q){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return NULL;}
 q->filter=_wcsdup(filter?filter:L"");q->extension=_wcsdup(extension?extension:L"");if(!q->filter||!q->extension){space_query_free(q);SetLastError(ERROR_NOT_ENOUGH_MEMORY);return NULL;}
 q->names=m->names;if(q->names)InterlockedIncrement(&q->names->refs);q->revision=m->revision;q->root=root;q->next=m->nodes[root].first;q->view=view;q->allocated=allocated;q->sort=sort;q->extension_set=extension!=NULL;return q;
}
DWORD space_query_capture(sp_query *q,const sp_model *m,int *done){
 *done=0;if(m->revision!=q->revision||m->names!=q->names)return ERROR_RETRY;q->batch_count=0;
 for(unsigned visited=0;q->next!=SP_NONE&&visited<QUERY_BATCH;visited++){uint32_t id=q->next;const sp_node *n=m->nodes+id;q->next=subtree_next(m,q->root,id);if(!(n->flags&(SP_DIR|SP_SKIP|SP_REMOVED)))q->batch[q->batch_count++]=(query_file){n->sum.logical,n->sum.allocated,n->name,id};}
 *done=q->next==SP_NONE;return 0;
}
DWORD space_query_work(sp_query *q,HANDLE cancel){
 for(uint32_t i=0;i<q->batch_count;i++){if(!(i&127)&&query_cancelled(cancel))return ERROR_CANCELLED;query_file f=q->batch[i];if(!contains(f.name,q->filter))continue;
  if(q->view==2){wchar_t type[48];extension_of(f.name,type);if(q->count==q->capacity&&!grow_groups(&q->groups,&q->table,&q->capacity,q->count))return ERROR_NOT_ENOUGH_MEMORY;uint32_t at=type_hash(type)&(q->capacity*2-1);while(q->table[at]&&wcscmp(q->groups[q->table[at]-1].type,type))at=(at+1)&(q->capacity*2-1);
   if(!q->table[at]){q->table[at]=++q->count;q->groups[q->count-1]=(sp_row){.id=f.id};wcscpy(q->groups[q->count-1].type,type);}sp_row *r=q->groups+q->table[at]-1;if(f.id<r->id)r->id=f.id;r->logical+=f.logical;r->allocated+=f.allocated;r->size+=q->allocated?f.allocated:f.logical;r->files++;
  }else{if(q->view==3&&q->extension_set){wchar_t type[48];extension_of(f.name,type);if(wcscmp(type,q->extension))continue;}if(q->count==q->capacity){uint32_t cap=q->capacity?q->capacity*2:256;query_file *p=realloc(q->files,(size_t)cap*sizeof *p);if(!p)return ERROR_NOT_ENOUGH_MEMORY;q->files=p;q->capacity=cap;}q->files[q->count++]=f;}
 }
 q->batch_count=0;return 0;
}
static _Thread_local const sp_query *sorting_query;
static int query_file_cmp(const void *a_,const void *b_){const query_file *a=a_,*b=b_;uint64_t x=sorting_query->allocated?a->allocated:a->logical,y=sorting_query->allocated?b->allocated:b->logical;int c=sorting_query->sort==2?0:x>y?-1:x<y?1:0;if(sorting_query->sort==1)c=-c;if(!c)c=_wcsicmp(a->name,b->name);return c?c:(a->id>b->id)-(a->id<b->id);}
/* Bottom-up merge sort has bounded cancellation points, including its longest
 * merge. It never holds or reads the live model. */
static DWORD query_sort(void *items,uint32_t n,size_t width,int (*cmp)(const void *,const void *),HANDLE cancel){
 if(query_cancelled(cancel))return ERROR_CANCELLED;if(n<2)return 0;uint8_t *scratch=malloc((size_t)n*width);if(!scratch)return ERROR_NOT_ENOUGH_MEMORY;uint8_t *src=items,*dst=scratch;DWORD error=0;
 for(uint32_t run=1;run<n;run*=2){for(uint32_t base=0;base<n;base+=run*2){uint32_t mid=base+run<n?base+run:n,end=base+run*2<n?base+run*2:n,i=base,j=mid,k=base;
   while(i<mid&&j<end){if(!(k&1023)&&query_cancelled(cancel)){error=ERROR_CANCELLED;goto done;}uint32_t at=cmp(src+(size_t)i*width,src+(size_t)j*width)<=0?i++:j++;memcpy(dst+(size_t)k++*width,src+(size_t)at*width,width);}
   if(query_cancelled(cancel)){error=ERROR_CANCELLED;goto done;}if(i<mid)memcpy(dst+(size_t)k*width,src+(size_t)i*width,(size_t)(mid-i)*width);else if(j<end)memcpy(dst+(size_t)k*width,src+(size_t)j*width,(size_t)(end-j)*width);
  }uint8_t *swap=src;src=dst;dst=swap;}
 if(src!=(uint8_t*)items)memcpy(items,src,(size_t)n*width);if(query_cancelled(cancel))error=ERROR_CANCELLED;
 done:free(scratch);return error;
}
sp_row *space_query_finish(sp_query *q,HANDLE cancel,uint32_t *count,DWORD *error){
 *count=0;*error=0;sorting_query=q;sort_view=q->view;sort_order=q->sort;
 if(q->next!=SP_NONE||q->batch_count){*error=ERROR_INVALID_STATE;return NULL;}
 *error=query_sort(q->view==2?(void*)q->groups:(void*)q->files,q->count,q->view==2?sizeof(sp_row):sizeof(query_file),q->view==2?row_cmp:query_file_cmp,cancel);if(*error)return NULL;
 sp_row *rows=q->view==2?q->groups:malloc((size_t)(q->count?q->count:1)*sizeof *rows);if(!rows&&q->view==2&&!q->count)rows=calloc(1,sizeof *rows);if(!rows){*error=ERROR_NOT_ENOUGH_MEMORY;return NULL;}
 if(q->view==2)q->groups=NULL;else for(uint32_t i=0;i<q->count;i++){if(!(i&255)&&query_cancelled(cancel)){free(rows);*error=ERROR_CANCELLED;return NULL;}query_file f=q->files[i];rows[i]=(sp_row){.id=f.id,.size=q->allocated?f.allocated:f.logical,.logical=f.logical,.allocated=f.allocated,.files=1};extension_of(f.name,rows[i].type);}
 *count=q->count;return rows;
}
uint64_t space_query_revision(const sp_query *q){return q->revision;}
void space_query_free(sp_query *q){if(!q)return;names_release(q->names);free(q->files);free(q->groups);free(q->table);free(q->filter);free(q->extension);free(q);}
sp_row *space_model_rows(const sp_model *m,uint32_t root,int view,int allocated,int sort,const wchar_t *filter,const wchar_t *ext,const unsigned char *expanded,uint32_t *count){
 *count=0;if(root>=m->count)return NULL;sort_model=m;sort_order=sort;sort_view=view;
 if(view==0){uint32_t capacity=tree_capacity(m,root,expanded,0);sp_row *rows=calloc(capacity?capacity:1,sizeof *rows);if(rows)tree_rows(m,root,0,allocated,filter,expanded,rows,count);return rows;}
 sp_query *q=space_query_begin(m,root,view,allocated,sort,filter,ext);if(!q)return NULL;DWORD error=0;int done=0;while(!error&&!done){error=space_query_capture(q,m,&done);if(!error)error=space_query_work(q,NULL);}sp_row *rows=error?NULL:space_query_finish(q,NULL,count,&error);space_query_free(q);if(error)SetLastError(error);return rows;
}
