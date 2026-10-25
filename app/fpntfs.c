/* Disposable, read-only NTFS worker. Cache IO impersonates its exact owner. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>
#include <stdio.h>
#include "ntfscache.h"
#include "ntfswire.h"
#include "ntfspool.h"
typedef ntfs_pool_link link;
typedef struct view {
 HANDLE pipe,parent;ntfs_cache *cache;ntfs_pool pool;link **slots,**ids;uint32_t *done,*dir,slots_count,count,capacity,epoch;
 uint32_t *retire;uint32_t retire_count,retire_capacity;int stopped,paused,failed,initializing;uint32_t control_checks,status_phase,status_error;uint64_t status_generation;int status_valid,polling;uint8_t output[65536];DWORD used;
} view;
static int raw_io(HANDLE h,void *data,DWORD n,int output){while(n){DWORD got=0;if(!(output?WriteFile(h,data,n,&got,NULL):ReadFile(h,data,n,&got,NULL))||!got)return 0;data=(uint8_t*)data+got;n-=got;}return 1;}
static int flush(view *v){if(v->failed)return 0;if(v->used&&!raw_io(v->pipe,v->output,v->used,1)){v->failed=1;return 0;}v->used=0;return 1;}
static int send_record(view *v,sp_record r,const wchar_t *name){if(r.type==SP_STATUS){v->status_phase=r.flags;v->status_error=r.error;v->status_generation=r.allocated;v->status_valid=1;}r.magic=SP_MAGIC;r.chars=name?(uint32_t)wcslen(name):0;DWORD size=sizeof r+r.chars*2;if(size>sizeof v->output){v->failed=1;return 0;}if(v->used+size>sizeof v->output&&!flush(v))return 0;memcpy(v->output+v->used,&r,sizeof r);v->used+=sizeof r;if(r.chars){memcpy(v->output+v->used,name,r.chars*2);v->used+=r.chars*2;}if(r.type==SP_END||r.type==SP_STATUS||r.type==NTN_ERROR)return flush(v);return !v->failed;}
static int alive(void *arg){view *v=arg;if(v->stopped||v->failed||WaitForSingleObject(v->parent,0)!=WAIT_TIMEOUT)return 0;DWORD have=0;if(!PeekNamedPipe(v->pipe,NULL,0,NULL,&have,NULL))return 0;while(have>=sizeof(sp_record)){sp_record r;if(!raw_io(v->pipe,&r,sizeof r,0)||r.magic!=NTN_MAGIC||r.type!=NTN_CONTROL||r.chars||r.reserved||r.error||r.id||r.parent||r.device||r.identity||r.logical||r.allocated){v->stopped=1;return 0;}if(r.flags==NTN_STOP){v->stopped=1;return 0;}if(r.flags==NTN_PAUSE)v->paused=1;else if(r.flags==NTN_RESUME)v->paused=0;else {v->stopped=1;return 0;}if(!PeekNamedPipe(v->pipe,NULL,0,NULL,&have,NULL))return 0;}return 1;}
static void progress(void *arg,unsigned phase){view *v=arg;if(phase==NTFS_LIVE&&!v->count)phase=NTFS_CATCHUP;
 /* Routine polling must not flicker an already live view through CATCHUP. */
 if(v->polling&&phase==NTFS_CATCHUP&&v->status_valid&&v->status_phase==NTFS_LIVE&&!v->status_error)return;
 uint64_t generation=v->cache?ntfs_cache_generation(v->cache):0;if(v->status_valid&&v->status_phase==phase&&!v->status_error&&v->status_generation==generation)return;
 send_record(v,(sp_record){.type=SP_STATUS,.flags=phase,.identity=SP_BACKEND_NTFS,.device=GetCurrentProcessId(),.logical=GetTickCount64(),.allocated=generation},NULL);}

