/* Copy-only NTFS group commit for eligible local destinations. Every staged file gets a checked
 * data AND metadata writeout, followed by a full same-volume fence before
 * publication. Published prefixes are fenced again before durable credit.
 * GROUP_FAULT is a compile-time test hook after the real persistence API;
 * production builds never read fault-injection environment variables. */
#include <winternl.h>
#ifndef GROUP_FAULT
#define GROUP_FAULT(tag,error) (error)
#endif
static void group_profile(const char *zone,int64_t started)
{
    if(!strcmp(zone,"group.pre_writeout")){FPE_PROF_END("group.pre_writeout",started);}
    else if(!strcmp(zone,"group.pre_fence")){FPE_PROF_END("group.pre_fence",started);}
    else if(!strcmp(zone,"group.post_writeout")){FPE_PROF_END("group.post_writeout",started);}
    else if(!strcmp(zone,"group.post_fence")){FPE_PROF_END("group.post_fence",started);}
    else if(!strcmp(zone,"group.namespace")){FPE_PROF_END("group.namespace",started);}
}
static fpe_err group_fence(HANDLE h,const char *zone)
{
    int64_t started=fpe_prof_begin();fpe_err error=FlushFileBuffers(h)?0:GetLastError();
    group_profile(zone,started);return GROUP_FAULT(zone,error);
}
/* No parallel committers, volume handles, or source removal. */
typedef NTSTATUS (NTAPI *group_nt_flush)(HANDLE,ULONG,PVOID,ULONG,PIO_STATUS_BLOCK);
typedef ULONG (WINAPI *group_nt_error)(NTSTATUS);
static int windows_group_requested(const wchar_t *dest)
{
    wchar_t opt[4],fs[32];DWORD n=GetEnvironmentVariableW(L"OPENPILOT_WINDOWS_GROUP_COMMIT",opt,4);
    int enabled=(!n&&GetLastError()==ERROR_ENVVAR_NOT_FOUND)||(n==1&&opt[0]==L'1');
    if(!enabled||!dest||!dest[0]||dest[1]!=L':'||dest[2]!=L'\\')return 0;
    wchar_t drive[]={dest[0],L':',L'\\',0};return GetDriveTypeW(drive)==DRIVE_FIXED&&GetVolumeInformationW(drive,NULL,0,NULL,NULL,NULL,fs,32)&&!wcscmp(fs,L"NTFS");
}
static fpe_err group_writeout(HANDLE h,group_nt_flush flush,group_nt_error dos,const char *zone)
{
    int64_t started=fpe_prof_begin();IO_STATUS_BLOCK io={0};NTSTATUS status=flush(h,2,NULL,0,&io);
    if(status==(NTSTATUS)0x103){DWORD wait=WaitForSingleObject(h,INFINITE);if(wait!=WAIT_OBJECT_0){fpe_err error=wait==WAIT_FAILED?GetLastError():ERROR_GEN_FAILURE;group_profile(zone,started);return error;}status=io.Status;if(status==(NTSTATUS)0x103){group_profile(zone,started);return ERROR_GEN_FAILURE;}}
    fpe_err error=status>=0?0:dos(status);group_profile(zone,started);return GROUP_FAULT(zone,error);
}
/* The caller holds the reader's current frame. Release it once for each fresh
 * source check; on return the reader again waits for the outer consumed event. */
