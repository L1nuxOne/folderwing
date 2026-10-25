/* Worker-only scanners. File metadata crosses the process boundary; payloads do not. */
#include "space.h"
#include "termutil.h"
#include "../src/namecodec.h"
#include <winternl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef struct scanner {HANDLE cancel;space_emit emit;void *user;uint32_t next,errors;int stopped,mode;DWORD limit_error;uint64_t names;const sp_page_request *page;} scanner;
static int live(scanner *s){if(s->cancel&&WaitForSingleObject(s->cancel,0)==WAIT_OBJECT_0)s->stopped=1;return !s->stopped;}
static int emit(scanner *s,sp_record *r,const wchar_t *name){r->magic=SP_MAGIC;if(r->type==SP_ENTRY){s->next=r->id+1;s->names+=(wcslen(name)+1)*2;if(s->names>(s->mode==1?SP_SUMMARY_NAMES_LIMIT:s->mode==3?SP_ANALYSIS_NAMES_LIMIT:SP_NAMES_LIMIT)){s->limit_error=ERROR_BUFFER_OVERFLOW;s->errors++;s->stopped=1;return 0;}}if(r->flags&SP_FAILED&&r->type==SP_ENTRY)s->errors++;if(!s->emit(s->user,r,name))s->stopped=1;return !s->stopped;}
static HANDLE child_open(HANDLE parent,const wchar_t *name,ACCESS_MASK access){
 typedef NTSTATUS (NTAPI *open_fn)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,PLARGE_INTEGER,ULONG,ULONG,ULONG,ULONG,PVOID,ULONG);
 typedef ULONG (WINAPI *error_fn)(NTSTATUS);
 open_fn open=(open_fn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtCreateFile");error_fn error=(error_fn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlNtStatusToDosError");
 if(!open||!error){SetLastError(ERROR_NOT_SUPPORTED);return INVALID_HANDLE_VALUE;}
 size_t n=wcslen(name)*2;if(!n||n>65534||wcschr(name,L'\\')||wcschr(name,L'/')){SetLastError(ERROR_INVALID_NAME);return INVALID_HANDLE_VALUE;}
 UNICODE_STRING u={(USHORT)n,(USHORT)n,(PWSTR)name};OBJECT_ATTRIBUTES a={.Length=sizeof a,.RootDirectory=parent,.ObjectName=&u};IO_STATUS_BLOCK io;HANDLE h=NULL;
 NTSTATUS status=open(&h,access|SYNCHRONIZE,&a,&io,NULL,0,FILE_SHARE_READ|FILE_SHARE_WRITE,1,0x204020,NULL,0);
 if(status<0){SetLastError(error(status));return INVALID_HANDLE_VALUE;}return h;
}
static void win_walk(scanner *s,HANDLE h,const wchar_t *name,uint32_t parent,unsigned depth){
 if(!live(s)){CloseHandle(h);return;}uint32_t id=s->next++;sp_record r={.type=SP_ENTRY,.id=id,.parent=parent,.flags=SP_DIR};
 if(depth>=128){r.flags|=SP_FAILED;r.error=ERROR_STACK_OVERFLOW;}if(!emit(s,&r,name)||depth>=128){CloseHandle(h);return;}
 BYTE *buf=malloc(65536);if(!buf){r=(sp_record){.type=SP_ENTRY,.id=s->next++,.parent=id,.flags=SP_SKIP|SP_FAILED,.error=ERROR_NOT_ENOUGH_MEMORY};emit(s,&r,L"(enumeration error)");CloseHandle(h);return;}
 while(live(s)&&s->next<SP_LIMIT){
  if(!GetFileInformationByHandleEx(h,FileIdBothDirectoryInfo,buf,65536)){DWORD error=GetLastError();if(error!=ERROR_NO_MORE_FILES){r=(sp_record){.type=SP_ENTRY,.id=s->next++,.parent=id,.flags=SP_SKIP|SP_FAILED,.error=error};emit(s,&r,L"(enumeration error)");}break;}
  DWORD at=0;for(;;){FILE_ID_BOTH_DIR_INFO *e=(void*)(buf+at);DWORD bytes=e->FileNameLength;size_t n=bytes/2;
   if(bytes%2||bytes>65536-at-offsetof(FILE_ID_BOTH_DIR_INFO,FileName)){s->errors++;break;}
   if(!(n==1&&e->FileName[0]==L'.')&&!(n==2&&e->FileName[0]==L'.'&&e->FileName[1]==L'.')){
    if(!live(s)||s->next>=SP_LIMIT)break;wchar_t *component=malloc(bytes+2);if(!component){s->errors++;s->stopped=1;break;}memcpy(component,e->FileName,bytes);component[n]=0;
    r=(sp_record){.type=SP_ENTRY,.id=s->next,.parent=id};HANDLE file=child_open(h,component,FILE_READ_ATTRIBUTES);BY_HANDLE_FILE_INFORMATION info;
    if(file==INVALID_HANDLE_VALUE){r.flags=SP_SKIP|SP_FAILED;r.error=GetLastError();}
    else if(!GetFileInformationByHandle(file,&info)){r.flags=SP_SKIP|SP_FAILED;r.error=GetLastError();}
    else if(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DEVICE))r.flags=SP_SKIP;
    else if(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){
     HANDLE dir=child_open(h,component,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES);BY_HANDLE_FILE_INFORMATION check;
     if(dir==INVALID_HANDLE_VALUE){r.flags=SP_DIR|SP_FAILED;r.error=GetLastError();}
     else if(!GetFileInformationByHandle(dir,&check)){r.flags=SP_DIR|SP_FAILED;r.error=GetLastError();CloseHandle(dir);}
     else if(info.dwVolumeSerialNumber!=check.dwVolumeSerialNumber||info.nFileIndexHigh!=check.nFileIndexHigh||info.nFileIndexLow!=check.nFileIndexLow){r.flags=SP_DIR|SP_FAILED;r.error=ERROR_FILE_INVALID;CloseHandle(dir);}
     else{CloseHandle(file);win_walk(s,dir,component,id,depth+1);free(component);goto next;}
    }else{
     r.logical=((uint64_t)info.nFileSizeHigh<<32)|info.nFileSizeLow;r.device=info.dwVolumeSerialNumber;r.identity=((uint64_t)info.nFileIndexHigh<<32)|info.nFileIndexLow;
     FILE_STANDARD_INFO standard;FILE_COMPRESSION_INFO compression;
     if(info.dwFileAttributes&(FILE_ATTRIBUTE_COMPRESSED|FILE_ATTRIBUTE_SPARSE_FILE)){
      if(GetFileInformationByHandleEx(file,FileCompressionInfo,&compression,sizeof compression)&&compression.CompressedFileSize.QuadPart>=0)r.allocated=compression.CompressedFileSize.QuadPart;
      else r.flags|=SP_ALLOC_UNKNOWN;
     }else if(GetFileInformationByHandleEx(file,FileStandardInfo,&standard,sizeof standard)&&standard.AllocationSize.QuadPart>=0)r.allocated=standard.AllocationSize.QuadPart;
     else r.flags|=SP_ALLOC_UNKNOWN;
    }
    if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);s->next++;int sent=emit(s,&r,component);free(component);if(!sent)break;
   }
next: if(!e->NextEntryOffset)break;if(e->NextEntryOffset<offsetof(FILE_ID_BOTH_DIR_INFO,FileName)+bytes||e->NextEntryOffset>=65536-at){s->errors++;break;}at+=e->NextEntryOffset;
  }
 }
 free(buf);CloseHandle(h);if(live(s)){r=(sp_record){.type=SP_LEAVE,.id=id};emit(s,&r,NULL);}
}
static DWORD read_pipe(scanner *s,HANDLE pipe,HANDLE process,void *data,DWORD n){uint64_t until=GetTickCount64()+60000;
 while(n){if(!live(s))return ERROR_CANCELLED;DWORD have=0,got=0;if(!PeekNamedPipe(pipe,NULL,0,NULL,&have,NULL))return ERROR_BROKEN_PIPE;
  if(!have){if(GetTickCount64()>=until)return ERROR_TIMEOUT;if(WaitForSingleObject(process,0)==WAIT_OBJECT_0)return ERROR_BROKEN_PIPE;if(s->cancel)WaitForSingleObject(s->cancel,5);else Sleep(5);continue;}
  DWORD want=have<n?have:n;if(!ReadFile(pipe,data,want,&got,NULL)||!got)return ERROR_BROKEN_PIPE;data=(char*)data+got;n-=got;until=GetTickCount64()+60000;
 }return 0;}
