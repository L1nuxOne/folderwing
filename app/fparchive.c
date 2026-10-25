#include "providerpath.h"
/* Archive parsers live only in these restricted, on-demand helpers. No destination paths. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>
#include "archiveutil.h"
#ifdef ARCP_MINIZ
#include "miniz.h"
#else
#define LIBARCHIVE_STATIC
#include "archive.h"
#include "archive_entry.h"
#endif
static HANDLE input,output,file=INVALID_HANDLE_VALUE;
static arcp_id identity;
static uint32_t entry_count;
static uint64_t sent,limit;
static DWORD stream_error;
static int io(HANDLE h,void *data,DWORD n,int write){char *p=data;while(n){DWORD got=0;if(!(write?WriteFile(h,p,n,&got,NULL):ReadFile(h,p,n,&got,NULL))||!got)return 0;p+=got;n-=got;}return 1;}
static int message(uint32_t type,const void *data,uint32_t size){arcp_head h={ARCP_MAGIC,ARCP_VERSION,type,size};return io(output,&h,sizeof h,1)&&(!size||io(output,(void*)data,size,1));}
static int finish(uint32_t type,DWORD error,uint32_t count,uint64_t bytes){arcp_end e={error,count,bytes};return message(type,&e,sizeof e);}
static DWORD last_error(void){DWORD e=GetLastError();return e?e:ERROR_READ_FAULT;}
static int fingerprint(arcp_id *id){BY_HANDLE_FILE_INFORMATION i;if(!GetFileInformationByHandle(file,&i))return 0;id->volume=i.dwVolumeSerialNumber;id->file=((uint64_t)i.nFileIndexHigh<<32)|i.nFileIndexLow;id->size=((uint64_t)i.nFileSizeHigh<<32)|i.nFileSizeLow;id->mtime=((uint64_t)i.ftLastWriteTime.dwHighDateTime<<32)|i.ftLastWriteTime.dwLowDateTime;return 1;}
static int unchanged(void){arcp_id id;return fingerprint(&id)&&!memcmp(&id,&identity,sizeof id);}
static int emit_data(const void *data,size_t n){
 if(n>limit-sent){stream_error=ERROR_FILE_TOO_LARGE;return 0;}
 const char *p=data;while(n){uint32_t k=n>ARCP_CHUNK?ARCP_CHUNK:(uint32_t)n;if(!message(ARCP_DATA,p,k)){stream_error=ERROR_BROKEN_PIPE;return 0;}p+=k;n-=k;sent+=k;}return 1;
}
static wchar_t *wide_name(const char *name,int utf8,uint32_t ordinal,uint32_t *flags){
 wchar_t *w=NULL;size_t n=name?strlen(name):0;
 if(n&&n<=ARCP_NAME*4){int k=MultiByteToWideChar(utf8?CP_UTF8:437,utf8?MB_ERR_INVALID_CHARS:0,name,(int)n,NULL,0);
  if(k>0&&k<ARCP_NAME){w=calloc((size_t)k+1,sizeof *w);if(w&&!MultiByteToWideChar(utf8?CP_UTF8:437,utf8?MB_ERR_INVALID_CHARS:0,name,(int)n,w,k)){free(w);w=NULL;}}
 }
 if(!w){w=malloc(80*sizeof *w);if(w)swprintf(w,80,L"[Unreadable name — entry %u]",ordinal);*flags|=ARCP_UNSAFE;}
 if(w)for(wchar_t *p=w;*p;p++)if(*p==L'\\')*p=L'/';
 if(w&&!arc_name_safe(w))*flags|=ARCP_UNSAFE;return w;
}
static int emit_entry(uint32_t i,uint32_t flags,const char *name,int utf8,uint64_t size,uint64_t packed,uint64_t *names){
 wchar_t *w=wide_name(name,utf8,i,&flags);if(!w)return 0;uint32_t chars=(uint32_t)wcslen(w);uint32_t bytes=chars*2;
 if(*names+bytes>ARCP_NAMES_BYTES){free(w);SetLastError(ERROR_BUFFER_OVERFLOW);return 0;}*names+=bytes;
 arcp_entry e={i,flags,chars,0,size,packed};uint32_t n=sizeof e+bytes;char *buf=malloc(n);if(!buf){free(w);return 0;}
 memcpy(buf,&e,sizeof e);memcpy(buf+sizeof e,w,bytes);int ok=message(ARCP_ENTRY,buf,n);free(w);free(buf);return ok;
}
#ifdef ARCP_MINIZ
static mz_zip_archive zip;
static size_t zip_read(void *opaque,mz_uint64 offset,void *data,size_t bytes){(void)opaque;LARGE_INTEGER p;p.QuadPart=(LONGLONG)offset;if(!SetFilePointerEx(file,p,NULL,FILE_BEGIN))return 0;DWORD got=0;if(!ReadFile(file,data,bytes>MAXDWORD?MAXDWORD:(DWORD)bytes,&got,NULL))return 0;return got;}
static void backend_close(void){mz_zip_reader_end(&zip);memset(&zip,0,sizeof zip);}
static DWORD backend_open(void){zip.m_pRead=zip_read;if(!mz_zip_reader_init(&zip,identity.size,MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY))return ERROR_BAD_FORMAT;entry_count=mz_zip_reader_get_num_files(&zip);if(entry_count>ARCP_ENTRIES)return ERROR_BUFFER_OVERFLOW;
 /* A single unsupported method routes the complete document to the general backend. */
 for(uint32_t i=0;i<entry_count;i++){mz_zip_archive_file_stat s;if(!mz_zip_reader_file_stat(&zip,i,&s))return ERROR_BAD_FORMAT;if(!s.m_is_directory&&!s.m_is_supported)return ERROR_NOT_SUPPORTED;}
 return 0;}
