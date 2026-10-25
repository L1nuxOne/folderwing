/* Worker-owned content traversal. No links followed and no destination writes. */
#include "content.h"
#include "termutil.h"
#include "../src/namecodec.h"
#include <winternl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef struct cscan {HANDLE cancel;content_emit emit;void *user;const cf_options *options;cf_summary sum;int stopped,limited;uint64_t notified;wchar_t relative[32768];} cscan;
static int live(cscan *s){if(s->cancel&&WaitForSingleObject(s->cancel,0)==WAIT_OBJECT_0)s->stopped=1;return !s->stopped&&!s->limited;}
static int progress(cscan *s){if(!live(s))return 0;uint64_t now=GetTickCount64();if(now-s->notified>=50){s->notified=now;cf_record r={.magic=CF_MAGIC,.type=CF_PROGRESS,.summary=s->sum};if(!s->emit(s->user,&r,NULL))s->stopped=1;}return live(s);}
static void failed(cscan *s,DWORD error){s->sum.errors++;if(!s->sum.error)s->sum.error=error?error:ERROR_GEN_FAILURE;}
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
static void read_file(cscan *s,HANDLE file,const BY_HANDLE_FILE_INFORMATION *info){
 uint64_t size=((uint64_t)info->nFileSizeHigh<<32)|info->nFileSizeLow;
 if(size>CF_FILE_BYTES){s->sum.large++;return;}if(size>CF_TOTAL_BYTES-s->sum.bytes){s->limited=1;return;}
 FILE_BASIC_INFO before,after;BY_HANDLE_FILE_INFORMATION check;
 if(!GetFileInformationByHandleEx(file,FileBasicInfo,&before,sizeof before)){failed(s,GetLastError());return;}
 unsigned char *data=malloc((size_t)size+1);if(!data){failed(s,ERROR_NOT_ENOUGH_MEMORY);return;}
 size_t at=0;DWORD error=0;while(at<size&&progress(s)){DWORD n=0,want=(DWORD)(size-at>65536?65536:size-at);if(!ReadFile(file,data+at,want,&n,NULL)||!n){error=GetLastError();if(!error)error=ERROR_FILE_INVALID;break;}at+=n;s->sum.bytes+=n;}
 if(!live(s)){free(data);return;}if(error){free(data);failed(s,error);return;}
 cf_hit *hits=NULL;uint32_t count=0;int more=0;int text=content_match_bytes(data,at,s->options,CF_RESULTS-(uint32_t)s->sum.matches,&hits,&count,&more);free(data);
 if(text<0){failed(s,ERROR_NOT_ENOUGH_MEMORY);return;}
 if(!GetFileInformationByHandle(file,&check)||!GetFileInformationByHandleEx(file,FileBasicInfo,&after,sizeof after)){free(hits);failed(s,GetLastError());return;}
 if(info->nFileSizeHigh!=check.nFileSizeHigh||info->nFileSizeLow!=check.nFileSizeLow||before.LastWriteTime.QuadPart!=after.LastWriteTime.QuadPart||before.ChangeTime.QuadPart!=after.ChangeTime.QuadPart){free(hits);failed(s,ERROR_FILE_INVALID);return;}
 s->sum.files++;if(!text){s->sum.binary++;free(hits);return;}
 for(uint32_t i=0;i<count&&live(s);i++){cf_record r={.magic=CF_MAGIC,.type=CF_MATCH,.bytes=size,.mtime=((uint64_t)info->ftLastWriteTime.dwHighDateTime<<32)|info->ftLastWriteTime.dwLowDateTime,.hit=hits[i]};if(!s->emit(s->user,&r,s->relative)){s->stopped=1;break;}s->sum.matches++;}
 free(hits);if(more)s->limited=1;
}
static void walk(cscan *s,HANDLE directory,size_t length,unsigned depth){
 if(!progress(s)){CloseHandle(directory);return;}if(depth>=128){failed(s,ERROR_STACK_OVERFLOW);CloseHandle(directory);return;}
 BYTE *buffer=malloc(65536);if(!buffer){failed(s,ERROR_NOT_ENOUGH_MEMORY);CloseHandle(directory);return;}
 while(progress(s)){
  if(!GetFileInformationByHandleEx(directory,FileIdBothDirectoryInfo,buffer,65536)){DWORD error=GetLastError();if(error!=ERROR_NO_MORE_FILES)failed(s,error);break;}
  DWORD at=0;for(;;){FILE_ID_BOTH_DIR_INFO *entry=(void*)(buffer+at);DWORD bytes=entry->FileNameLength;size_t n=bytes/2;
   if(bytes%2||bytes>65536-at-offsetof(FILE_ID_BOTH_DIR_INFO,FileName)){failed(s,ERROR_INVALID_DATA);break;}
   if(!(n==1&&entry->FileName[0]=='.')&&!(n==2&&entry->FileName[0]=='.'&&entry->FileName[1]=='.')){
    if(!progress(s))break;if(s->sum.scanned>=CF_FILES){s->limited=1;break;}s->sum.scanned++;
    wchar_t *name=malloc(bytes+2);if(!name){failed(s,ERROR_NOT_ENOUGH_MEMORY);s->limited=1;break;}memcpy(name,entry->FileName,bytes);name[n]=0;
    size_t sep=length?1:0;if(length+sep+n>=32768){failed(s,ERROR_FILENAME_EXCED_RANGE);free(name);goto next;}
    if(sep)s->relative[length]='\\';memcpy(s->relative+length+sep,name,bytes+2);
    if(entry->FileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DEVICE|FILE_ATTRIBUTE_OFFLINE)){s->sum.excluded++;free(name);goto restore;}
    if(entry->FileAttributes&FILE_ATTRIBUTE_DIRECTORY){if(s->options->flags&CF_RECURSE){HANDLE child=child_open(directory,name,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES);BY_HANDLE_FILE_INFORMATION actual;if(child==INVALID_HANDLE_VALUE)failed(s,GetLastError());else if(!GetFileInformationByHandle(child,&actual)){failed(s,GetLastError());CloseHandle(child);}else if(actual.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT){s->sum.excluded++;CloseHandle(child);}else if(!(actual.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)){failed(s,ERROR_FILE_INVALID);CloseHandle(child);}else walk(s,child,length+sep+n,depth+1);}else s->sum.excluded++;}
    else if(content_name_matches(name,s->options->pattern)){
     HANDLE file=child_open(directory,name,FILE_READ_DATA|FILE_READ_ATTRIBUTES);BY_HANDLE_FILE_INFORMATION info;
     if(file==INVALID_HANDLE_VALUE)failed(s,GetLastError());else{if(!GetFileInformationByHandle(file,&info))failed(s,GetLastError());else if(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DEVICE|FILE_ATTRIBUTE_DIRECTORY))s->sum.excluded++;else read_file(s,file,&info);CloseHandle(file);}
    }else s->sum.excluded++;free(name);
restore:s->relative[length]=0;
   }
next:if(!live(s)||!entry->NextEntryOffset)break;if(entry->NextEntryOffset<offsetof(FILE_ID_BOTH_DIR_INFO,FileName)+bytes||entry->NextEntryOffset>=65536-at){failed(s,ERROR_INVALID_DATA);break;}at+=entry->NextEntryOffset;
  }
 }
 free(buffer);CloseHandle(directory);
}
static DWORD read_pipe(cscan *s,HANDLE pipe,HANDLE process,void *data,DWORD n){uint64_t until=GetTickCount64()+60000;
 while(n){if(!live(s))return ERROR_CANCELLED;DWORD have=0,got=0;if(!PeekNamedPipe(pipe,NULL,0,NULL,&have,NULL))return ERROR_BROKEN_PIPE;
  if(!have){if(GetTickCount64()>=until)return ERROR_TIMEOUT;if(WaitForSingleObject(process,0)==WAIT_OBJECT_0)return ERROR_BROKEN_PIPE;if(s->cancel)WaitForSingleObject(s->cancel,5);else Sleep(5);continue;}
  DWORD want=have<n?have:n;if(!ReadFile(pipe,data,want,&got,NULL)||!got)return ERROR_BROKEN_PIPE;data=(char*)data+got;n-=got;until=GetTickCount64()+60000;
 }return 0;}
