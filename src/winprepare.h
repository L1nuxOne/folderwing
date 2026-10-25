/* Bounded copy-only Linux -> Windows preparation. No background committer,
 * early durable ACK or moved source. Eligible NTFS copies default to group8;
 * explicit environment overrides retain ordinary preparation or serial mode. */
#ifndef FPE_WINPREPARE_H
#define FPE_WINPREPARE_H
typedef struct windows_pending {windows_file file;uint64_t token;} windows_pending;
typedef struct windows_prepare {
    windows_pending slots[WSLP_PREPARE_MAX];
    uint64_t serial,active_token,expected,received;
    unsigned width,count,group;
} windows_prepare;
static unsigned windows_prepare_width(void)
{
    wchar_t value[8];DWORD n=GetEnvironmentVariableW(L"OPENPILOT_WINDOWS_PREPARE_WIDTH",value,8);
    if(!n&&GetLastError()==ERROR_ENVVAR_NOT_FOUND)return 8;
    return n==1&&(value[0]==L'2'||value[0]==L'4'||value[0]==L'8')?(unsigned)(value[0]-L'0'):0;
}
static void windows_prepare_abort(windows_prepare *q)
{for(unsigned i=0;i<q->count;i++)windows_abort(&q->slots[i].file);q->count=0;}
static fpe_err windows_prepare_begin(windows_prepare *q,windows_file *w,const wslp_prepared_file *f,
                                    const uint8_t *suffix,const wchar_t *dest,int sync_directory)
{
    if(!q->width||q->count>=q->width||w->active||q->active_token||f->file.replace||f->file.pad||
       f->token!=q->serial+1||!f->bytes||f->bytes>WSLP_PREPARE_BYTES)return ERROR_INVALID_DATA;
    const uint8_t *leaf=suffix;
    for(uint32_t i=0;i<f->file.path_bytes;i++)if(suffix[i]=='/')leaf=suffix+i+1;
    for(const uint8_t *p=leaf;p<suffix+f->file.path_bytes;p++)if(*p>=128)return ERROR_INVALID_DATA;
    windows_begin(w,&f->file,suffix,dest,sync_directory);
    /* The helper drains aliases before resolving their conflict policy. Reject
     * a malformed peer which tries to retain the same Windows target twice. */
    if(w->dest)for(unsigned i=0;i<q->count;i++)if(!_wcsicmp(w->dest,q->slots[i].file.dest))return ERROR_INVALID_DATA;
    q->active_token=f->token;q->serial=f->token;q->expected=f->bytes;q->received=0;return 0;
}
static fpe_err windows_prepare_data(windows_prepare *q,DWORD bytes)
{
    if(!q->active_token)return 0;
    if(bytes>q->expected-q->received)return ERROR_INVALID_DATA;
    q->received+=bytes;return 0;
}
static wslp_file_result windows_prepare_end(windows_prepare *q,windows_file *w,int32_t source_error,volatile LONG *stop)
{
    fpe_err error=w->err;
    if((source_error||(stop&&*stop))&&!error)error=ERROR_CANCELLED;
    if(q->received!=q->expected&&!error)error=ERROR_INVALID_DATA;
    wslp_file_result result={q->active_token,errno_windows(error),0};q->active_token=0;
    if(error)windows_abort(w);
    else {
        q->slots[q->count++]=(windows_pending){*w,result.token};
        uint32_t serial=w->serial;memset(w,0,sizeof *w);w->serial=serial;
    }
    return result;
}
static fpe_err windows_prepare_commit(windows_prepare *q,const wslp_file_result *command,volatile LONG *stop,wslp_file_result *result)
{
    if(!q->count||q->active_token||command->pad||command->error<0||command->error>4095||
       command->token!=q->slots[0].token)return ERROR_INVALID_DATA;
    fpe_err error=windows_end(&q->slots[0].file,command->error,stop);
    *result=(wslp_file_result){command->token,errno_windows(error),0};
    memmove(q->slots,q->slots+1,--q->count*sizeof q->slots[0]);return 0;
}
#endif