static DWORD backend_list(void){uint64_t names=0;for(uint32_t i=0;i<entry_count;i++){mz_zip_archive_file_stat s;if(!mz_zip_reader_file_stat(&zip,i,&s))return ERROR_BAD_FORMAT;
 uint32_t flags=s.m_is_directory?ARCP_DIR:0;uint32_t mode=s.m_external_attr>>16;uint32_t type=mode&0170000;
 if((type&&type!=0100000&&type!=0040000)||(s.m_external_attr&FILE_ATTRIBUTE_REPARSE_POINT))flags|=ARCP_SPECIAL;
 if(s.m_is_encrypted)flags|=ARCP_ENCRYPTED;if(!s.m_is_supported&&!s.m_is_directory)flags|=ARCP_UNSUPPORTED;
 uint32_t n=mz_zip_reader_get_filename(&zip,i,NULL,0);if(!n||n>ARCP_NAME*4)return ERROR_BUFFER_OVERFLOW;char *name=malloc(n);if(!name)return ERROR_NOT_ENOUGH_MEMORY;
 if(mz_zip_reader_get_filename(&zip,i,name,n)!=n||strlen(name)+1!=n){free(name);return ERROR_BAD_FORMAT;}
 int ok=emit_entry(i,flags,name,!!(s.m_bit_flag&0x800),s.m_uncomp_size,s.m_comp_size,&names);free(name);if(!ok)return last_error();
 }return 0;}
static size_t zip_sink(void *opaque,mz_uint64 off,const void *data,size_t n){(void)opaque;if(off!=sent){stream_error=ERROR_INVALID_DATA;return 0;}return emit_data(data,n)?n:0;}
static DWORD backend_read(uint32_t index){mz_zip_archive_file_stat s;if(index>=entry_count||!mz_zip_reader_file_stat(&zip,index,&s))return ERROR_INVALID_PARAMETER;
 uint32_t type=(s.m_external_attr>>16)&0170000;if(s.m_is_directory||(type&&type!=0100000)||(s.m_external_attr&FILE_ATTRIBUTE_REPARSE_POINT))return ERROR_NOT_SUPPORTED;
 if(s.m_is_encrypted||!s.m_is_supported)return ERROR_NOT_SUPPORTED;if(s.m_uncomp_size>limit)return ERROR_FILE_TOO_LARGE;
 if(!mz_zip_reader_extract_to_callback(&zip,index,zip_sink,NULL,0))return stream_error?stream_error:ERROR_CRC;
 return sent==s.m_uncomp_size?0:ERROR_INVALID_DATA;}