static DWORD linux_scan(cscan *s,const wchar_t *root,int as_root){
 wchar_t distro[128],path[32768],exe[MAX_PATH],dir[MAX_PATH],cmd[4*MAX_PATH];if(!term_wsl_split(root,distro,128,path,32768))return ERROR_INVALID_NAME;
 uint8_t raw[131072];size_t len=0;/* term_wsl_split returns a literal Linux path. */
 for(size_t i=0;path[i];){uint32_t c=(uint16_t)path[i++];if(c>=0xd800&&c<=0xdbff){uint32_t low=(uint16_t)path[i++];if(low<0xdc00||low>0xdfff)return ERROR_INVALID_NAME;c=0x10000+((c-0xd800)<<10)+low-0xdc00;}
  if(c>=0xdc80&&c<=0xdcff)raw[len++]=(uint8_t)(c-0xdc00);else if(c<128)raw[len++]=(uint8_t)c;else if(c<2048){raw[len++]=0xc0|(c>>6);raw[len++]=0x80|(c&63);}else if(c<65536){raw[len++]=0xe0|(c>>12);raw[len++]=0x80|((c>>6)&63);raw[len++]=0x80|(c&63);}else{raw[len++]=0xf0|(c>>18);raw[len++]=0x80|((c>>12)&63);raw[len++]=0x80|((c>>6)&63);raw[len++]=0x80|(c&63);}}
 if(!len||len>32768)return ERROR_FILENAME_EXCED_RANGE;GetModuleFileNameW(NULL,dir,MAX_PATH);wchar_t *slash=wcsrchr(dir,L'\\');if(!slash)return ERROR_INVALID_NAME;*slash=0;
 DWORD n=GetSystemDirectoryW(exe,MAX_PATH-10);if(!n||n>=MAX_PATH-10)return ERROR_NOT_SUPPORTED;wcscat(exe,L"\\wsl.exe");
 swprintf(cmd,4*MAX_PATH,L"\"%ls\" --distribution %ls%ls --cd \"%ls\" --exec ./folderwing-wsl --content-search",exe,distro,as_root?L" --user root":L"",dir);
 SECURITY_ATTRIBUTES sa={sizeof sa,NULL,TRUE};HANDLE in=NULL,to=NULL,from=NULL,out=NULL,nul=NULL,job=NULL;PROCESS_INFORMATION pi={0};LPPROC_THREAD_ATTRIBUTE_LIST attrs=NULL;int initialized=0;DWORD error=ERROR_NOT_ENOUGH_MEMORY;
 if(!CreatePipe(&in,&to,&sa,65536)||!CreatePipe(&from,&out,&sa,65536))goto done;SetHandleInformation(to,HANDLE_FLAG_INHERIT,0);SetHandleInformation(from,HANDLE_FLAG_INHERIT,0);
 nul=CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,NULL);if(nul==INVALID_HANDLE_VALUE){nul=NULL;goto done;}
 SIZE_T need=0;InitializeProcThreadAttributeList(NULL,1,0,&need);attrs=malloc(need);if(!attrs||!InitializeProcThreadAttributeList(attrs,1,0,&need))goto done;initialized=1;
 HANDLE inherit[]={in,out,nul};if(!UpdateProcThreadAttribute(attrs,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherit,sizeof inherit,NULL,NULL))goto done;
 STARTUPINFOEXW si={{sizeof si}};si.StartupInfo.dwFlags=STARTF_USESTDHANDLES;si.StartupInfo.hStdInput=in;si.StartupInfo.hStdOutput=out;si.StartupInfo.hStdError=nul;si.lpAttributeList=attrs;
 job=CreateJobObjectW(NULL,NULL);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;if(!job||!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof limits))goto done;
 if(!CreateProcessW(exe,cmd,NULL,NULL,TRUE,CREATE_SUSPENDED|CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,NULL,NULL,&si.StartupInfo,&pi)){error=GetLastError();goto done;}
 if(!AssignProcessToJobObject(job,pi.hProcess)){error=GetLastError();TerminateProcess(pi.hProcess,1);goto done;}ResumeThread(pi.hThread);CloseHandle(in);in=NULL;CloseHandle(out);out=NULL;
 uint32_t request[]={CF_MAGIC,CF_VERSION,(uint32_t)len};DWORD wrote;
 if(!WriteFile(to,request,sizeof request,&wrote,NULL)||wrote!=sizeof request||!WriteFile(to,s->options,sizeof *s->options,&wrote,NULL)||wrote!=sizeof *s->options||!WriteFile(to,raw,(DWORD)len,&wrote,NULL)||wrote!=len){error=ERROR_BROKEN_PIPE;goto done;}
 for(;;){cf_record record;error=read_pipe(s,from,pi.hProcess,&record,sizeof record);if(error)break;
  if(record.magic!=CF_MAGIC||record.path_chars>=32768||(record.type!=CF_MATCH&&record.path_chars)){error=ERROR_INVALID_DATA;break;}
  wchar_t *relative=NULL;if(record.path_chars){relative=calloc((size_t)record.path_chars+1,2);if(!relative){error=ERROR_NOT_ENOUGH_MEMORY;break;}error=read_pipe(s,from,pi.hProcess,relative,record.path_chars*2);if(error){free(relative);break;}if(wcslen(relative)!=record.path_chars||!content_relative_safe(relative)){free(relative);error=ERROR_INVALID_DATA;break;}}
  int accepted=s->emit(s->user,&record,relative);free(relative);if(!accepted){error=ERROR_CANCELLED;break;}
  if(record.type==CF_END){error=0;break;}
 }