static DWORD linux_scan(scanner *s,const wchar_t *root,int as_root){
 wchar_t distro[128],path[32768],exe[MAX_PATH],dir[MAX_PATH],cmd[4*MAX_PATH];if(!term_wsl_split(root,distro,128,path,32768))return ERROR_INVALID_NAME;
 uint8_t raw[131072];size_t len=0;/* term_wsl_split returns a literal Linux path. */
 for(size_t i=0;path[i];){uint32_t c=(uint16_t)path[i++];if(c>=0xd800&&c<=0xdbff){uint32_t low=(uint16_t)path[i++];if(low<0xdc00||low>0xdfff)return ERROR_INVALID_NAME;c=0x10000+((c-0xd800)<<10)+low-0xdc00;}
  if(c>=0xdc80&&c<=0xdcff)raw[len++]=(uint8_t)(c-0xdc00);else if(c<128)raw[len++]=(uint8_t)c;else if(c<2048){raw[len++]=0xc0|(c>>6);raw[len++]=0x80|(c&63);}else if(c<65536){raw[len++]=0xe0|(c>>12);raw[len++]=0x80|((c>>6)&63);raw[len++]=0x80|(c&63);}else{raw[len++]=0xf0|(c>>18);raw[len++]=0x80|((c>>12)&63);raw[len++]=0x80|((c>>6)&63);raw[len++]=0x80|(c&63);}}
 if(!len||len>32768)return ERROR_FILENAME_EXCED_RANGE;GetModuleFileNameW(NULL,dir,MAX_PATH);wchar_t *slash=wcsrchr(dir,L'\\');if(!slash)return ERROR_INVALID_NAME;*slash=0;
 DWORD n=GetSystemDirectoryW(exe,MAX_PATH-10);if(!n||n>=MAX_PATH-10)return ERROR_NOT_SUPPORTED;wcscat(exe,L"\\wsl.exe");
 swprintf(cmd,4*MAX_PATH,L"\"%ls\" --distribution %ls%ls --cd \"%ls\" --exec ./folderwing-wsl %ls",exe,distro,as_root?L" --user root":L"",dir,s->mode==1?L"--analyze-space-summary":s->mode==2?L"--analyze-space-list":s->mode==3?L"--analyze-space-full":s->mode==4?L"--analyze-space-page":L"--analyze-space");
 SECURITY_ATTRIBUTES sa={sizeof sa,NULL,TRUE};HANDLE in=NULL,to=NULL,from=NULL,out=NULL,nul=NULL,job=NULL;PROCESS_INFORMATION pi={0};LPPROC_THREAD_ATTRIBUTE_LIST attrs=NULL;int initialized=0;DWORD error=ERROR_NOT_ENOUGH_MEMORY;
 if(!CreatePipe(&in,&to,&sa,65536)||!CreatePipe(&from,&out,&sa,65536))goto done;SetHandleInformation(to,HANDLE_FLAG_INHERIT,0);SetHandleInformation(from,HANDLE_FLAG_INHERIT,0);
 nul=CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,NULL);if(nul==INVALID_HANDLE_VALUE){nul=NULL;goto done;}
 SIZE_T need=0;InitializeProcThreadAttributeList(NULL,1,0,&need);attrs=malloc(need);if(!attrs||!InitializeProcThreadAttributeList(attrs,1,0,&need))goto done;initialized=1;
 HANDLE inherit[]={in,out,nul};if(!UpdateProcThreadAttribute(attrs,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherit,sizeof inherit,NULL,NULL))goto done;
 STARTUPINFOEXW si={{sizeof si}};si.StartupInfo.dwFlags=STARTF_USESTDHANDLES;si.StartupInfo.hStdInput=in;si.StartupInfo.hStdOutput=out;si.StartupInfo.hStdError=nul;si.lpAttributeList=attrs;
 job=CreateJobObjectW(NULL,NULL);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;if(!job||!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof limits))goto done;
 if(!CreateProcessW(exe,cmd,NULL,NULL,TRUE,CREATE_SUSPENDED|CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,NULL,NULL,&si.StartupInfo,&pi)){error=GetLastError();goto done;}
 if(!AssignProcessToJobObject(job,pi.hProcess)){error=GetLastError();TerminateProcess(pi.hProcess,1);goto done;}ResumeThread(pi.hThread);CloseHandle(in);in=NULL;CloseHandle(out);out=NULL;
 uint32_t request[]={SP_MAGIC,SP_VERSION,(uint32_t)len};DWORD wrote;if(!WriteFile(to,request,sizeof request,&wrote,NULL)||wrote!=sizeof request||!WriteFile(to,raw,(DWORD)len,&wrote,NULL)||wrote!=len){error=ERROR_BROKEN_PIPE;goto done;}
 if(s->mode==4&&(!WriteFile(to,s->page,sizeof *s->page,&wrote,NULL)||wrote!=sizeof *s->page)){error=ERROR_BROKEN_PIPE;goto done;}
 for(;;){sp_record r;error=read_pipe(s,from,pi.hProcess,&r,sizeof r);if(error)break;
  if(r.magic!=SP_MAGIC||(r.type!=SP_ENTRY&&r.type!=SP_LEAVE&&r.type!=SP_END&&!(s->mode==4&&r.type==SP_PAGE)&&!(s->mode==1&&r.type==SP_TOTAL)&&!((s->mode==1||s->mode==3)&&r.type==SP_RESET))||r.chars>4095||(r.type!=SP_ENTRY&&r.chars)){error=ERROR_INVALID_DATA;break;}
  wchar_t name[8192]=L"";if(r.chars){uint8_t bytes[4096];error=read_pipe(s,from,pi.hProcess,bytes,r.chars);if(error)break;size_t units;if(!fpe_lx_from_bytes(bytes,r.chars,name,8192,1,0,&units)){error=ERROR_INVALID_DATA;break;}}
  if(r.type==SP_RESET&&r.identity!=(uint64_t)(s->mode==1?SP_BACKEND_SUMMARY:SP_BACKEND_ANALYSIS)){error=ERROR_INVALID_DATA;break;}
  if(s->mode==4&&r.type==SP_END&&r.error==116)r.error=ERROR_FILE_INVALID;else if(s->mode==4&&r.type==SP_END&&(r.error==22||r.error==34))r.error=ERROR_INVALID_PARAMETER;
  if(r.type==SP_END&&r.error==75)r.error=ERROR_BUFFER_OVERFLOW;else if(r.type==SP_END&&r.error==12)r.error=ERROR_NOT_ENOUGH_MEMORY;
  if(!emit(s,&r,r.type==SP_ENTRY?name:NULL)){error=s->limit_error?s->limit_error:ERROR_CANCELLED;break;}
  if(r.type==SP_END){error=0;break;}
 }
