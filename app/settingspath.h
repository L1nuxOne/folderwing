/* Product data migration. Copy known files once; never remove legacy data or
 * replace an existing destination. Explicit command-line paths bypass this. */
#pragma once
#include <windows.h>
#include <wchar.h>
#include <stdint.h>
#include <wctype.h>
#include <stdio.h>

static int fw_data_file(const wchar_t *path)
{
    DWORD a=GetFileAttributesW(path);
    return a!=INVALID_FILE_ATTRIBUTES && !(a&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT));
}
static int fw_data_join(wchar_t *out,size_t cap,const wchar_t *dir,const wchar_t *name)
{
    int n=swprintf(out,cap,L"%ls\\%ls",dir,name);
    return n>=0 && (size_t)n<cap;
}
/* Publish a complete flushed copy without overwrite, including a concurrent
 * creator. Hold the source without sharing writes and refuse reparse points. */
static DWORD fw_data_copy(const wchar_t *source,const wchar_t *target)
{
    DWORD a=GetFileAttributesW(target);
    if(a!=INVALID_FILE_ATTRIBUTES)return fw_data_file(target)?0:ERROR_ACCESS_DENIED;
    DWORD e=GetLastError();if(e!=ERROR_FILE_NOT_FOUND&&e!=ERROR_PATH_NOT_FOUND)return e;
    HANDLE in=CreateFileW(source,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(in==INVALID_HANDLE_VALUE){e=GetLastError();return (e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND)?0:e;}
    BY_HANDLE_FILE_INFORMATION info;
    if(!GetFileInformationByHandle(in,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))){CloseHandle(in);return ERROR_ACCESS_DENIED;}
    wchar_t temp[MAX_PATH+96];static volatile LONG serial;
    int n=swprintf(temp,sizeof temp/sizeof *temp,L"%ls.%lu.%llu.%ld.migrating",target,GetCurrentProcessId(),(unsigned long long)GetTickCount64(),InterlockedIncrement(&serial));
    if(n<0||(size_t)n>=sizeof temp/sizeof *temp){CloseHandle(in);return ERROR_FILENAME_EXCED_RANGE;}
    HANDLE out=CreateFileW(temp,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if(out==INVALID_HANDLE_VALUE){e=GetLastError();CloseHandle(in);return e;}
    char buffer[16384];DWORD got,written;e=0;
    for(;;){if(!ReadFile(in,buffer,sizeof buffer,&got,NULL)){e=GetLastError();break;}if(!got)break;
        if(!WriteFile(out,buffer,got,&written,NULL)||written!=got){e=GetLastError();if(!e)e=ERROR_WRITE_FAULT;break;}}
    if(!e&&!FlushFileBuffers(out))e=GetLastError();CloseHandle(out);CloseHandle(in);
    if(!e&&!MoveFileExW(temp,target,MOVEFILE_WRITE_THROUGH)){
        e=GetLastError();if((e==ERROR_ALREADY_EXISTS||e==ERROR_FILE_EXISTS)&&fw_data_file(target))e=0;
    }
    DeleteFileW(temp);return e;
}
static DWORD fw_data_migrate(const wchar_t *legacy,const wchar_t *current)
{
    wchar_t old[MAX_PATH+32],next[MAX_PATH+32],marker[MAX_PATH+32];
    const wchar_t *files[]={L"session.ini",L"session.ini.workspaces",L"folder-views.txt",L"pdf-positions.txt",L"openpilot.ini"};
    if(!fw_data_join(marker,MAX_PATH+32,current,L".openpilot-migrated"))return ERROR_FILENAME_EXCED_RANGE;
    uint64_t hash=14695981039346656037ull;for(const wchar_t *p=current;*p;p++){hash^=(uint16_t)towlower(*p);hash*=1099511628211ull;}
    wchar_t name[80];swprintf(name,80,L"Local\\FolderwingMigration-%016llx",(unsigned long long)hash);
    HANDLE mutex=CreateMutexW(NULL,FALSE,name);if(!mutex)return GetLastError();
    DWORD wait=WaitForSingleObject(mutex,5000),e=0;
    if(wait!=WAIT_OBJECT_0&&wait!=WAIT_ABANDONED){CloseHandle(mutex);return ERROR_BUSY;}
    if(fw_data_file(marker))goto done;
    /* A current settings file already establishes the new data set as authority. */
    if(!fw_data_join(next,MAX_PATH+32,current,L"folderwing.ini")){e=ERROR_FILENAME_EXCED_RANGE;goto done;}
    if(fw_data_file(next))goto done;
    int found=0;
    for(unsigned i=0;i<sizeof files/sizeof *files;i++){
        if(!fw_data_join(old,MAX_PATH+32,legacy,files[i])){e=ERROR_FILENAME_EXCED_RANGE;goto done;}
        DWORD a=GetFileAttributesW(old);
        if(a!=INVALID_FILE_ATTRIBUTES)found=1;
        else {DWORD reason=GetLastError();if(reason!=ERROR_FILE_NOT_FOUND&&reason!=ERROR_PATH_NOT_FOUND){e=reason;goto done;}}
    }
    if(!found)goto done;
    if(!CreateDirectoryW(current,NULL)&&GetLastError()!=ERROR_ALREADY_EXISTS){e=GetLastError();goto done;}
    DWORD attr=GetFileAttributesW(current);
    if(attr==INVALID_FILE_ATTRIBUTES||!(attr&FILE_ATTRIBUTE_DIRECTORY)||(attr&FILE_ATTRIBUTE_REPARSE_POINT)){e=ERROR_ACCESS_DENIED;goto done;}
    for(unsigned i=0;i<sizeof files/sizeof *files;i++){
        fw_data_join(old,MAX_PATH+32,legacy,files[i]);
        fw_data_join(next,MAX_PATH+32,current,i==4?L"folderwing.ini":files[i]);
        if((e=fw_data_copy(old,next)))goto done;
    }
    HANDLE f=CreateFileW(marker,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f==INVALID_HANDLE_VALUE){e=GetLastError();if(e==ERROR_FILE_EXISTS||e==ERROR_ALREADY_EXISTS)e=0;}
    else {DWORD wrote;const char text[]="Legacy OpenPilot data retained; migration complete.\r\n";
        if(!WriteFile(f,text,sizeof text-1,&wrote,NULL)||wrote!=sizeof text-1||!FlushFileBuffers(f))e=ERROR_WRITE_FAULT;
        CloseHandle(f);if(e)DeleteFileW(marker);}
done:
    ReleaseMutex(mutex);CloseHandle(mutex);return e;
}
/* Portable files take precedence. A failed migration continues using the old
 * location, preserving settings and all session-relative companions together. */
static DWORD fw_settings_path(wchar_t *out,size_t cap,const wchar_t *portable,const wchar_t *appdata)
{
    wchar_t legacy[MAX_PATH+32],current[MAX_PATH+32],oldfile[MAX_PATH+32];
    if(portable&&*portable){
        if(!fw_data_join(out,cap,portable,L"folderwing.ini")||!fw_data_join(oldfile,MAX_PATH+32,portable,L"openpilot.ini"))return ERROR_FILENAME_EXCED_RANGE;
        if(GetFileAttributesW(out)!=INVALID_FILE_ATTRIBUTES)return 0;
        if(GetFileAttributesW(oldfile)!=INVALID_FILE_ATTRIBUTES){DWORD e=fw_data_copy(oldfile,out);if(e)swprintf(out,cap,L"%ls",oldfile);return e;}
    }
    if(!appdata||!*appdata){out[0]=0;return ERROR_PATH_NOT_FOUND;}
    if(!fw_data_join(legacy,MAX_PATH+32,appdata,L"OpenPilot")||!fw_data_join(current,MAX_PATH+32,appdata,L"Folderwing"))return ERROR_FILENAME_EXCED_RANGE;
    DWORD e=fw_data_migrate(legacy,current);
    if(!fw_data_join(out,cap,e?legacy:current,e?L"openpilot.ini":L"folderwing.ini"))return ERROR_FILENAME_EXCED_RANGE;
    return e;
}
