#include "duplicates.h"
#include "space.h"
#include "termutil.h"
#include "../src/namecodec.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef struct identity {uint64_t device,id;uint32_t root,used;} identity;
#define DUP_IDS (1u<<18)
typedef struct collect {sp_model model;du_stamp *stamps;uint32_t capacity;duplicate_result *out;HANDLE cancel;duplicate_notify notify;void *user;} collect;
static int stopped(HANDLE cancel){return cancel&&WaitForSingleObject(cancel,0)==WAIT_OBJECT_0;}
static void progress(duplicate_result *r,uint32_t phase,duplicate_notify notify,void *user){if(notify){duplicate_progress p={phase,r->count,r->groups,r->hashed,r->compared,r->errors};notify(user,&p);}}
static int collect_record(void *data,const sp_record *r,const wchar_t *name){collect *c=data;if(stopped(c->cancel))return 0;if(r->type==SP_ENTRY){if(c->out->entries>=DUP_FILES){c->out->error=ERROR_BUFFER_OVERFLOW;return 0;}c->out->entries++;if(r->id>=c->capacity){uint32_t n=c->capacity?c->capacity*2:1024;if(n<=r->id)n=r->id+1;du_stamp *p=realloc(c->stamps,(size_t)n*sizeof *p);if(!p){c->out->error=ERROR_NOT_ENOUGH_MEMORY;return 0;}c->stamps=p;c->capacity=n;}c->stamps[r->id]=(du_stamp){r->logical,r->device,r->identity,0,0};}
 if(!space_model_record(&c->model,r,name)){c->out->error=ERROR_INVALID_DATA;return 0;}if(r->type==SP_ENTRY&&!(r->id%128)&&c->notify){duplicate_progress p={.phase=DUP_ENUMERATE,.files=c->out->count+(uint32_t)c->model.nodes[0].sum.files,.errors=c->out->errors+c->model.nodes[0].sum.errors};c->notify(c->user,&p);}return 1;}
void duplicates_free(duplicate_result *r){for(uint32_t i=0;i<r->count;i++)free(r->files[i].relative);free(r->files);memset(r,0,sizeof *r);}
static int by_size(const void *a,const void *b){const duplicate_file *x=a,*y=b;return x->scan.size<y->scan.size?-1:x->scan.size>y->scan.size?1:0;}
static int by_hash(const void *a,const void *b){const duplicate_file *x=a,*y=b;if(x->valid!=y->valid)return x->valid?-1:1;int n=by_size(a,b);return n?n:memcmp(x->hash,y->hash,32);}
static int by_root(const void *a,const void *b){const duplicate_file *x=a,*y=b;return x->root<y->root?-1:x->root>y->root?1:0;}
static int by_group(const void *a,const void *b){const duplicate_file *x=a,*y=b;uint32_t g=x->group?x->group:UINT32_MAX,h=y->group?y->group:UINT32_MAX;if(g!=h){if(x->group&&y->group&&x->scan.size!=y->scan.size)return x->scan.size>y->scan.size?-1:1;return g<h?-1:1;}int n=by_root(a,b);return n?n:wcscmp(x->relative,y->relative);}
static int ancestor(const wchar_t *a,const wchar_t *b,int fold){size_t n=wcslen(a),m=wcslen(b);while(n>1&&(a[n-1]==L'\\'||a[n-1]==L'/'))n--;if(m<n||(fold?_wcsnicmp(a,b,n):wcsncmp(a,b,n)))return 0;return m==n||a[n-1]==L'/'||a[n-1]==L'\\'||b[n]==L'\\'||b[n]==L'/';}
static int covered(const wchar_t *a,const wchar_t *b){wchar_t da[128],db[128],pa[32768],pb[32768];int x=term_wsl_split(a,da,128,pa,32768),y=term_wsl_split(b,db,128,pb,32768);if(x!=y)return 0;if(x)return !_wcsicmp(da,db)&&ancestor(pa,pb,0);return ancestor(a,b,0);}
/* Lexical ancestry orders requests, but cannot prove native traversal across
 * exclusions. A completed scan must contain the exact entered/finished scope.
 * Work is bounded by the existing model and at most DUP_ROOTS requested roots. */
