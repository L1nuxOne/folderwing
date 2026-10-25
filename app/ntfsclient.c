/* Unelevated panel-side owner of an exact disposable NTFS worker. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <sddl.h>
#include <stdint.h>
#include <wchar.h>
#include <stdlib.h>
#include <wctype.h>
#include "space.h"
#include "ntfswire.h"
typedef struct channel {HANDLE pipe,process,job,cancel,visible,read_event;uint8_t data[65536];DWORD at,used;uint64_t deadline;int paused,initialized;} channel;
int space_ntfs_root(const wchar_t *root){if(!root||wcslen(root)!=3||root[1]!=':'||root[2]!='\\'||!((root[0]>='A'&&root[0]<='Z')||(root[0]>='a'&&root[0]<='z')))return 0;wchar_t fs[32],guid[64];return GetDriveTypeW(root)==DRIVE_FIXED&&GetVolumeNameForVolumeMountPointW(root,guid,64)&&GetVolumeInformationW(root,NULL,0,NULL,NULL,NULL,fs,32)&&!_wcsicmp(fs,L"NTFS");}
static int cancelled(channel *c){return c->cancel&&WaitForSingleObject(c->cancel,0)==WAIT_OBJECT_0;}
static DWORD finish(channel *c,OVERLAPPED *o,DWORD need){DWORD got=0,error=0;for(;;){DWORD w=WaitForSingleObject(o->hEvent,20);if(w==WAIT_OBJECT_0){if(!GetOverlappedResult(c->pipe,o,&got,FALSE))error=GetLastError();else if(got!=need)error=ERROR_INVALID_DATA;break;}if(cancelled(c)){error=ERROR_CANCELLED;break;}if(c->process&&WaitForSingleObject(c->process,0)==WAIT_OBJECT_0){error=ERROR_BROKEN_PIPE;break;}if(GetTickCount64()>c->deadline){error=ERROR_TIMEOUT;break;}}if(error){CancelIoEx(c->pipe,o);GetOverlappedResult(c->pipe,o,&got,TRUE);}return error;}
static DWORD write_bytes(channel *c,const void *p,DWORD n){OVERLAPPED o={.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL)};if(!o.hEvent)return GetLastError();DWORD got=0,error=0;if(!WriteFile(c->pipe,p,n,&got,&o)){error=GetLastError();if(error==ERROR_IO_PENDING)error=finish(c,&o,n);}else if(got!=n)error=ERROR_INVALID_DATA;CloseHandle(o.hEvent);return error;}
static DWORD control(channel *c,uint32_t command){sp_record r={.magic=NTN_MAGIC,.type=NTN_CONTROL,.flags=command};return write_bytes(c,&r,sizeof r);}
/* One pending read owns its buffer/event until completion or cancellation is
 * drained. Visibility uses a bounded timeout because its manual event stays
 * signaled while shown; cancellation and worker exit wake the wait directly. */
