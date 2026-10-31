#include "docmarks.h"
#include "fpe.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#define DM_BYTES (8u<<20)
typedef struct dmrow {docmark mark;wchar_t *path;} dmrow;
typedef struct dmhead {char magic[8];uint32_t count,bytes;uint64_t checksum;} dmhead;
typedef struct dmitem {uint64_t id;uint32_t page;float y;uint32_t path_chars,title_chars;} dmitem;
static uint64_t dmhash(const unsigned char *p,size_t n){uint64_t h=UINT64_C(14695981039346656037);while(n--){h^=*p++;h*=UINT64_C(1099511628211);}return h;}
static void dmfree(dmrow *v,uint32_t n){for(uint32_t i=0;i<n;i++)free(v[i].path);free(v);}
static DWORD dmread(const wchar_t *file,dmrow **out,uint32_t *count){
 *out=NULL;*count=0;HANDLE f=CreateFileW(file,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
 if(f==INVALID_HANDLE_VALUE){DWORD e=GetLastError();return e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND?0:e;}
 LARGE_INTEGER size;DWORD e=ERROR_INVALID_DATA,got=0;unsigned char *buf=NULL;dmrow *v=NULL;uint32_t used=0;
 if(!GetFileSizeEx(f,&size)){e=GetLastError();goto done;}
 if(size.QuadPart<(LONGLONG)sizeof(dmhead)||size.QuadPart>DM_BYTES)goto done;
 buf=malloc((size_t)size.QuadPart);if(!buf){e=ERROR_NOT_ENOUGH_MEMORY;goto done;}
 if(!ReadFile(f,buf,(DWORD)size.QuadPart,&got,NULL)||got!=(DWORD)size.QuadPart){e=ERROR_READ_FAULT;goto done;}
 dmhead h;memcpy(&h,buf,sizeof h);
 if(memcmp(h.magic,"FWMARK01",8)||h.count>DOCMARK_STORE_MAX||h.bytes!=got-sizeof h||h.checksum!=dmhash(buf+sizeof h,h.bytes))goto done;
 v=calloc(h.count+1,sizeof *v);if(!v){e=ERROR_NOT_ENOUGH_MEMORY;goto done;}
 size_t at=sizeof h;
 for(uint32_t i=0;i<h.count;i++){
  if(got-at<sizeof(dmitem))goto done;dmitem d;memcpy(&d,buf+at,sizeof d);at+=sizeof d;
  if(!d.id||!isfinite(d.y)||d.y<0||d.y>1e6f||!d.path_chars||d.path_chars>32767||d.title_chars>=DOCMARK_TITLE)goto done;
  size_t bytes=((size_t)d.path_chars+d.title_chars)*sizeof(wchar_t);if(bytes>got-at)goto done;
  wchar_t *path=malloc(((size_t)d.path_chars+1)*sizeof *path);if(!path){e=ERROR_NOT_ENOUGH_MEMORY;goto done;}
  memcpy(path,buf+at,d.path_chars*sizeof *path);path[d.path_chars]=0;at+=d.path_chars*sizeof *path;
  if(wmemchr(path,0,d.path_chars)){free(path);goto done;}
  docmark mark={.id=d.id,.page=d.page,.y=d.y};memcpy(mark.title,buf+at,d.title_chars*sizeof(wchar_t));at+=d.title_chars*sizeof(wchar_t);
  if(wmemchr(mark.title,0,d.title_chars)){free(path);goto done;}
  for(uint32_t k=0;k<used;k++)if(v[k].mark.id==mark.id){free(path);goto done;}
  v[used].path=path;v[used++].mark=mark;
 }
 if(at!=got)goto done;*out=v;*count=used;v=NULL;e=0;
 done:CloseHandle(f);free(buf);dmfree(v,v?used:0);return e;
}
static DWORD dmwrite(const wchar_t *file,const dmrow *v,uint32_t n){
 size_t bytes=sizeof(dmhead);for(uint32_t i=0;i<n;i++)bytes+=sizeof(dmitem)+(wcslen(v[i].path)+wcslen(v[i].mark.title))*sizeof(wchar_t);
 if(bytes>DM_BYTES)return ERROR_FILE_TOO_LARGE;unsigned char *buf=calloc(1,bytes);if(!buf)return ERROR_NOT_ENOUGH_MEMORY;
 size_t at=sizeof(dmhead);for(uint32_t i=0;i<n;i++){const docmark *m=&v[i].mark;dmitem d={.id=m->id,.page=m->page,.y=m->y,.path_chars=(uint32_t)wcslen(v[i].path),.title_chars=(uint32_t)wcslen(m->title)};memcpy(buf+at,&d,sizeof d);at+=sizeof d;memcpy(buf+at,v[i].path,d.path_chars*sizeof(wchar_t));at+=d.path_chars*sizeof(wchar_t);memcpy(buf+at,m->title,d.title_chars*sizeof(wchar_t));at+=d.title_chars*sizeof(wchar_t);}
 dmhead h={.magic={'F','W','M','A','R','K','0','1'},.count=n,.bytes=(uint32_t)(bytes-sizeof h),.checksum=dmhash(buf+sizeof h,bytes-sizeof h)};memcpy(buf,&h,sizeof h);
 size_t cap=wcslen(file)+80;wchar_t *tmp=malloc(cap*sizeof *tmp);if(!tmp){free(buf);return ERROR_NOT_ENOUGH_MEMORY;}
 swprintf(tmp,cap,L"%ls.%lu.%llu.tmp",file,GetCurrentProcessId(),(unsigned long long)GetTickCount64());
 HANDLE f=CreateFileW(tmp,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);DWORD e=0;
 if(f==INVALID_HANDLE_VALUE)e=GetLastError();else{DWORD wrote=0;if(!WriteFile(f,buf,(DWORD)bytes,&wrote,NULL)||wrote!=bytes)e=ERROR_WRITE_FAULT;else if(!FlushFileBuffers(f))e=GetLastError();CloseHandle(f);if(!e&&!MoveFileExW(tmp,file,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))e=GetLastError();if(e)DeleteFileW(tmp);}
 free(tmp);free(buf);return e;
}
DWORD docmarks_load(const wchar_t *file,const wchar_t *doc,docmark *out,uint32_t cap,uint32_t *count){
 if(!count)return ERROR_INVALID_PARAMETER;*count=0;if(!file||!*file||!doc||(!out&&cap))return ERROR_INVALID_PARAMETER;
 dmrow *v=NULL;uint32_t n=0;DWORD e=dmread(file,&v,&n);if(e)return e;
 for(uint32_t i=0;i<n;i++)if(fpe_path_equal(v[i].path,doc)){if(*count==cap){e=ERROR_INSUFFICIENT_BUFFER;break;}out[(*count)++]=v[i].mark;}
 dmfree(v,n);return e;
}
DWORD docmarks_change(const wchar_t *file,const wchar_t *doc,docmark *mark,int remove){
 if(!file||!*file||!doc||!*doc||wcslen(doc)>32767||!mark||!isfinite(mark->y)||mark->y<0||mark->y>1e6f||!wmemchr(mark->title,0,DOCMARK_TITLE))return ERROR_INVALID_PARAMETER;
 size_t cap=wcslen(file)+8;wchar_t *lockpath=malloc(cap*sizeof *lockpath);if(!lockpath)return ERROR_NOT_ENOUGH_MEMORY;swprintf(lockpath,cap,L"%ls.lock",file);
 HANDLE lock=CreateFileW(lockpath,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);free(lockpath);if(lock==INVALID_HANDLE_VALUE)return GetLastError();
 dmrow *v=NULL;uint32_t n=0;DWORD e=dmread(file,&v,&n);if(e)goto done;
 if(remove){uint32_t i=0;while(i<n&&!(v[i].mark.id==mark->id&&fpe_path_equal(v[i].path,doc)))i++;if(i==n)goto done;free(v[i].path);memmove(v+i,v+i+1,(n-i-1)*sizeof *v);n--;}
 else{
  uint32_t per=0;for(uint32_t i=0;i<n;i++)if(fpe_path_equal(v[i].path,doc))per++;
  if(n>=DOCMARK_STORE_MAX||per>=DOCMARK_PER_FILE){e=ERROR_TOO_MANY_NAMES;goto done;}
  for(uint32_t i=0;i<n;i++)if(fpe_path_equal(v[i].path,doc)&&v[i].mark.page==mark->page&&fabsf(v[i].mark.y-mark->y)<1){mark->id=v[i].mark.id;goto done;}
  if(!mark->id){FILETIME time;GetSystemTimeAsFileTime(&time);mark->id=(((uint64_t)time.dwHighDateTime<<32)|time.dwLowDateTime)^((uint64_t)GetCurrentProcessId()<<32)^GetTickCount64();if(!mark->id)mark->id=1;
   for(uint32_t i=0;i<n;){if(v[i].mark.id==mark->id){if(!++mark->id)mark->id=1;i=0;}else i++;}}
  else for(uint32_t i=0;i<n;i++)if(v[i].mark.id==mark->id){e=ERROR_ALREADY_EXISTS;goto done;}
  dmrow *next=realloc(v,(n+1)*sizeof *v);if(!next){e=ERROR_NOT_ENOUGH_MEMORY;goto done;}v=next;v[n].path=_wcsdup(doc);if(!v[n].path){e=ERROR_NOT_ENOUGH_MEMORY;goto done;}v[n++].mark=*mark;
 }
 e=dmwrite(file,v,n);
 done:dmfree(v,n);CloseHandle(lock);return e;
}

wchar_t *docmarks_location(const wchar_t *reference,const wchar_t *leaf){
 if(!leaf||!*leaf||wcschr(leaf,L'\\')||wcschr(leaf,L'/'))return NULL;
 wchar_t base[32768]={0};if(reference&&wcslen(reference)<32700){wcscpy(base,reference);wchar_t *slash=wcsrchr(base,L'\\');if(slash)slash[1]=0;else base[0]=0;}
 if(!base[0]){DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",base,32600);if(!n||n>=32600)return NULL;wcscat(base,L"\\Folderwing");CreateDirectoryW(base,NULL);wcscat(base,L"\\");}
 size_t n=wcslen(base),m=wcslen(leaf);if(n+m>=32768)return NULL;wchar_t *out=malloc((n+m+1)*sizeof *out);if(out){memcpy(out,base,n*sizeof *out);memcpy(out+n,leaf,(m+1)*sizeof *out);}return out;
}

wchar_t *docmarks_identity(const wchar_t *document,const wchar_t *scope){
 if(!document||!*document||!scope)return NULL;size_t n=wcslen(document),m=wcslen(scope);if(n+1+m*4>=32768)return NULL;
 wchar_t *out=malloc((n+2+m*4)*sizeof *out);if(!out)return NULL;memcpy(out,document,n*sizeof *out);out[n++]=0x1f;const wchar_t hex[]=L"0123456789ABCDEF";
 for(size_t i=0;i<m;i++){unsigned value=(uint16_t)scope[i];for(int j=0;j<4;j++)out[n+i*4+j]=hex[(value>>(12-j*4))&15];}out[n+m*4]=0;return out;
}