#else
static char read_buffer[ARCP_CHUNK];
static la_ssize_t la_read(struct archive *a,void *opaque,const void **data){(void)a;(void)opaque;DWORD n=0;*data=read_buffer;return ReadFile(file,read_buffer,sizeof read_buffer,&n,NULL)?(la_ssize_t)n:-1;}
static la_int64_t la_seek(struct archive *a,void *opaque,la_int64_t off,int whence){(void)a;(void)opaque;LARGE_INTEGER pos,done;pos.QuadPart=off;return SetFilePointerEx(file,pos,&done,whence==SEEK_SET?FILE_BEGIN:whence==SEEK_CUR?FILE_CURRENT:FILE_END)?done.QuadPart:-1;}
static la_int64_t la_skip(struct archive *a,void *opaque,la_int64_t n){la_int64_t from=la_seek(a,opaque,0,SEEK_CUR);return from<0?-1:la_seek(a,opaque,n,SEEK_CUR)-from;}
static struct archive *reader(void){LARGE_INTEGER zero={0};if(!SetFilePointerEx(file,zero,NULL,FILE_BEGIN))return NULL;struct archive *a=archive_read_new();if(!a)return NULL;
 /* Register only compiled native filters: unsupported formats must never spawn PATH tools. */
 archive_read_support_filter_none(a);archive_read_support_filter_gzip(a);archive_read_support_filter_bzip2(a);archive_read_support_filter_xz(a);archive_read_support_filter_lzma(a);archive_read_support_filter_lzip(a);archive_read_support_filter_compress(a);archive_read_support_format_all(a);archive_read_support_format_raw(a);archive_read_set_seek_callback(a,la_seek);archive_read_set_skip_callback(a,la_skip);
 if(archive_read_open(a,NULL,NULL,la_read,NULL)!=ARCHIVE_OK){archive_read_free(a);return NULL;}return a;}
static void backend_close(void){}
static DWORD backend_open(void){entry_count=0;return 0;}
static uint32_t flags_for(struct archive_entry *e){uint32_t flags=0;int type=archive_entry_filetype(e);if(type==AE_IFDIR)flags|=ARCP_DIR;else if(type!=AE_IFREG)flags|=ARCP_SPECIAL;if(archive_entry_symlink(e)||archive_entry_hardlink(e))flags|=ARCP_SPECIAL;if(archive_entry_is_encrypted(e)>0)flags|=ARCP_ENCRYPTED;if(!archive_entry_size_is_set(e))flags|=ARCP_UNKNOWN_SIZE;return flags;}
static int raw_allowed(struct archive *a){return (archive_format(a)&ARCHIVE_FORMAT_BASE_MASK)!=ARCHIVE_FORMAT_RAW||archive_filter_code(a,0)!=ARCHIVE_FILTER_NONE;}
static DWORD backend_list(void){struct archive *a=reader();if(!a)return ERROR_BAD_FORMAT;struct archive_entry *e;uint64_t names=0;int code;DWORD err=0;
 while((code=archive_read_next_header(a,&e))==ARCHIVE_OK){if(!raw_allowed(a)){err=ERROR_BAD_FORMAT;break;}if(entry_count>=ARCP_ENTRIES){err=ERROR_BUFFER_OVERFLOW;break;}
  uint32_t flags=flags_for(e);const char *name=archive_entry_pathname_utf8(e);if(!name){name=archive_entry_pathname(e);flags|=ARCP_UNSAFE;}
  if(!emit_entry(entry_count,flags,name,1,archive_entry_size_is_set(e)?(uint64_t)archive_entry_size(e):0,0,&names)){err=last_error();break;}entry_count++;
 }
 if(!err&&code!=ARCHIVE_EOF)err=ERROR_BAD_FORMAT;archive_read_free(a);return err;}