static DWORD visibility(channel *c){int paused=c->visible&&WaitForSingleObject(c->visible,0)!=WAIT_OBJECT_0;if(c->initialized&&paused!=c->paused){c->deadline=GetTickCount64()+NTN_TIMEOUT;DWORD e=control(c,paused?NTN_PAUSE:NTN_RESUME);if(e)return e;c->paused=paused;}return 0;}
static DWORD read_chunk(channel *c){
 if(cancelled(c))return ERROR_CANCELLED;DWORD error=visibility(c);if(error)return error;
 if(!c->read_event)c->read_event=CreateEventW(NULL,TRUE,FALSE,NULL);if(!c->read_event)return GetLastError();ResetEvent(c->read_event);
 OVERLAPPED o={.hEvent=c->read_event};DWORD got=0;
 if(!ReadFile(c->pipe,c->data,sizeof c->data,&got,&o)){
  error=GetLastError();if(error!=ERROR_IO_PENDING)return error;
  HANDLE waits[3]={c->read_event};DWORD count=1,cancel_at=MAXDWORD,process_at=MAXDWORD;
  if(c->cancel){cancel_at=count;waits[count++]=c->cancel;}if(c->process){process_at=count;waits[count++]=c->process;}
  for(;;){
   if(cancelled(c)){error=ERROR_CANCELLED;break;}error=visibility(c);if(error)break;
   DWORD wait=WaitForMultipleObjects(count,waits,FALSE,50);
   if(wait==WAIT_OBJECT_0){if(!GetOverlappedResult(c->pipe,&o,&got,FALSE))return GetLastError();error=0;break;}
   if(cancel_at!=MAXDWORD&&wait==WAIT_OBJECT_0+cancel_at){error=ERROR_CANCELLED;break;}
   if(process_at!=MAXDWORD&&wait==WAIT_OBJECT_0+process_at){error=ERROR_BROKEN_PIPE;break;}
   if(wait==WAIT_FAILED){error=GetLastError();break;}
   if(!c->paused&&GetTickCount64()>c->deadline){error=ERROR_TIMEOUT;break;}
  }
  if(error){CancelIoEx(c->pipe,&o);GetOverlappedResult(c->pipe,&o,&got,TRUE);return error;}
 }
 if(!got)return ERROR_BROKEN_PIPE;c->at=0;c->used=got;c->deadline=GetTickCount64()+NTN_TIMEOUT;return 0;
}
static DWORD receive(channel *c,void *output,DWORD size){uint8_t *p=output;while(size){if(c->at==c->used){DWORD e=read_chunk(c);if(e)return e;}DWORD n=c->used-c->at;if(n>size)n=size;memcpy(p,c->data+c->at,n);c->at+=n;p+=n;size-=n;}return 0;}
static void close_channel(channel *c){if(c->pipe&&c->pipe!=INVALID_HANDLE_VALUE){CancelIoEx(c->pipe,NULL);CloseHandle(c->pipe);}if(c->read_event)CloseHandle(c->read_event);if(c->job)CloseHandle(c->job);if(c->process)CloseHandle(c->process);}
static DWORD connect_worker(channel *c,wchar_t drive){
 BYTE random[16];typedef BOOLEAN(WINAPI *random_fn)(PVOID,ULONG);HMODULE dll=LoadLibraryW(L"advapi32.dll");random_fn gen=dll?(random_fn)(void*)GetProcAddress(dll,"SystemFunction036"):NULL;int generated=gen&&gen(random,sizeof random);if(dll)FreeLibrary(dll);if(!generated)return ERROR_GEN_FAILURE;
 wchar_t nonce[33],name[128],params[260],exe[32768];for(unsigned i=0;i<16;i++)swprintf(nonce+i*2,3,L"%02x",random[i]);swprintf(name,128,L"%ls%ls",NTN_PIPE_PREFIX,nonce);
 HANDLE token=NULL;BYTE user[512];DWORD n;LPWSTR sid=NULL;PSECURITY_DESCRIPTOR security=NULL;DWORD error=0;
 if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return GetLastError();if(!GetTokenInformation(token,TokenUser,user,sizeof user,&n)||!ConvertSidToStringSidW(((TOKEN_USER*)user)->User.Sid,&sid))error=GetLastError();CloseHandle(token);if(error)return error;
 wchar_t acl[512];swprintf(acl,512,L"D:P(A;;GA;;;SY)(A;;GA;;;%ls)",sid);LocalFree(sid);if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl,SDDL_REVISION_1,&security,NULL))return GetLastError();SECURITY_ATTRIBUTES sa={sizeof sa,security,FALSE};
 c->pipe=CreateNamedPipeW(name,PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,65536,1024*1024,0,&sa);LocalFree(security);if(c->pipe==INVALID_HANDLE_VALUE)return GetLastError();
 c->job=CreateJobObjectW(NULL,NULL);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;if(!c->job||!SetInformationJobObject(c->job,JobObjectExtendedLimitInformation,&limits,sizeof limits))return GetLastError();
 n=GetModuleFileNameW(NULL,exe,32768);wchar_t *slash=n&&n<32768?wcsrchr(exe,L'\\'):NULL;if(!slash||(size_t)(slash-exe)>32740)return ERROR_FILENAME_EXCED_RANGE;wcscpy(slash+1,L"folderwing-ntfs.exe");if(GetFileAttributesW(exe)==INVALID_FILE_ATTRIBUTES)return GetLastError();FILETIME born,ex,kernel,user_time;if(!GetProcessTimes(GetCurrentProcess(),&born,&ex,&kernel,&user_time))return GetLastError();
 swprintf(params,260,L"%lu %llx %llu %ls %lc",GetCurrentProcessId(),(unsigned long long)(((uint64_t)born.dwHighDateTime<<32)|born.dwLowDateTime),(unsigned long long)(uintptr_t)c->job,nonce,drive);
 SHELLEXECUTEINFOW launch={.cbSize=sizeof launch,.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_NOASYNC|SEE_MASK_FLAG_NO_UI,.lpVerb=L"runas",.lpFile=exe,.lpParameters=params,.nShow=SW_HIDE};if(!ShellExecuteExW(&launch))return GetLastError();c->process=launch.hProcess;if(!c->process)return ERROR_INVALID_HANDLE;
 OVERLAPPED o={.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL)};if(!o.hEvent)return GetLastError();if(!ConnectNamedPipe(c->pipe,&o)){error=GetLastError();if(error==ERROR_IO_PENDING)error=finish(c,&o,0);else if(error==ERROR_PIPE_CONNECTED)error=0;}CloseHandle(o.hEvent);if(error)return error;ULONG pid=0;if(!GetNamedPipeClientProcessId(c->pipe,&pid)||pid!=GetProcessId(c->process))return ERROR_ACCESS_DENIED;return 0;
}
/* Flush a partial batch before any receive that could wait. The current pipe
 * buffer can end inside either a header or a name; only complete frames batch. */