static int reserve(view *v){const mft_model *m=ntfs_cache_model(v->cache);if(m->slots<=v->slots_count)return 1;uint32_t n=m->slots;link **slots=calloc(n,sizeof *slots);uint32_t *done=calloc(n,4),*dir=malloc((size_t)n*4);if(!slots||!done||!dir){free(slots);free(done);free(dir);return 0;}for(uint32_t i=0;i<n;i++)dir[i]=SP_NONE;if(v->slots_count){memcpy(slots,v->slots,v->slots_count*sizeof *slots);memcpy(done,v->done,v->slots_count*4);memcpy(dir,v->dir,v->slots_count*4);}free(v->slots);free(v->done);free(v->dir);v->slots=slots;v->done=done;v->dir=dir;v->slots_count=n;return 1;}
static link *create_link(view *v,uint32_t slot,uint64_t ref,const wchar_t *name){if(v->count>=SP_NTFS_LIMIT)return NULL;if(v->count==v->capacity){uint32_t cap=v->capacity?v->capacity*2:1024;if(cap>SP_NTFS_LIMIT)cap=SP_NTFS_LIMIT;link **p=realloc(v->ids,(size_t)cap*sizeof *p);if(!p)return NULL;v->ids=p;v->capacity=cap;}wchar_t *copy=ntfs_pool_name_new(&v->pool,name);if(!copy)return NULL;link *p=ntfs_pool_link_new(&v->pool);if(!p){ntfs_pool_name_free(&v->pool,copy);return NULL;}p->name=copy;p->id=v->count++;p->slot=slot;p->ref=ref;p->parent=p->first=p->last=p->next=p->previous=SP_NONE;p->slot_next=v->slots[slot];v->slots[slot]=p;v->ids[p->id]=p;return p;}
static void detach(view *v,link *p){if(p->parent==SP_NONE)return;link *parent=v->ids[p->parent];if(p->previous==SP_NONE)parent->first=p->next;else v->ids[p->previous]->next=p->next;if(p->next==SP_NONE)parent->last=p->previous;else v->ids[p->next]->previous=p->previous;p->parent=p->previous=p->next=SP_NONE;}
static void attach(view *v,link *p,uint32_t parent){if(p->parent==parent)return;detach(v,p);link *a=v->ids[parent];p->parent=parent;p->previous=a->last;if(a->last==SP_NONE)a->first=p->id;else v->ids[a->last]->next=p->id;a->last=p->id;}
static int queue_retire(view *v,uint32_t id){if(v->retire_count==v->retire_capacity){uint32_t cap=v->retire_capacity?v->retire_capacity*2:256;uint32_t *p=realloc(v->retire,(size_t)cap*4);if(!p)return 0;v->retire=p;v->retire_capacity=cap;}v->retire[v->retire_count++]=id;return 1;}
static int sync_slot(view *,uint32_t,unsigned);
static uint32_t directory_id(view *v,uint64_t ref,unsigned depth){const mft_model *m=ntfs_cache_model(v->cache);uint64_t slot=ref&MFT_INDEX_MASK;if(slot>=m->slots||m->nodes[slot].reference!=ref||!(m->nodes[slot].flags&MF_DIRECTORY)||m->nodes[slot].attributes&FILE_ATTRIBUTE_REPARSE_POINT||depth>128)return SP_NONE;uint32_t id=v->dir[slot];if(id!=SP_NONE&&!v->ids[id]->gone&&v->ids[id]->ref==ref&&v->done[slot]==v->epoch)return id;if(!sync_slot(v,(uint32_t)slot,depth+1))return SP_NONE;id=v->dir[slot];return id!=SP_NONE&&!v->ids[id]->gone&&v->ids[id]->ref==ref?id:SP_NONE;}
static int sync_slot(view *v,uint32_t slot,unsigned depth){if(depth>128||(!(v->control_checks++&255)&&!alive(v)))return 0;if(v->done[slot]==v->epoch)return 1;v->done[slot]=v->epoch;const mft_model *m=ntfs_cache_model(v->cache);const mft_node *node=m->nodes+slot;if(slot==5)return 1;
 uint32_t count=0,owner=SP_NONE;int ok=1;
 for(uint32_t at=node->first;at!=MFT_NONE;at=m->names[at].next){const mft_name *n=m->names+at;if(!n->valid||!(node->flags&MF_ACTIVE))continue;uint32_t parent=directory_id(v,n->parent,depth);if(parent==SP_NONE)continue;const wchar_t *name=(const wchar_t*)(m->text+n->offset);link *p=NULL;
  for(link *x=v->slots[slot];x;x=x->slot_next)if(!x->gone&&x->ref==node->reference&&x->epoch!=v->epoch&&x->parent==parent&&!wcscmp(x->name,name)){p=x;break;}
  if(!p)for(link *x=v->slots[slot];x;x=x->slot_next)if(!x->gone&&x->ref==node->reference&&x->epoch!=v->epoch){p=x;break;}
  int fresh=p==NULL;if(fresh)p=create_link(v,slot,node->reference,name);if(!p){ok=0;break;}if(wcscmp(p->name,name)){wchar_t *copy=ntfs_pool_name_new(&v->pool,name);if(!copy){ok=0;break;}ntfs_pool_name_free(&v->pool,p->name);p->name=copy;}p->epoch=v->epoch;attach(v,p,parent);if(node->flags&MF_DIRECTORY)v->dir[slot]=p->id;
  count++;if(p->id<owner)owner=p->id;uint32_t flags=(node->flags&MF_DIRECTORY?SP_DIR:0)|(node->attributes&FILE_ATTRIBUTE_REPARSE_POINT?SP_SKIP:0)|(node->flags&MF_BAD?SP_FAILED|SP_ALLOC_UNKNOWN:0);
  if(fresh){uint32_t initial_flags=flags;if(v->initializing&&!(flags&(SP_DIR|SP_SKIP))&&count>1)initial_flags|=SP_HARDLINK;sp_record r={.type=SP_ENTRY,.id=p->id,.parent=parent,.flags=initial_flags,.logical=v->initializing&&!(flags&(SP_DIR|SP_SKIP))?node->logical:0,.allocated=v->initializing&&!(initial_flags&(SP_DIR|SP_SKIP|SP_HARDLINK))?node->allocated:0};if(!send_record(v,r,p->name)){ok=0;break;}if(flags&SP_DIR&&!send_record(v,(sp_record){.type=SP_LEAVE,.id=p->id},NULL)){ok=0;break;}}
 }
 for(link *p=v->slots[slot];!v->initializing&&ok&&p;p=p->slot_next){if(p->gone||p->epoch!=v->epoch)continue;uint32_t flags=(node->flags&MF_DIRECTORY?SP_DIR:0)|(node->attributes&FILE_ATTRIBUTE_REPARSE_POINT?SP_SKIP:0)|(node->flags&MF_BAD?SP_FAILED|SP_ALLOC_UNKNOWN:0);if(!(flags&(SP_DIR|SP_SKIP))&&p->id!=owner)flags|=SP_HARDLINK;
  sp_record r={.type=SP_UPDATE,.id=p->id,.parent=p->parent,.flags=flags,.logical=flags&(SP_DIR|SP_SKIP)?0:node->logical,.allocated=flags&(SP_DIR|SP_SKIP|SP_HARDLINK)?0:node->allocated};if(!send_record(v,r,p->name))ok=0;
 }
 for(link *p=v->slots[slot];ok&&p;p=p->slot_next)if(!p->gone&&p->epoch!=v->epoch)ok=queue_retire(v,p->id);
 return ok;
}
static int retire_tree(view *v,uint32_t id,unsigned depth){link *p=v->ids[id];if(p->gone)return 1;if(!id||depth>128)return 0;while(p->first!=SP_NONE)if(!retire_tree(v,p->first,depth+1))return 0;if(!send_record(v,(sp_record){.type=SP_REMOVE,.id=id},NULL))return 0;detach(v,p);p->gone=1;ntfs_pool_name_free(&v->pool,p->name);p->name=NULL;return 1;}
static void clear_view(view *v){ntfs_pool_clear(&v->pool);free(v->ids);free(v->slots);free(v->done);free(v->dir);free(v->retire);v->ids=v->slots=NULL;v->done=v->dir=v->retire=NULL;v->count=v->capacity=v->slots_count=v->retire_count=v->retire_capacity=0;}
static int publish(view *v,DWORD error){uint32_t n;int reset;const uint32_t *changed=ntfs_cache_changes(v->cache,&n,&reset);if(!reset&&v->count&&!n)return 1;if(reset||!v->count){clear_view(v);if(!reserve(v)||!send_record(v,(sp_record){.type=SP_RESET,.identity=SP_BACKEND_NTFS,.logical=ntfs_cache_model(v->cache)->name_count},NULL))return 0;const mft_model *m=ntfs_cache_model(v->cache);link *root=create_link(v,5,m->nodes[5].reference,L"");if(!root||!send_record(v,(sp_record){.type=SP_ENTRY,.id=0,.parent=SP_NONE,.flags=SP_DIR},L""))return 0;v->dir[5]=0;v->initializing=1;v->epoch=1;v->done[5]=1;for(uint32_t slot=0;slot<m->slots;slot++)if(m->nodes[slot].flags&MF_ACTIVE&&!sync_slot(v,slot,0))return 0;v->initializing=0;}
 else{if(!reserve(v))return 0;if(++v->epoch==0){memset(v->done,0,v->slots_count*4);v->epoch=1;}v->done[5]=v->epoch;v->retire_count=0;for(uint32_t i=0;i<n;i++)if(changed[i]<v->slots_count&&!sync_slot(v,changed[i],0))return 0;}
 for(uint32_t i=0;i<v->retire_count;i++)if(!retire_tree(v,v->retire[i],0))return 0;
 ntfs_cache_ack_changes(v->cache);return send_record(v,(sp_record){.type=SP_END,.flags=error?SP_PARTIAL:SP_COMPLETE,.error=error},NULL);
}
static int same_user(HANDLE parent){HANDLE tokens[2]={0};BYTE users[2][512];DWORD n,sessions[2];int ok=1;HANDLE processes[]={GetCurrentProcess(),parent};for(unsigned i=0;i<2;i++)if(!OpenProcessToken(processes[i],TOKEN_QUERY,&tokens[i])||!GetTokenInformation(tokens[i],TokenUser,users[i],sizeof users[i],&n)||!GetTokenInformation(tokens[i],TokenSessionId,&sessions[i],sizeof sessions[i],&n)){ok=0;break;}if(ok)ok=sessions[0]==sessions[1]&&EqualSid(((TOKEN_USER*)users[0])->User.Sid,((TOKEN_USER*)users[1])->User.Sid);for(unsigned i=0;i<2;i++)if(tokens[i])CloseHandle(tokens[i]);return ok;}
static int number(const wchar_t *s,uint64_t *n,int base){if(!s||!*s||*s=='-'||*s=='+')return 0;wchar_t *end;*n=wcstoull(s,&end,base);return !*end;}
int wmain(int argc,wchar_t **argv){uint64_t pid,born,job_value;if(argc!=6||!number(argv[1],&pid,10)||!pid||pid>MAXDWORD||!number(argv[2],&born,16)||!number(argv[3],&job_value,10)||!job_value||job_value>UINTPTR_MAX||wcslen(argv[4])!=32||wcslen(argv[5])!=1||argv[5][0]<'A'||argv[5][0]>'Z')return 2;for(unsigned i=0;i<32;i++)if(!wcschr(L"0123456789abcdef",argv[4][i]))return 2;
 view v={.pipe=INVALID_HANDLE_VALUE};HANDLE job=NULL,owner=NULL,token=NULL,volume=INVALID_HANDLE_VALUE;int result=3;wchar_t pipe[128],device[]=L"\\\\.\\C:",root[]=L"C:\\",volume_guid[64],folder[32768],path[32768];swprintf(pipe,128,L"%ls%ls",NTN_PIPE_PREFIX,argv[4]);device[4]=root[0]=argv[5][0];
 v.parent=OpenProcess(PROCESS_DUP_HANDLE|PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,(DWORD)pid);FILETIME created,ex,kernel,user;if(!v.parent||!GetProcessTimes(v.parent,&created,&ex,&kernel,&user)||(((uint64_t)created.dwHighDateTime<<32)|created.dwLowDateTime)!=born||!same_user(v.parent))goto done;
 if(!DuplicateHandle(v.parent,(HANDLE)(uintptr_t)job_value,GetCurrentProcess(),&job,0,FALSE,DUPLICATE_SAME_ACCESS)||!AssignProcessToJobObject(job,GetCurrentProcess()))goto done;CloseHandle(job);job=NULL;
 uint64_t until=GetTickCount64()+10000;do{v.pipe=CreateFileW(pipe,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,NULL);if(v.pipe!=INVALID_HANDLE_VALUE)break;Sleep(10);}while(GetTickCount64()<until);ULONG server;if(v.pipe==INVALID_HANDLE_VALUE||!GetNamedPipeServerProcessId(v.pipe,&server)||server!=pid)goto done;
 if(GetDriveTypeW(root)!=DRIVE_FIXED||!GetVolumeNameForVolumeMountPointW(root,volume_guid,64)||wcslen(volume_guid)!=49)goto capability;
 volume=CreateFileW(device,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);if(volume==INVALID_HANDLE_VALUE)goto capability;
 NTFS_VOLUME_DATA_BUFFER geometry;DWORD got;if(!DeviceIoControl(volume,FSCTL_GET_NTFS_VOLUME_DATA,NULL,0,&geometry,sizeof geometry,&got,NULL))goto capability;
 if(!OpenProcessToken(v.parent,TOKEN_QUERY|TOKEN_DUPLICATE,&token)||!DuplicateToken(token,SecurityImpersonation,&owner))goto capability;CloseHandle(token);token=NULL;
 sp_record hello={.magic=NTN_MAGIC,.type=NTN_REQUEST,.flags=NTN_VERSION,.device=(uint64_t)geometry.VolumeSerialNumber.QuadPart};if(!raw_io(v.pipe,&hello,sizeof hello,1))goto done;sp_record request;if(!raw_io(v.pipe,&request,sizeof request,0)||request.magic!=NTN_MAGIC||request.type!=NTN_REQUEST||request.flags!=NTN_VERSION||request.chars<4||request.chars>32000||request.id||request.parent||request.error||request.reserved||request.device||request.identity||request.logical||request.allocated||!raw_io(v.pipe,folder,request.chars*2,0))goto done;folder[request.chars]=0;if(wcslen(folder)!=request.chars||folder[1]!=':'||folder[2]!='\\'||wcschr(folder+2,L':')||swprintf(path,32768,L"%ls\\ntfs-%.36ls-%016llx.v2.bin",folder,volume_guid+11,(unsigned long long)geometry.VolumeSerialNumber.QuadPart)<0)goto done;
 DWORD error=ntfs_cache_open(volume,owner,path,alive,progress,&v,&v.cache);if(error)goto failure;
 error=ntfs_cache_poll(v.cache);if(error&&error!=ERROR_RETRY)goto failure;if(!publish(&v,error))goto done;progress(&v,error?NTFS_INCOMPLETE:NTFS_LIVE);DWORD save_error=ntfs_cache_save(v.cache);if(save_error)send_record(&v,(sp_record){.type=SP_STATUS,.flags=NTFS_INCOMPLETE,.error=save_error,.identity=SP_BACKEND_NTFS},NULL);uint64_t saved=GetTickCount64();
 while(alive(&v)){if(v.paused){progress(&v,NTFS_PAUSED);if(GetTickCount64()-saved>=5000){ntfs_cache_save(v.cache);saved=GetTickCount64();}while(v.paused&&alive(&v))Sleep(100);if(!alive(&v))break;}
  uint64_t next=GetTickCount64()+1000;while(GetTickCount64()<next&&alive(&v)&&!v.paused)Sleep(50);if(v.paused)continue;if(!alive(&v))break;
  v.polling=1;error=ntfs_cache_poll(v.cache);v.polling=0;if(error&&error!=ERROR_RETRY)goto failure;if(!publish(&v,error))goto done;progress(&v,error?NTFS_INCOMPLETE:NTFS_LIVE);if(GetTickCount64()-saved>=30000){DWORD e=ntfs_cache_save(v.cache);if(e)send_record(&v,(sp_record){.type=SP_STATUS,.flags=NTFS_INCOMPLETE,.error=e,.identity=SP_BACKEND_NTFS},NULL);saved=GetTickCount64();}
 }
 result=0;goto done;
capability:error=GetLastError();if(!error)error=ERROR_NOT_SUPPORTED;
failure:send_record(&v,(sp_record){.type=NTN_ERROR,.error=error},NULL);
done:if(v.cache)ntfs_cache_close(v.cache);clear_view(&v);if(owner)CloseHandle(owner);if(token)CloseHandle(token);if(job)CloseHandle(job);if(volume!=INVALID_HANDLE_VALUE)CloseHandle(volume);if(v.pipe!=INVALID_HANDLE_VALUE)CloseHandle(v.pipe);if(v.parent)CloseHandle(v.parent);return result;
}
