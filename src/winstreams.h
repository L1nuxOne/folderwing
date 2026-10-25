/* Held-object inventory for routes that cannot represent Windows named streams.
 * Unknown or failed inventory is an error, including empty named streams. */
#ifndef FPE_WINSTREAMS_H
#define FPE_WINSTREAMS_H
#include <stddef.h>
static fpe_err win_require_unnamed_streams(HANDLE file)
{
    fpe_err error=0;
    BY_HANDLE_FILE_INFORMATION info;DWORD volume_flags=0;
    if(!GetFileInformationByHandle(file,&info))error=GetLastError();
    else if(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DEVICE))error=ERROR_NOT_SUPPORTED;
    else if(!GetVolumeInformationByHandleW(file,NULL,0,NULL,NULL,&volume_flags,NULL,0))error=GetLastError();
    if(!error&&(volume_flags&FILE_NAMED_STREAMS)){
        for(DWORD cap=4096;;cap*=2){
            uint8_t *buf=fpe_zalloc(cap);if(!buf){error=ERROR_NOT_ENOUGH_MEMORY;break;}
            SetLastError(0);
            if(!GetFileInformationByHandleEx(file,FileStreamInfo,buf,cap)){
                error=GetLastError();fpe_free(buf);
                if((error==ERROR_MORE_DATA||error==ERROR_INSUFFICIENT_BUFFER)&&cap<(1u<<24))continue;
                if(error==ERROR_HANDLE_EOF)error=0;break;
            }
            error=0;
            for(DWORD at=0;;){
                const DWORD header=(DWORD)offsetof(FILE_STREAM_INFO,StreamName);
                if(at>cap-header){error=ERROR_INVALID_DATA;break;}
                FILE_STREAM_INFO *stream=(FILE_STREAM_INFO *)(buf+at);DWORD bytes=stream->StreamNameLength;
                if((bytes&1)||bytes>cap-at-header){error=ERROR_INVALID_DATA;break;}
                if(bytes&&(bytes!=7*sizeof(wchar_t)||_wcsnicmp(stream->StreamName,L"::$DATA",7))){error=ERROR_NOT_SUPPORTED;break;}
                DWORD next=stream->NextEntryOffset;if(!next)break;
                if(next<header+bytes||(next&7)||next>cap-at-header){error=ERROR_INVALID_DATA;break;}at+=next;
            }
            fpe_free(buf);break;
        }
    }
    return error;
}
#endif