static DWORD stream_records(channel *c,space_emit_batch callback,void *user){
 sp_batch_item *items=malloc(SP_NTFS_BATCH*sizeof *items);if(!items)return ERROR_NOT_ENOUGH_MEMORY;
 DWORD error=0;uint32_t count=0;
 for(;;){
  if(count){DWORD available=c->used-c->at;int flush=available<sizeof(sp_record);
   if(!flush){sp_record next;memcpy(&next,c->data+c->at,sizeof next);flush=next.chars>255||available<sizeof next+next.chars*2;}
   if(flush){if(cancelled(c)||!callback(user,items,count)){error=ERROR_CANCELLED;break;}count=0;}
  }
  sp_batch_item *item=items+count;sp_record *r=&item->record;error=receive(c,r,sizeof *r);if(error)break;
  if(r->magic!=SP_MAGIC||r->chars>255||(r->type!=SP_ENTRY&&r->type!=SP_UPDATE&&r->chars)||(r->type!=SP_ENTRY&&r->type!=SP_UPDATE&&r->type!=SP_LEAVE&&r->type!=SP_END&&r->type!=SP_RESET&&r->type!=SP_REMOVE&&r->type!=SP_STATUS&&r->type!=NTN_ERROR)){error=ERROR_INVALID_DATA;break;}
  if(r->type==NTN_ERROR){error=r->error?r->error:ERROR_INVALID_DATA;break;}
  item->name[r->chars]=0;if(r->chars){error=receive(c,item->name,r->chars*2);if(error)break;if(wcslen(item->name)!=r->chars){error=ERROR_INVALID_DATA;break;}}
  count++;if(count==SP_NTFS_BATCH||r->type==SP_STATUS||r->type==SP_RESET||r->type==SP_END){if(cancelled(c)||!callback(user,items,count)){error=ERROR_CANCELLED;break;}count=0;}
 }
 if(count&&error!=ERROR_CANCELLED&&!cancelled(c)&&!callback(user,items,count))error=ERROR_CANCELLED;
 free(items);return error;
}
DWORD space_ntfs_scan_batched(const wchar_t *root,HANDLE cancel,HANDLE visible,space_emit_batch callback,void *user){
 if(!space_ntfs_root(root)||!callback)return ERROR_NOT_SUPPORTED;channel c={.pipe=INVALID_HANDLE_VALUE,.cancel=cancel,.visible=visible,.deadline=GetTickCount64()+NTN_TIMEOUT};if(cancelled(&c))return ERROR_CANCELLED;
 DWORD error=connect_worker(&c,(wchar_t)towupper(root[0]));if(error)goto done;sp_record hello;error=receive(&c,&hello,sizeof hello);if(error)goto done;if(hello.magic==SP_MAGIC&&hello.type==NTN_ERROR){error=hello.error?hello.error:ERROR_NOT_SUPPORTED;goto done;}if(hello.magic!=NTN_MAGIC||hello.type!=NTN_REQUEST||hello.flags!=NTN_VERSION||hello.chars){error=ERROR_INVALID_DATA;goto done;}
 wchar_t folder[32768],base[32000];DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",base,32000);if(!n||n>=32000||base[1]!=L':'||base[2]!=L'\\'||swprintf(folder,32768,L"%ls\\OpenPilot\\SpaceCache\\v2",base)<0){error=ERROR_INVALID_NAME;goto done;}int e=SHCreateDirectoryExW(NULL,folder,NULL);if(e!=ERROR_SUCCESS&&e!=ERROR_ALREADY_EXISTS&&e!=ERROR_FILE_EXISTS){error=(DWORD)e;goto done;}
 sp_record request={.magic=NTN_MAGIC,.type=NTN_REQUEST,.flags=NTN_VERSION,.chars=(uint32_t)wcslen(folder)};error=write_bytes(&c,&request,sizeof request);if(!error)error=write_bytes(&c,folder,request.chars*2);if(error)goto done;c.initialized=1;
 error=stream_records(&c,callback,user);
 done:close_channel(&c);return error;
}
typedef struct single_delivery {space_emit callback;void *user;HANDLE cancel;} single_delivery;
static int emit_single(void *arg,const sp_batch_item *items,uint32_t count){single_delivery *s=arg;for(uint32_t i=0;i<count;i++){const sp_record *r=&items[i].record;if((s->cancel&&WaitForSingleObject(s->cancel,0)==WAIT_OBJECT_0)||!s->callback(s->user,r,r->type==SP_ENTRY||r->type==SP_UPDATE?items[i].name:NULL))return 0;}return 1;}
DWORD space_ntfs_scan(const wchar_t *root,HANDLE cancel,HANDLE visible,space_emit callback,void *user){if(!callback)return ERROR_NOT_SUPPORTED;single_delivery s={callback,user,cancel};return space_ntfs_scan_batched(root,cancel,visible,emit_single,&s);}
