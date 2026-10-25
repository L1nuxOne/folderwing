/* One owned worker; one replaceable request/result. The UI never reads archives or waits on helpers. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "archiveclient.h"
#include "previewlaunch.h"
#include "fpe.h"
#include "providerpath.h"
typedef struct request {LONG generation;uint32_t action,index;wchar_t *path,*folder;} request;
struct arc_client {CRITICAL_SECTION lock;LONG refs,stop,generation;HANDLE wake;HWND hwnd;UINT message;request *pending;arc_result *ready;};
typedef struct helper {HANDLE to,from,process,job,section;preview_launch *launch;uint32_t backend;} helper;
/* Count both cleanup tasks and their worker producers. A stopped client may
 * release its ready extraction only as its worker leaves; counting producers
 * prevents a false empty queue during final shutdown. */
static SRWLOCK archive_work_lock=SRWLOCK_INIT;
static CONDITION_VARIABLE archive_work_idle=CONDITION_VARIABLE_INIT;
static unsigned archive_work;
static void work_begin(void){AcquireSRWLockExclusive(&archive_work_lock);archive_work++;ReleaseSRWLockExclusive(&archive_work_lock);}
static void work_end(void){AcquireSRWLockExclusive(&archive_work_lock);if(!--archive_work)WakeAllConditionVariable(&archive_work_idle);ReleaseSRWLockExclusive(&archive_work_lock);}
void arc_cleanup_drain(void){AcquireSRWLockExclusive(&archive_work_lock);while(archive_work)SleepConditionVariableSRW(&archive_work_idle,&archive_work_lock,INFINITE,0);ReleaseSRWLockExclusive(&archive_work_lock);}
static DWORD WINAPI file_delete(void *arg){arc_file *f=arg;if(f->lock)CloseHandle(f->lock);if(f->file_created)DeleteFileW(f->path);if(f->anchor)CloseHandle(f->anchor);if(f->directory_created)RemoveDirectoryW(f->directory);free(f);work_end();return 0;}
void arc_file_hold(arc_file *f){if(f)InterlockedIncrement(&f->refs);}
void arc_file_release(void *arg){arc_file *f=arg;if(f&&!InterlockedDecrement(&f->refs)){work_begin();if(!QueueUserWorkItem(file_delete,f,WT_EXECUTEDEFAULT)){if(f->lock)CloseHandle(f->lock);if(f->anchor)CloseHandle(f->anchor);free(f);work_end();/* Keep owned data for review if cleanup cannot be scheduled. */}}}
void arc_result_free(arc_result *r){if(r){arc_rows_free(r->rows,r->count);arc_file_release(r->file);free(r);}}
static void request_free(request *r){if(r){free(r->path);free(r->folder);free(r);}}
static void release(arc_client *c){if(!InterlockedDecrement(&c->refs)){request_free(c->pending);arc_result_free(c->ready);CloseHandle(c->wake);DeleteCriticalSection(&c->lock);free(c);}}
static int cancelled(arc_client *c,LONG gen){return InterlockedCompareExchange(&c->stop,0,0)||InterlockedCompareExchange(&c->generation,0,0)!=gen;}
static void helper_close(helper *h){preview_launch_cancel(&h->launch);if(h->job)TerminateJobObject(h->job,1);else if(h->process)TerminateProcess(h->process,1);HANDLE *a[]={&h->to,&h->from,&h->process,&h->job,&h->section};for(size_t i=0;i<sizeof a/sizeof *a;i++)if(*a[i])CloseHandle(*a[i]);memset(h,0,sizeof *h);}
static DWORD helper_start(arc_client *c,LONG gen,helper *h,uint32_t backend){
 SECURITY_ATTRIBUTES sa={sizeof sa,NULL,TRUE};HANDLE in=NULL,out=NULL;DWORD error=0;
 h->section=CreateFileMappingW(INVALID_HANDLE_VALUE,&sa,PAGE_READWRITE,0,4096,NULL);
 if(!h->section||!CreatePipe(&in,&h->to,&sa,65536)||!CreatePipe(&h->from,&out,&sa,65536)){error=GetLastError();goto done;}
 if(!SetHandleInformation(h->to,HANDLE_FLAG_INHERIT,0)||!SetHandleInformation(h->from,HANDLE_FLAG_INHERIT,0)){error=GetLastError();goto done;}
 h->launch=preview_launch_begin(backend==1?L"folderwing-zip.exe":L"folderwing-archive.exe",in,out,h->section,(SIZE_T)512<<20,c->hwnd,c->message+1,(uint32_t)gen);
 if(!h->launch){error=ERROR_NOT_ENOUGH_MEMORY;goto done;}
 uint64_t until=GetTickCount64()+15000;int state;DWORD pid;
 while(!(state=preview_launch_take(&h->launch,&h->process,&h->job,&pid))){if(cancelled(c,gen)){error=ERROR_CANCELLED;goto done;}if(GetTickCount64()>=until){error=ERROR_TIMEOUT;goto done;}WaitForSingleObject(c->wake,5);}
 if(state<0)error=ERROR_ACCESS_DENIED;else h->backend=backend;
 done:if(in)CloseHandle(in);if(out)CloseHandle(out);if(error)helper_close(h);return error;
}
static DWORD read_bytes(arc_client *c,LONG gen,helper *h,void *data,DWORD bytes){char *p=data;uint64_t until=GetTickCount64()+30000;
 while(bytes){if(cancelled(c,gen))return ERROR_CANCELLED;DWORD available=0,n=0;if(!PeekNamedPipe(h->from,NULL,0,NULL,&available,NULL))return ERROR_BROKEN_PIPE;
  if(!available){if(GetTickCount64()>=until)return ERROR_TIMEOUT;if(WaitForSingleObject(h->process,0)==WAIT_OBJECT_0)return ERROR_BROKEN_PIPE;WaitForSingleObject(c->wake,5);continue;}
  DWORD take=bytes<available?bytes:available;if(!ReadFile(h->from,p,take,&n,NULL)||!n)return ERROR_BROKEN_PIPE;p+=n;bytes-=n;until=GetTickCount64()+30000;
 }return 0;
}
static DWORD send_request(helper *h,uint32_t type,const void *data,uint32_t bytes){arcp_head a={ARCP_MAGIC,ARCP_VERSION,type,bytes};DWORD n;if(!WriteFile(h->to,&a,sizeof a,&n,NULL)||n!=sizeof a)return ERROR_BROKEN_PIPE;if(bytes&&(!WriteFile(h->to,data,bytes,&n,NULL)||n!=bytes))return ERROR_BROKEN_PIPE;return 0;}
static DWORD read_head(arc_client *c,LONG gen,helper *h,arcp_head *head){DWORD err=read_bytes(c,gen,h,head,sizeof *head);if(!err&&(head->magic!=ARCP_MAGIC||head->version!=ARCP_VERSION||head->size>ARCP_CHUNK+sizeof(arcp_entry)))err=ERROR_INVALID_DATA;return err;}
static DWORD list(arc_client *c,LONG gen,helper *h,const wchar_t *path,arc_entry **out,uint32_t *count,arcp_id *id){
 *out=NULL;*count=0;uint32_t capacity=0;uint64_t names=0;int info_seen=0;DWORD info_error=0;DWORD error=send_request(h,ARCP_OPEN,path,((uint32_t)wcslen(path)+1)*2);if(error)return error;
 for(;;){arcp_head head;error=read_head(c,gen,h,&head);if(error)break;
  if(head.type==ARCP_INFO&&head.size==sizeof(arcp_info)&&!info_seen){arcp_info info;error=read_bytes(c,gen,h,&info,sizeof info);if(error)break;info_seen=1;info_error=info.error;*id=info.id;if(info.backend!=h->backend){error=ERROR_INVALID_DATA;break;}}
  else if(head.type==ARCP_ENTRY&&info_seen&&!info_error&&head.size>=sizeof(arcp_entry)){
   arcp_entry e;error=read_bytes(c,gen,h,&e,sizeof e);if(error)break;
   if(e.index!=*count||*count>=ARCP_ENTRIES||!e.chars||e.chars>=ARCP_NAME||e.flags&~63u||head.size!=sizeof e+e.chars*2||(names+=e.chars*2)>ARCP_NAMES_BYTES){error=ERROR_INVALID_DATA;break;}
   if(*count==capacity){uint32_t next=capacity?capacity*2:256;if(next>ARCP_ENTRIES)next=ARCP_ENTRIES;arc_entry *a=realloc(*out,next*sizeof **out);if(!a){error=ERROR_NOT_ENOUGH_MEMORY;break;}*out=a;capacity=next;}
   wchar_t *name=calloc((size_t)e.chars+1,sizeof *name);if(!name){error=ERROR_NOT_ENOUGH_MEMORY;break;}error=read_bytes(c,gen,h,name,e.chars*2);
   if(error||wcslen(name)!=e.chars){free(name);if(!error)error=ERROR_INVALID_DATA;break;}
   if(!arc_name_safe(name))e.flags|=ARCP_UNSAFE;(*out)[*count]=(arc_entry){e,name};(*count)++;
  }else if(head.type==ARCP_LIST_END&&head.size==sizeof(arcp_end)&&info_seen){arcp_end end;error=read_bytes(c,gen,h,&end,sizeof end);if(!error)error=info_error?info_error:end.error;if(!error&&end.count!=*count)error=ERROR_INVALID_DATA;break;}
  else {error=ERROR_INVALID_DATA;break;}
 }
 if(error){arc_entries_free(*out,*count);*out=NULL;*count=0;}return error;
}
static int prefer_zip(const wchar_t *path){const wchar_t *e=wcsrchr(path,L'.');if(e&&(!_wcsicmp(e,L".zip")||!_wcsicmp(e,L".zipx")||!_wcsicmp(e,L".jar")||!_wcsicmp(e,L".cbz")||!_wcsicmp(e,L".whl")||!_wcsicmp(e,L".apk")||!_wcsicmp(e,L".nupkg")))return 1;
 HANDLE f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);unsigned char sig[4];DWORD n=0;int zip=f!=INVALID_HANDLE_VALUE&&ReadFile(f,sig,4,&n,NULL)&&n==4&&sig[0]=='P'&&sig[1]=='K';if(f!=INVALID_HANDLE_VALUE)CloseHandle(f);return zip;}