done:/* END is already delivered. Let the owned launcher drain its WSL session
       * before closing the kill-on-close job; cancellation still kills promptly. */
 if(!error&&pi.hProcess){if(to){CloseHandle(to);to=NULL;}WaitForSingleObject(pi.hProcess,250);}
 if(initialized)DeleteProcThreadAttributeList(attrs);free(attrs);if(job)CloseHandle(job);HANDLE handles[]={in,to,from,out,nul,pi.hThread,pi.hProcess};for(size_t i=0;i<sizeof handles/sizeof *handles;i++)if(handles[i])CloseHandle(handles[i]);return error;
}
static DWORD scan_mode(const wchar_t *root,int wsl_enabled,int as_root,HANDLE cancel,space_emit callback,void *user,int mode,const sp_page_request *page){
 scanner s={.cancel=cancel,.emit=callback,.user=user,.mode=mode,.page=page};DWORD error=0;if(!root||!*root||!callback)return ERROR_INVALID_PARAMETER;
 wchar_t d[128],p[32768];int native=term_wsl_split(root,d,128,p,32768);
 if(native){if(!wsl_enabled)error=ERROR_NOT_SUPPORTED;else error=linux_scan(&s,root,as_root);if(!error)return 0;}
 else if(mode==4)error=ERROR_NOT_SUPPORTED;
 else if(!_wcsnicmp(root,L"\\\\wsl",5))error=ERROR_NOT_SUPPORTED;
 else{size_t n=wcslen(root);wchar_t *path=calloc(n+8,sizeof *path);if(!path)error=ERROR_NOT_ENOUGH_MEMORY;
  else{if(root[0]&&root[1]==L':')swprintf(path,n+8,L"\\\\?\\%ls",root);else wcscpy(path,root);
   HANDLE h=CreateFileW(path,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);BY_HANDLE_FILE_INFORMATION info;
   if(h==INVALID_HANDLE_VALUE)error=GetLastError();else if(!GetFileInformationByHandle(h,&info)){error=GetLastError();CloseHandle(h);}else if(!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT){error=ERROR_CANT_ACCESS_FILE;CloseHandle(h);}else win_walk(&s,h,L"",SP_NONE,0);free(path);}}
 if(error&&!s.next){sp_record r={.type=SP_ENTRY,.id=0,.parent=SP_NONE,.flags=SP_DIR|SP_FAILED,.error=error};emit(&s,&r,L"");}
 if(!error)error=s.limit_error;sp_record end={.magic=SP_MAGIC,.type=SP_END,.flags=s.stopped?SP_STOPPED:error||s.errors||s.next>=SP_LIMIT?SP_PARTIAL:SP_COMPLETE,.error=error?error:s.next>=SP_LIMIT?ERROR_BUFFER_OVERFLOW:0};callback(user,&end,NULL);return error;
}

DWORD space_scan(const wchar_t *root,int wsl_enabled,int as_root,HANDLE cancel,space_emit callback,void *user){return space_scan_mode(root,wsl_enabled,as_root,cancel,callback,user,0);}

DWORD space_scan_mode(const wchar_t *root,int wsl_enabled,int as_root,HANDLE cancel,space_emit callback,void *user,int mode){if(mode<0||mode>3)return ERROR_INVALID_PARAMETER;return scan_mode(root,wsl_enabled,as_root,cancel,callback,user,mode,NULL);}
DWORD space_scan_page(const wchar_t *root,int wsl_enabled,int as_root,HANDLE cancel,space_emit callback,void *user,const sp_page_request *page){if(!page||page->mtime_ns>=1000000000||page->ctime_ns>=1000000000)return ERROR_INVALID_PARAMETER;return scan_mode(root,wsl_enabled,as_root,cancel,callback,user,4,page);}