done:if(initialized)DeleteProcThreadAttributeList(attrs);free(attrs);if(job)CloseHandle(job);HANDLE handles[]={in,to,from,out,nul,pi.hThread,pi.hProcess};for(size_t i=0;i<sizeof handles/sizeof *handles;i++)if(handles[i])CloseHandle(handles[i]);return error;
}
DWORD content_scan(const wchar_t *root,const cf_options *options,int wsl,int as_root,HANDLE cancel,content_emit callback,void *user){
 if(!root||!*root||!content_options_valid(options)||!callback)return ERROR_INVALID_PARAMETER;
 cscan *s=calloc(1,sizeof *s);if(!s)return ERROR_NOT_ENOUGH_MEMORY;s->cancel=cancel;s->emit=callback;s->user=user;s->options=options;DWORD error=0;
 wchar_t distro[128],linux[32768];int native=term_wsl_split(root,distro,128,linux,32768);
 if(native){if(wsl)error=linux_scan(s,root,as_root);else error=ERROR_NOT_SUPPORTED;if(!error){free(s);return 0;}}
 else if(!_wcsnicmp(root,L"\\\\wsl",5))error=ERROR_NOT_SUPPORTED;
 else{size_t n=wcslen(root);wchar_t *path=calloc(n+8,2);if(!path)error=ERROR_NOT_ENOUGH_MEMORY;else{
  if(root[0]&&root[1]==':')swprintf(path,n+8,L"\\\\?\\%ls",root);else wcscpy(path,root);
  HANDLE h=CreateFileW(path,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);BY_HANDLE_FILE_INFORMATION info;
  if(h==INVALID_HANDLE_VALUE)error=GetLastError();else if(!GetFileInformationByHandle(h,&info)){error=GetLastError();CloseHandle(h);}else if(!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT){error=ERROR_CANT_ACCESS_FILE;CloseHandle(h);}else walk(s,h,0,0);free(path);}}
 if(error)failed(s,error);s->sum.state=s->stopped?CF_STOPPED:error||s->sum.errors||s->sum.large||s->limited?CF_PARTIAL:CF_COMPLETE;
 cf_record end={.magic=CF_MAGIC,.type=CF_END,.summary=s->sum};callback(user,&end,NULL);free(s);return error;
}