static arc_file *new_file(const wchar_t *name,HANDLE *out){SetLastError(ERROR_NOT_ENOUGH_MEMORY);
 arc_file *f=calloc(1,sizeof *f);if(!f)return NULL;f->refs=1;wchar_t temp[ARCP_NAME],guid[64];GUID id;
 DWORD n=GetTempPathW(ARCP_NAME,temp);if(!n||n>ARCP_NAME-100||FAILED(CoCreateGuid(&id))||!StringFromGUID2(&id,guid,64))goto fail;
 swprintf(f->directory,ARCP_NAME,L"%lsOpenPilot-archive-%ls",temp,guid);if(!CreateDirectoryW(f->directory,NULL))goto fail;f->directory_created=1;
 f->anchor=CreateFileW(f->directory,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
 if(f->anchor==INVALID_HANDLE_VALUE){f->anchor=NULL;goto fail;}FILE_ATTRIBUTE_TAG_INFO tag;if(!GetFileInformationByHandleEx(f->anchor,FileAttributeTagInfo,&tag,sizeof tag)||(tag.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))goto fail;
 const wchar_t *leaf=wcsrchr(name,L'/');leaf=leaf?leaf+1:name;wchar_t safe[260];
 if(arc_leaf_safe(leaf))wcscpy(safe,leaf);else {const wchar_t *ext=wcsrchr(leaf,L'.');int valid=ext&&wcslen(ext)<=16;for(const wchar_t *p=valid?ext+1:L"";*p;p++)if(!((*p>=L'a'&&*p<=L'z')||(*p>=L'A'&&*p<=L'Z')||(*p>=L'0'&&*p<=L'9')))valid=0;swprintf(safe,260,L"entry%ls",valid?ext:L".bin");}
 if(wcslen(f->directory)+wcslen(safe)+2>=ARCP_NAME)goto fail;swprintf(f->path,ARCP_NAME,L"%ls%ls\\%ls",f->directory[1]==L':'?L"\\\\?\\":L"",f->directory,safe);
 *out=CreateFileW(f->path,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY,NULL);if(*out==INVALID_HANDLE_VALUE)goto fail;f->file_created=1;return f;
 fail:arc_file_release(f);return NULL;
}
static DWORD extract(arc_client *c,LONG gen,helper *h,const arc_entry *entry,uint64_t limit,arc_file **out){
 *out=NULL;if(entry->info.flags&(ARCP_DIR|ARCP_SPECIAL|ARCP_UNSAFE|ARCP_UNSUPPORTED))return ERROR_NOT_SUPPORTED;
 if(entry->info.flags&ARCP_ENCRYPTED)return ERROR_PASSWORD_RESTRICTION;
 if(!(entry->info.flags&ARCP_UNKNOWN_SIZE)&&entry->info.size>limit)return ERROR_FILE_TOO_LARGE;
 HANDLE file;arc_file *f=new_file(entry->name,&file);if(!f)return GetLastError()?GetLastError():ERROR_NOT_ENOUGH_MEMORY;
 arcp_read req={entry->info.index,0,limit};DWORD error=send_request(h,ARCP_READ,&req,sizeof req);uint64_t bytes=0;
 while(!error){arcp_head head;error=read_head(c,gen,h,&head);if(error)break;
  if(head.type==ARCP_DATA&&head.size&&head.size<=ARCP_CHUNK){char buffer[ARCP_CHUNK];error=read_bytes(c,gen,h,buffer,head.size);if(error)break;
   if(bytes+head.size>limit){error=ERROR_FILE_TOO_LARGE;break;}DWORD n;if(!WriteFile(file,buffer,head.size,&n,NULL)||n!=head.size){error=ERROR_WRITE_FAULT;break;}bytes+=head.size;
  }else if(head.type==ARCP_READ_END&&head.size==sizeof(arcp_end)){arcp_end end;error=read_bytes(c,gen,h,&end,sizeof end);if(!error)error=end.error;if(!error&&(end.count!=1||bytes!=end.bytes||(!(entry->info.flags&ARCP_UNKNOWN_SIZE)&&bytes!=entry->info.size)))error=ERROR_INVALID_DATA;break;}
  else {error=ERROR_INVALID_DATA;break;}
 }
 CloseHandle(file);if(!error){f->lock=CreateFileW(f->path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);if(f->lock==INVALID_HANDLE_VALUE){f->lock=NULL;error=GetLastError();}}if(error||cancelled(c,gen)){arc_file_release(f);return error?error:ERROR_CANCELLED;}*out=f;return 0;
}
static DWORD WINAPI worker(void *arg){arc_client *c=arg;helper h={0};arc_entry *entries=NULL;uint32_t count=0;wchar_t *path=NULL;arcp_id id={0};uint32_t backend=0;
 for(;;){EnterCriticalSection(&c->lock);request *r=c->pending;c->pending=NULL;LeaveCriticalSection(&c->lock);
  if(InterlockedCompareExchange(&c->stop,0,0)){request_free(r);break;}
  if(!r){if(WaitForSingleObject(c->wake,30000)==WAIT_TIMEOUT)helper_close(&h);continue;}
  arc_result *result=calloc(1,sizeof *result);if(!result){request_free(r);continue;}result->generation=r->generation;result->action=r->action;
  result->error=provider_path_check(r->path,1);
  if(!result->error&&r->action==ARC_OPEN){helper_close(&h);arc_entries_free(entries,count);entries=NULL;count=0;free(path);path=_wcsdup(r->path);backend=prefer_zip(r->path)?1:2;
   DWORD err=path?helper_start(c,r->generation,&h,backend):ERROR_NOT_ENOUGH_MEMORY;
   if(!err)err=list(c,r->generation,&h,path,&entries,&count,&id);
   if(err&&err!=ERROR_CANCELLED&&backend==1){helper_close(&h);backend=2;err=helper_start(c,r->generation,&h,backend);if(!err)err=list(c,r->generation,&h,path,&entries,&count,&id);}
   result->error=err;
  }
  if(!result->error&&(!path||wcscmp(path,r->path)))result->error=ERROR_INVALID_STATE;
  if(!result->error&&(r->action==ARC_OPEN||r->action==ARC_FOLDER)){
   if(!arc_rows_build(entries,count,r->folder,&result->rows,&result->count))result->error=ERROR_NOT_ENOUGH_MEMORY;
  }else if(!result->error){
   if(r->index>=count)result->error=ERROR_INVALID_PARAMETER;
   else {
    if(!h.process){arc_entry *check=NULL;uint32_t n=0;arcp_id fresh;result->error=helper_start(c,r->generation,&h,backend);if(!result->error)result->error=list(c,r->generation,&h,path,&check,&n,&fresh);if(!result->error&&(n!=count||memcmp(&fresh,&id,sizeof id)))result->error=ERROR_FILE_INVALID;arc_entries_free(check,n);}
    if(!result->error)result->error=extract(c,r->generation,&h,&entries[r->index],r->action==ARC_EXTRACT?ARCP_EXTRACT_BYTES:ARCP_PREVIEW_BYTES,&result->file);
   }
  }
  result->backend=backend;
  if(result->error)helper_close(&h);
  if(!cancelled(c,r->generation)){
   EnterCriticalSection(&c->lock);arc_result *old=c->ready;c->ready=result;result=NULL;if(c->hwnd)PostMessageW(c->hwnd,c->message,0,0);LeaveCriticalSection(&c->lock);arc_result_free(old);
  }
  arc_result_free(result);request_free(r);
 }
 helper_close(&h);arc_entries_free(entries,count);free(path);release(c);work_end();return 0;
}
arc_client *arc_client_start(HWND hwnd,UINT message){arc_client *c=calloc(1,sizeof *c);if(!c)return NULL;InitializeCriticalSection(&c->lock);c->refs=2;c->hwnd=hwnd;c->message=message;c->wake=CreateEventW(NULL,FALSE,FALSE,NULL);if(!c->wake){c->refs=1;release(c);return NULL;}work_begin();HANDLE thread=CreateThread(NULL,0,worker,c,0,NULL);if(!thread){c->refs=1;release(c);work_end();return NULL;}CloseHandle(thread);return c;}
LONG arc_client_cancel(arc_client *c){if(!c)return 0;EnterCriticalSection(&c->lock);LONG gen=InterlockedIncrement(&c->generation);request *old=c->pending;c->pending=NULL;SetEvent(c->wake);LeaveCriticalSection(&c->lock);request_free(old);return gen;}
void arc_client_stop(arc_client **slot){arc_client *c=*slot;*slot=NULL;if(!c)return;EnterCriticalSection(&c->lock);c->hwnd=NULL;InterlockedExchange(&c->stop,1);SetEvent(c->wake);LeaveCriticalSection(&c->lock);release(c);}
LONG arc_client_request(arc_client *c,uint32_t action,const wchar_t *path,const wchar_t *folder,uint32_t index){if(!c)return 0;request *r=calloc(1,sizeof *r);if(!r)return 0;r->path=_wcsdup(path);r->folder=_wcsdup(folder?folder:L"");if(!r->path||!r->folder){request_free(r);return 0;}r->action=action;r->index=index;
 EnterCriticalSection(&c->lock);r->generation=InterlockedIncrement(&c->generation);request *old=c->pending;c->pending=r;SetEvent(c->wake);LONG gen=r->generation;LeaveCriticalSection(&c->lock);request_free(old);return gen;}
arc_result *arc_client_take(arc_client *c){if(!c)return NULL;EnterCriticalSection(&c->lock);arc_result *r=c->ready;c->ready=NULL;LeaveCriticalSection(&c->lock);return r;}
