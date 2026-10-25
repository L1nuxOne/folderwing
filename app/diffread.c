#include "diff.h"
#include "providerpath.h"
#include <stdlib.h>
#include <string.h>
static int cancelled(HANDLE h){return h&&WaitForSingleObject(h,0)==WAIT_OBJECT_0;}
static DWORD read_chunk(HANDLE f,uint8_t *p,DWORD n,HANDLE cancel){while(n){if(cancelled(cancel))return ERROR_CANCELLED;DWORD got=0;if(!ReadFile(f,p,n,&got,NULL))return GetLastError();if(!got)return ERROR_HANDLE_EOF;p+=got;n-=got;}return 0;}
DWORD diff_files(const wchar_t *left,const wchar_t *right,int wsl,HANDLE cancel,diff_result *r){
 const wchar_t *paths[2]={left,right};HANDLE f[2]={INVALID_HANDLE_VALUE,INVALID_HANDLE_VALUE};BY_HANDLE_FILE_INFORMATION before[2];FILE_BASIC_INFO basic[2];uint8_t *data[2]={0},*chunk[2]={0};DWORD error=0;int same=1;
 for(int i=0;i<2;i++){r->error_side=(uint32_t)i;error=provider_path_check(paths[i],wsl);if(error)goto out;}
 for(int i=0;i<2;i++){r->error_side=(uint32_t)i;
  if(cancelled(cancel)){error=ERROR_CANCELLED;goto out;}
  f[i]=CreateFileW(paths[i],GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN|FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,NULL);
  if(f[i]==INVALID_HANDLE_VALUE){error=GetLastError();goto out;}
  if(!GetFileInformationByHandle(f[i],&before[i])||!GetFileInformationByHandleEx(f[i],FileBasicInfo,&basic[i],sizeof basic[i])){error=GetLastError();goto out;}
  if(GetFileType(f[i])!=FILE_TYPE_DISK||(before[i].dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DEVICE|FILE_ATTRIBUTE_OFFLINE))){error=ERROR_NOT_SUPPORTED;goto out;}
  r->doc[i].bytes=((uint64_t)before[i].nFileSizeHigh<<32)|before[i].nFileSizeLow;
  if(r->doc[i].bytes<=DIFF_TEXT_BYTES){data[i]=malloc((size_t)r->doc[i].bytes+1);if(!data[i]){error=ERROR_NOT_ENOUGH_MEMORY;goto out;}}
  chunk[i]=malloc(262144);if(!chunk[i]){error=ERROR_NOT_ENOUGH_MEMORY;goto out;}
 }
 same=r->doc[0].bytes==r->doc[1].bytes;
 /* Large unequal files need only sizes to prove inequality. Small complete
  * inputs still get decoded. Equal-size files are compared to their full end. */
 for(uint64_t at=0;;at+=262144){int any=0;DWORD got[2]={0};for(int i=0;i<2;i++){
   uint64_t n=r->doc[i].bytes;if(at<n&&(same||data[i])){any=1;got[i]=(DWORD)(n-at>262144?262144:n-at);r->error_side=(uint32_t)i;error=read_chunk(f[i],chunk[i],got[i],cancel);if(error)goto out;if(data[i])memcpy(data[i]+(size_t)at,chunk[i],got[i]);}}
  if(!any)break;if(got[0]!=got[1]||(got[0]&&memcmp(chunk[0],chunk[1],got[0])))same=0;
  /* Once inequality is known we can stop large reads, but never truncate a
   * small text candidate. The next iteration completes any small buffers. */
 }
 for(int i=0;i<2;i++){BY_HANDLE_FILE_INFORMATION after;FILE_BASIC_INFO now;r->error_side=(uint32_t)i;if(!GetFileInformationByHandle(f[i],&after)||!GetFileInformationByHandleEx(f[i],FileBasicInfo,&now,sizeof now)){error=GetLastError();goto out;}
  if(before[i].nFileSizeHigh!=after.nFileSizeHigh||before[i].nFileSizeLow!=after.nFileSizeLow||CompareFileTime(&before[i].ftLastWriteTime,&after.ftLastWriteTime)||basic[i].ChangeTime.QuadPart!=now.ChangeTime.QuadPart){error=ERROR_FILE_INVALID;goto out;}}
 if(cancelled(cancel)){error=ERROR_CANCELLED;goto out;}
 if(!data[0]||!data[1]){r->mode=DIFF_LARGE;r->bytes_equal=same;}
 else error=diff_buffers(data[0],(size_t)r->doc[0].bytes,data[1],(size_t)r->doc[1].bytes,cancel,r);
out:for(int i=0;i<2;i++){if(f[i]!=INVALID_HANDLE_VALUE)CloseHandle(f[i]);free(data[i]);free(chunk[i]);}r->error=error;return error;
}