static int native_scope_covered(const sp_model *m,const wchar_t *parent,const wchar_t *child){
 if(m->state!=SP_COMPLETE||m->error||!m->count)return 0;
 wchar_t da[128],db[128],pa[32768],pb[32768],name[4096];
 if(!term_wsl_split(parent,da,128,pa,32768)||!term_wsl_split(child,db,128,pb,32768)||_wcsicmp(da,db)||!ancestor(pa,pb,0))return 0;
 size_t n=wcslen(pa);while(n>1&&pa[n-1]==L'/')n--;const wchar_t *at=pb+n;uint32_t id=0;
 while(*at==L'/')at++;
 while(*at){const wchar_t *end=wcschr(at,L'/');size_t length=end?(size_t)(end-at):wcslen(at);uint32_t next=SP_NONE;
  for(uint32_t i=m->nodes[id].first;i!=SP_NONE;i=m->nodes[i].next){const sp_node *node=m->nodes+i;
   if((node->flags&(SP_DIR|SP_DONE))!=(SP_DIR|SP_DONE)||node->flags&(SP_SKIP|SP_FAILED|SP_REMOVED))continue;
   if(fpe_lx_literal(node->name,name,4096,0)&&wcslen(name)==length&&!wcsncmp(name,at,length)){next=i;break;}
  }
  if(next==SP_NONE)return 0;id=next;at=end?end+1:at+length;while(*at==L'/')at++;
 }
 const sp_node *node=m->nodes+id;return (node->flags&(SP_DIR|SP_DONE))==(SP_DIR|SP_DONE)&&!(node->flags&(SP_SKIP|SP_FAILED|SP_REMOVED));
}
static void endpoint(const wchar_t *root,wchar_t key[1024]){wchar_t d[128],p[32768];key[0]=0;if(term_wsl_split(root,d,128,p,32768)){swprintf(key,1024,L"wsl:%ls",d);return;}wchar_t vol[1024];if(GetVolumePathNameW(root,vol,1024)&&GetVolumeNameForVolumeMountPointW(vol,key,1024))return;if(root[0]==L'\\'&&root[1]==L'\\'){const wchar_t *a=wcschr(root+2,L'\\'),*b=a?wcschr(a+1,L'\\'):NULL;size_t n=b?(size_t)(b-root):wcslen(root);if(n<1024){memcpy(key,root,n*2);key[n]=0;}}}
int duplicates_equal(du_reader *a,du_reader *b,const duplicate_file *left,const duplicate_file *right,HANDLE cancel,DWORD *error){*error=duplicate_reader_open(a,left->relative,&left->scan);if(!*error&&memcmp(&a->stamp,&left->hashed,sizeof a->stamp))*error=ERROR_FILE_INVALID;if(!*error)*error=duplicate_reader_open(b,right->relative,&right->scan);if(!*error&&memcmp(&b->stamp,&right->hashed,sizeof b->stamp))*error=ERROR_FILE_INVALID;if(*error)return -1;uint8_t x[DU_BLOCK],y[DU_BLOCK];uint64_t total=0;int equal=1;for(;;){if(stopped(cancel)){*error=ERROR_CANCELLED;return -1;}DWORD nx,ny;*error=duplicate_reader_read(a,x,&nx);if(!*error)*error=duplicate_reader_read(b,y,&ny);if(*error)return -1;if(nx!=ny){equal=0;/* ordinary disk readers fill a block except at EOF */}if(nx==ny&&nx&&memcmp(x,y,nx))equal=0;total+=nx;if(!nx&&!ny)break;}if(total!=left->scan.size){*error=ERROR_FILE_INVALID;return -1;}return equal;}
typedef struct slot {du_reader reader;uint32_t root;int live;} slot;
static DWORD open_slot(slot *s,uint32_t root,const wchar_t *const *roots,int wsl,int as_root,HANDLE cancel){if(s->live&&s->root==root)return 0;if(s->live)duplicate_reader_free(&s->reader);s->live=0;DWORD e=duplicate_reader_init(&s->reader,roots[root],wsl,as_root,cancel);if(e){duplicate_reader_free(&s->reader);return e;}s->root=root;s->live=1;return 0;}
void duplicates_scan(const wchar_t *const *roots,uint32_t count,int wsl,int as_root,HANDLE cancel,duplicate_notify notify,void *user,duplicate_result *out){memset(out,0,sizeof *out);out->state=DUP_ENUMERATE;if(!count||count>DUP_ROOTS){out->error=ERROR_INVALID_PARAMETER;out->state=DUP_PARTIAL;return;}identity *ids=calloc(DUP_IDS,sizeof *ids);if(!ids){out->error=ERROR_NOT_ENOUGH_MEMORY;out->errors=1;out->state=DUP_PARTIAL;return;}wchar_t keys[DUP_ROOTS][1024],distro[128],path[32768];uint32_t order[DUP_ROOTS],visited=0,scanned=0;int native[DUP_ROOTS];for(uint32_t i=0;i<count;i++){endpoint(roots[i],keys[i]);order[i]=i;native[i]=term_wsl_split(roots[i],distro,128,path,32768);}
 /* Parent-first execution keeps ordinary overlap single-pass without changing
  * the caller's root indices used by results and host action authority. */
 for(uint32_t i=0;i<count;i++)for(uint32_t j=i+1;j<count;j++)if(covered(roots[order[j]],roots[order[i]])&&!covered(roots[order[i]],roots[order[j]])){uint32_t swap=order[i];order[i]=order[j];order[j]=swap;}
 for(uint32_t step=0;step<count&&!stopped(cancel)&&!out->error;step++){uint32_t root=order[step];int skip=0;
  if(native[root]){skip=!!(visited&(1u<<root));for(uint32_t j=0;!skip&&j<count;j++)if((scanned&(1u<<j))&&covered(roots[j],roots[root])&&covered(roots[root],roots[j]))skip=1;}
  else for(uint32_t j=0;j<count;j++)if(j!=root&&covered(roots[j],roots[root])&&(!covered(roots[root],roots[j])||j<root)){skip=1;break;}
  if(skip)continue;scanned|=1u<<root;
 collect c={.out=out,.cancel=cancel,.notify=notify,.user=user};DWORD error=space_scan(roots[root],wsl,as_root,cancel,collect_record,&c);if(error||c.model.state!=SP_COMPLETE)out->errors++;if(c.model.count){out->skipped+=c.model.nodes[0].sum.skipped;out->aliases+=c.model.nodes[0].sum.hardlinks;out->errors+=c.model.nodes[0].sum.errors;}
 for(uint32_t i=0;i<c.model.count&&!out->error;i++){sp_node *node=&c.model.nodes[i];if(node->flags&(SP_DIR|SP_SKIP|SP_FAILED|SP_HARDLINK))continue;du_stamp st=c.stamps[i];int alias=0;
  if(keys[root][0]&&st.identity){uint64_t h=(st.device*0x9e3779b97f4a7c15ull)^st.identity;uint32_t at=(uint32_t)(h^(h>>32))&(DUP_IDS-1);while(ids[at].used){identity *id=&ids[at];if(id->device==st.device&&id->id==st.identity&&!_wcsicmp(keys[root],keys[id->root])){alias=1;break;}at=(at+1)&(DUP_IDS-1);}if(!alias)ids[at]=(identity){st.device,st.identity,root,1};}if(alias){out->aliases++;continue;}
  wchar_t *full=space_model_path(&c.model,i,roots[root]);if(!full){out->error=ERROR_NOT_ENOUGH_MEMORY;break;}size_t n=wcslen(roots[root]);const wchar_t *rel=full+n;while(*rel==L'\\')rel++;size_t bytes=(wcslen(rel)+1)*2;if(!*rel||out->path_bytes+bytes>DUP_PATH_BYTES||out->count>=DUP_FILES){free(full);out->error=ERROR_BUFFER_OVERFLOW;break;}
  if(out->count==out->capacity){uint32_t cap=out->capacity?out->capacity*2:1024;duplicate_file *p=realloc(out->files,(size_t)cap*sizeof *p);if(!p){free(full);out->error=ERROR_NOT_ENOUGH_MEMORY;break;}out->files=p;out->capacity=cap;}wchar_t *copy=_wcsdup(rel);free(full);if(!copy){out->error=ERROR_NOT_ENOUGH_MEMORY;break;}out->files[out->count++]=(duplicate_file){.relative=copy,.scan=st,.root=root};out->path_bytes+=bytes;
 }if(native[root]&&!error&&!out->error)for(uint32_t j=0;j<count;j++)if(native[j]&&!(scanned&(1u<<j))&&native_scope_covered(&c.model,roots[root],roots[j]))visited|=1u<<j;
 free(c.stamps);space_model_free(&c.model);progress(out,DUP_ENUMERATE,notify,user);}
 free(ids);if(out->error)out->errors++;qsort(out->files,out->count,sizeof *out->files,by_size);for(uint32_t i=0;i<out->count;){uint32_t end=i+1;while(end<out->count&&out->files[end].scan.size==out->files[i].scan.size)end++;if(end-i>1)for(uint32_t j=i;j<end;j++)out->files[j].candidate=1;i=end;}
 slot a={0},b={0};qsort(out->files,out->count,sizeof *out->files,by_root);out->state=DUP_HASHING;progress(out,DUP_HASHING,notify,user);
 for(uint32_t i=0;i<out->count&&!stopped(cancel);i++){duplicate_file *f=&out->files[i];if(!f->candidate)continue;DWORD e=open_slot(&a,f->root,roots,wsl,as_root,cancel);if(!e)e=duplicate_reader_open(&a.reader,f->relative,&f->scan);if(!e)e=duplicate_reader_hash(&a.reader,f->hash);if(e){out->errors++;if(!out->error)out->error=e;/* broken channels are never reused */if(a.live){duplicate_reader_free(&a.reader);a.live=0;}}else{f->valid=1;f->hashed=a.reader.stamp;out->hashed++;}progress(out,DUP_HASHING,notify,user);}
 qsort(out->files,out->count,sizeof *out->files,by_hash);out->state=DUP_VERIFYING;progress(out,DUP_VERIFYING,notify,user);
 for(uint32_t start=0;start<out->count&&!stopped(cancel);){if(!out->files[start].valid)break;uint32_t end=start+1;while(end<out->count&&out->files[end].valid&&!by_hash(&out->files[start],&out->files[end]))end++;
  for(uint32_t i=start;i<end&&!stopped(cancel);i++){duplicate_file *f=&out->files[i];if(f->group||!f->valid)continue;uint32_t group=0;for(uint32_t j=i+1;j<end&&!stopped(cancel);j++){duplicate_file *g=&out->files[j];if(g->group||!g->valid)continue;DWORD e=open_slot(&a,f->root,roots,wsl,as_root,cancel);if(!e)e=open_slot(&b,g->root,roots,wsl,as_root,cancel);int equal=e?-1:duplicates_equal(&a.reader,&b.reader,f,g,cancel,&e);out->compared++;if(equal<0){out->errors++;if(!out->error)out->error=e;/* Drop the entire unsettled group on a validation failure. */if(group){for(uint32_t k=i;k<=j;k++)if(out->files[k].group==group)out->files[k].group=0;out->groups--;group=0;}f->valid=0;g->valid=0;if(a.live){duplicate_reader_free(&a.reader);a.live=0;}if(b.live){duplicate_reader_free(&b.reader);b.live=0;}break;}if(equal){if(!group)group=++out->groups;f->group=g->group=group;}progress(out,DUP_VERIFYING,notify,user);}
  }start=end;}
 if(a.live)duplicate_reader_free(&a.reader);if(b.live)duplicate_reader_free(&b.reader);qsort(out->files,out->count,sizeof *out->files,by_group);uint32_t last=0,number=0;for(uint32_t i=0;i<out->count;i++){duplicate_file *f=&out->files[i];if(!f->group)continue;out->members++;if(f->group==last){if(UINT64_MAX-out->duplicate_bytes<f->scan.size){out->duplicate_bytes=UINT64_MAX;out->errors++;}else out->duplicate_bytes+=f->scan.size;}if(f->group!=last)number++;last=f->group;f->group=number;}
 out->state=stopped(cancel)?DUP_STOPPED:out->errors?DUP_PARTIAL:DUP_COMPLETE;progress(out,out->state,notify,user);
}