static fpe_err group_source_check(HANDLE to,job_reader *reader,uint64_t token,volatile LONG *stop,int *cancel_sent,int *fatal)
{
    wslp_file_result check={token,0,0};
    if(!windows_file_reply(to,WSLP_GROUP_CHECK,&check)){*fatal=1;return ERROR_BROKEN_PIPE;}
    SetEvent(reader->consumed);uint64_t began=GetTickCount64();
    for(;;){
        if(stop&&*stop&&!*cancel_sent){if(!job_control(to,WSLP_JOB_CANCEL)){*fatal=1;return ERROR_BROKEN_PIPE;}*cancel_sent=1;}
        DWORD wait=WaitForSingleObject(reader->ready,10);
        if(wait==WAIT_OBJECT_0)break;
        if(wait!=WAIT_TIMEOUT||GetTickCount64()-began>REQ_TIMEOUT_MS){*fatal=1;return wait==WAIT_TIMEOUT?ERROR_TIMEOUT:ERROR_BROKEN_PIPE;}
    }
    if(reader->err){*fatal=1;return reader->err;}
    wslp_file_result answer={0};
    if(reader->head.id!=1||reader->head.type!=WSLP_GROUP_CHECKED||reader->head.size!=sizeof answer){*fatal=1;return ERROR_INVALID_DATA;}
    memcpy(&answer,reader->buf,sizeof answer);
    if(answer.token!=token||answer.pad||answer.error<0||answer.error>4095){*fatal=1;return ERROR_INVALID_DATA;}
    return from_errno(answer.error);
}
static fpe_err windows_group_commit(windows_prepare *q,const void *data,uint32_t size,HANDLE to,job_reader *reader,volatile LONG *stop,int *cancel_sent)
{
    wslp_group result={0};int fatal=0;fpe_err error=0;
    if(!q->group||(q->width!=2&&q->width!=4&&q->width!=8)||!q->count||q->count>WSLP_PREPARE_MAX||q->count>q->width||q->active_token||size!=8+q->count*sizeof(wslp_file_result))return ERROR_INVALID_DATA;
    memcpy(&result,data,size);
    if(result.count!=q->count||result.pad)return ERROR_INVALID_DATA;
    for(unsigned i=0;i<result.count;i++)if(result.files[i].token!=q->slots[i].token||result.files[i].pad||result.files[i].error<0||result.files[i].error>4095)return ERROR_INVALID_DATA;
    HMODULE nt=GetModuleHandleW(L"ntdll.dll");group_nt_flush flush=(group_nt_flush)GetProcAddress(nt,"NtFlushBuffersFileEx");group_nt_error dos=(group_nt_error)GetProcAddress(nt,"RtlNtStatusToDosError");
    int eligible=flush&&dos&&q->count>1;BY_HANDLE_FILE_INFORMATION parent={0};
    for(unsigned i=0;i<q->count&&eligible;i++){
        windows_file *w=&q->slots[i].file;BY_HANDLE_FILE_INFORMATION f,d;wchar_t fs[32];
        if(w->replace||!w->directory||w->ancestry){eligible=0;break;}
        if(!GetFileInformationByHandle(w->file,&f)||!GetFileInformationByHandle(w->directory,&d))return GetLastError();
        if(f.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DEVICE)||f.dwVolumeSerialNumber!=d.dwVolumeSerialNumber||
           !GetVolumeInformationByHandleW(w->directory,NULL,0,NULL,NULL,NULL,fs,32)||wcscmp(fs,L"NTFS")){eligible=0;break;}
        if(!i)parent=d;
        else if(parent.dwVolumeSerialNumber!=d.dwVolumeSerialNumber||parent.nFileIndexHigh!=d.nFileIndexHigh||parent.nFileIndexLow!=d.nFileIndexLow){eligible=0;break;}
    }
    /* An initial source error aborts the bounded group before publication. */
    for(unsigned i=0;i<q->count;i++)if(result.files[i].error&&!error)error=from_errno(result.files[i].error);
    if(!error&&stop&&*stop)error=ERROR_CANCELLED;
    if(eligible&&!error){
        for(unsigned i=0;i<q->count&&!error;i++){
            windows_file *w=&q->slots[i].file;FILE_BASIC_INFO basic={0};basic.FileAttributes=FILE_ATTRIBUTE_NORMAL;
            if(w->err)error=w->err;
            else if(!SetFileTime(w->file,NULL,&w->atime,&w->mtime)||!SetFileInformationByHandle(w->file,FileBasicInfo,&basic,sizeof basic))error=GetLastError();
            if(!error)error=group_writeout(w->file,flush,dos,"group.pre_writeout");
            if(!error&&stop&&*stop)error=ERROR_CANCELLED;
        }
        if(!error)error=group_fence(q->slots[0].file.file,"group.pre_fence");
    }
    unsigned published=0;
    for(unsigned i=0;i<q->count;i++){
        windows_file *w=&q->slots[i].file;
        if(!error&&stop&&*stop)error=ERROR_CANCELLED;
        if(!error)error=group_source_check(to,reader,q->slots[i].token,stop,cancel_sent,&fatal);
        if(!error&&stop&&*stop)error=ERROR_CANCELLED;
        if(!error&&eligible){
            const wchar_t *leaf=wcsrchr(w->dest,L'\\');int64_t began=fpe_prof_begin();error=leaf&&leaf[1]?fpe_win_rename_handle(w->file,NULL,leaf+1,0,0):ERROR_INVALID_NAME;FPE_PROF_END("group.rename",began);
            if(!error){w->created=0;published++;error=group_writeout(w->file,flush,dos,"group.post_writeout");}
        }else if(!error){error=windows_end(w,0,stop);if(!error)published++;}
        result.files[i].error=errno_windows(error);
    }
    /* Always fence a published prefix, also on failed later publication,
     * broken source transport, stop, or writeout failure. Never delete it. */
    if(eligible&&published){
        fpe_err fence=0;
        fence=group_fence(q->slots[0].file.file,"group.post_fence");
        {fpe_err e=group_fence(q->slots[0].file.directory,"group.namespace");if(e&&!fence)fence=e;}
        if(fence){for(unsigned i=0;i<q->count;i++)if(!q->slots[i].file.created)result.files[i].error=errno_windows(fence);if(!error)error=fence;}
    }
    /* Cancellation after all persistence barriers retains durable successes;
     * outstanding/unpublished work carries cancellation/error results. */
    if(stop&&*stop&&!*cancel_sent){if(!job_control(to,WSLP_JOB_CANCEL)){fatal=1;if(!error)error=ERROR_BROKEN_PIPE;}*cancel_sent=1;}
    for(unsigned i=0;i<q->count;i++)windows_abort(&q->slots[i].file);
    q->count=0;
    if(fatal)return error?error:ERROR_BROKEN_PIPE;
    wslp_head h={WSLP_GROUP_COMMITTED,8+result.count*sizeof(wslp_file_result),1};uint8_t bytes[sizeof h+sizeof result];DWORD wrote=0;
    memcpy(bytes,&h,sizeof h);memcpy(bytes+sizeof h,&result,h.size);
    if(!WriteFile(to,bytes,sizeof h+h.size,&wrote,NULL)||wrote!=sizeof h+h.size)return ERROR_BROKEN_PIPE;
    return 0;
}