static DWORD backend_read(uint32_t index){if(index>=entry_count)return ERROR_INVALID_PARAMETER;struct archive *a=reader();if(!a)return ERROR_BAD_FORMAT;struct archive_entry *e;uint32_t i=0;DWORD error=ERROR_FILE_NOT_FOUND;int code;
 while((code=archive_read_next_header(a,&e))==ARCHIVE_OK){if(i++!=index)continue;uint32_t flags=flags_for(e);if(flags&(ARCP_DIR|ARCP_SPECIAL|ARCP_ENCRYPTED)){error=(flags&ARCP_ENCRYPTED)?ERROR_PASSWORD_RESTRICTION:ERROR_NOT_SUPPORTED;break;}
  if(!(flags&ARCP_UNKNOWN_SIZE)&&(uint64_t)archive_entry_size(e)>limit){error=ERROR_FILE_TOO_LARGE;break;}
  char bytes[ARCP_CHUNK];la_ssize_t n;error=0;while((n=archive_read_data(a,bytes,sizeof bytes))>0)if(!emit_data(bytes,(size_t)n)){error=stream_error;break;}
  if(n<0&&!error){const char *why=archive_error_string(a);error=why&&(strstr(why,"unsupported")||strstr(why,"Unsupported")||strstr(why,"not supported"))?ERROR_NOT_SUPPORTED:ERROR_CRC;}break;
 }
 archive_read_free(a);return error;}
#endif
int wmain(void){input=GetStdHandle(STD_INPUT_HANDLE);output=GetStdHandle(STD_OUTPUT_HANDLE);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
 for(;;){arcp_head h;if(!io(input,&h,sizeof h,0)||h.magic!=ARCP_MAGIC||h.version!=ARCP_VERSION)break;
  if(h.type==ARCP_QUIT&&!h.size)break;
  if(h.type==ARCP_OPEN&&h.size>=4&&h.size<=ARCP_NAME*2&&!(h.size%2)){
   wchar_t *path=malloc(h.size);if(!path||!io(input,path,h.size,0)){free(path);break;}
   size_t chars=h.size/2;if(path[chars-1]||wcslen(path)+1!=chars){free(path);break;}
   backend_close();if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);file=INVALID_HANDLE_VALUE;DWORD error=provider_path_check(path,1);
   if(!error){file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);error=file==INVALID_HANDLE_VALUE?last_error():!fingerprint(&identity)?last_error():backend_open();}free(path);
   arcp_info info={error,
#ifdef ARCP_MINIZ
   1,
#else
   2,
#endif
   identity};if(!message(ARCP_INFO,&info,sizeof info))break;
   if(!error)error=backend_list();if(!error&&!unchanged())error=ERROR_FILE_INVALID;
   if(!finish(ARCP_LIST_END,error,error?0:entry_count,0))break;
  }else if(h.type==ARCP_READ&&h.size==sizeof(arcp_read)){
   arcp_read r;if(!io(input,&r,sizeof r,0))break;limit=r.limit<ARCP_EXTRACT_BYTES?r.limit:ARCP_EXTRACT_BYTES;sent=0;stream_error=0;
   DWORD error=file==INVALID_HANDLE_VALUE?ERROR_INVALID_HANDLE:!unchanged()?ERROR_FILE_INVALID:backend_read(r.index);
   if(!error&&!unchanged())error=ERROR_FILE_INVALID;if(!finish(ARCP_READ_END,error,1,sent))break;
  }else break;
 }
 backend_close();if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);return 0;
}
