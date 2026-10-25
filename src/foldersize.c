/* Uncached read-only scanner. All work belongs to the invoking worker. */
#include "internal.h"
#include <string.h>
#define SIZE_DEPTH 128
#define SIZE_PATH 32768
#define SIZE_BUFFER 65536

typedef struct scan {
    fpe_size_result r; fpe_size_cb cb; void *user;
    uint64_t notified; int stopped; wchar_t *path;
} scan;
static void failed(scan *s,DWORD err)
{
    s->r.errors++;if(!s->r.first_error)s->r.first_error=err?err:ERROR_GEN_FAILURE;
}
static int pulse(scan *s,int force)
{
    uint64_t now=GetTickCount64();
    if(!s->stopped&&(force||now-s->notified>=50)){
        s->notified=now;if(s->cb&&!s->cb(s->user,&s->r))s->stopped=1;
    }
    return !s->stopped;
}
static void walk(scan *s,size_t length,unsigned depth)
{
    if(!pulse(s,0))return;
    HANDLE h=CreateFileW(s->path,FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(h==INVALID_HANDLE_VALUE){failed(s,GetLastError());return;}
    BY_HANDLE_FILE_INFORMATION info;
    if(!GetFileInformationByHandle(h,&info)){failed(s,GetLastError());CloseHandle(h);return;}
    if(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT){s->r.skipped++;CloseHandle(h);return;}
    if(!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)){failed(s,ERROR_DIRECTORY);CloseHandle(h);return;}
    s->r.folders++;
    if(depth>=SIZE_DEPTH){failed(s,ERROR_STACK_OVERFLOW);CloseHandle(h);return;}
    BYTE *buf=fpe_alloc(SIZE_BUFFER);
    if(!buf){failed(s,ERROR_NOT_ENOUGH_MEMORY);CloseHandle(h);return;}
    while(pulse(s,0)){
        if(!GetFileInformationByHandleEx(h,FileIdBothDirectoryInfo,buf,SIZE_BUFFER)){
            DWORD err=GetLastError();if(err!=ERROR_NO_MORE_FILES)failed(s,err);break;
        }
        DWORD at=0;
        for(;;){
            FILE_ID_BOTH_DIR_INFO *entry=(void*)(buf+at);
            DWORD bytes=entry->FileNameLength;size_t n=bytes/sizeof(wchar_t);
            if(bytes%2||at+offsetof(FILE_ID_BOTH_DIR_INFO,FileName)+bytes>SIZE_BUFFER){failed(s,ERROR_INVALID_DATA);break;}
            int dot=(n==1&&entry->FileName[0]==L'.')||(n==2&&entry->FileName[0]==L'.'&&entry->FileName[1]==L'.');
            if(!pulse(s,0))break;
            if(!dot){
                if(entry->FileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DEVICE))s->r.skipped++;
                else{
                    size_t sep=length&&s->path[length-1]!=L'\\';
                    if(length+sep+n+1>SIZE_PATH)failed(s,ERROR_FILENAME_EXCED_RANGE);
                    else{
                        if(sep)s->path[length]=L'\\';memcpy(s->path+length+sep,entry->FileName,bytes);s->path[length+sep+n]=0;
                        if(entry->FileAttributes&FILE_ATTRIBUTE_DIRECTORY)walk(s,length+sep+n,depth+1);
                        else{
                            /* NTFS can cache a stale size per hard-link name in
                             * directory records. Query the actual file handle. */
                            HANDLE file=CreateFileW(s->path,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                                NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_OPEN_NO_RECALL,NULL);
                            BY_HANDLE_FILE_INFORMATION actual;
                            if(file==INVALID_HANDLE_VALUE)failed(s,GetLastError());
                            else{
                                if(!GetFileInformationByHandle(file,&actual))failed(s,GetLastError());
                                else if(actual.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)s->r.skipped++;
                                else if(actual.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)failed(s,ERROR_FILE_INVALID);
                                else{
                                    uint64_t size=((uint64_t)actual.nFileSizeHigh<<32)|actual.nFileSizeLow;
                                    if(UINT64_MAX-s->r.bytes<size)failed(s,ERROR_ARITHMETIC_OVERFLOW);
                                    else{s->r.files++;s->r.bytes+=size;}
                                }
                                CloseHandle(file);
                            }
                        }
                        s->path[length]=0;
                    }
                }
            }
            if(s->stopped||!entry->NextEntryOffset)break;
            if(entry->NextEntryOffset<offsetof(FILE_ID_BOTH_DIR_INFO,FileName)+bytes||entry->NextEntryOffset>=SIZE_BUFFER-at){failed(s,ERROR_INVALID_DATA);break;}
            at+=entry->NextEntryOffset;
        }
    }
    fpe_free(buf);CloseHandle(h);
}
FPE_API fpe_err fpe_folder_size(const wchar_t *path,int as_root,fpe_size_cb cb,void *user,fpe_size_result *out)
{
    if(!out)return ERROR_INVALID_PARAMETER;memset(out,0,sizeof *out);
    if(!path||!path[0]){out->errors=1;out->first_error=ERROR_INVALID_PARAMETER;out->state=FPE_SIZE_PARTIAL;return out->first_error;}
    if(fpe_wsl_path_kind(path))return fpe_wsl_size(path,as_root,cb,user,out);
    scan s={.cb=cb,.user=user};wchar_t *nt=NULL;fpe_err err=fpe_path_nt(path,&nt);
    if(!err){
        size_t n=wcslen(nt);s.path=fpe_alloc(SIZE_PATH*sizeof(wchar_t));
        if(!s.path)err=ERROR_NOT_ENOUGH_MEMORY;
        else if(n>=SIZE_PATH)err=ERROR_FILENAME_EXCED_RANGE;
        else{memcpy(s.path,nt,(n+1)*sizeof(wchar_t));if(pulse(&s,1))walk(&s,n,0);}
    }
    if(err)failed(&s,err);
    /* Ask once more so a stop during the last filesystem call is retained. */
    pulse(&s,1);s.r.state=s.stopped?FPE_SIZE_CANCELLED:s.r.errors?FPE_SIZE_PARTIAL:FPE_SIZE_COMPLETE;
    if(cb)cb(user,&s.r);*out=s.r;fpe_free(s.path);fpe_free(nt);
    return s.r.state==FPE_SIZE_CANCELLED?ERROR_CANCELLED:s.r.first_error;
}
