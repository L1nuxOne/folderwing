/* A replaced, immutable order snapshot. One scanner and one pending request;
 * classification uses directory metadata only and never opens any file. */
#include "imagebrowse.h"
#include "imageutil.h"
#include "imageproto.h"
#include <stdlib.h>
#include <string.h>
struct imagebrowse_worker {
    HWND target;UINT message;HANDLE wake,thread;CRITICAL_SECTION lock;
    volatile LONG stop;volatile LONG64 sequence;imagebrowse_list *pending,*result;int posted;
};
int imagebrowse_candidate(const fpe_dir *dir,const fpe_entry *entry,const wchar_t *name)
{
    if(!entry||fpe_entry_is_dir(entry)||!image_extension(name))return 0;
    fpe_unix u;if(dir&&fpe_dir_unix(dir,entry,&u)&&(u.mode&0170000)!=0100000)return 0;
    return 1;
}
void imagebrowse_free(imagebrowse_list *l){if(l){fpe_dir_release(l->dir);free(l);}}
static DWORD WINAPI scan(void *arg)
{
    imagebrowse_worker *w=arg;
    for(;;){
        WaitForSingleObject(w->wake,INFINITE);
        EnterCriticalSection(&w->lock);imagebrowse_list *l=w->pending;w->pending=NULL;
        int stop=w->stop!=0;LeaveCriticalSection(&w->lock);
        if(stop){imagebrowse_free(l);break;}if(!l)continue;
        uint32_t n=l->count,k=0;const fpe_entry *entries=fpe_dir_entries(l->dir);
        const wchar_t *parent=fpe_dir_path(l->dir);size_t prefix=wcslen(parent);if(prefix&&parent[prefix-1]!=L'\\')prefix++;
        for(uint32_t i=0;i<n;i++){
            if(!(i&255)&&(w->stop||(uint64_t)InterlockedCompareExchange64(&w->sequence,0,0)!=l->sequence))break;
            const fpe_entry *e=entries+l->rows[i];
            if(e->size<=l->max_bytes&&prefix+e->name_len<IMGP_PATH_MAX&&imagebrowse_candidate(l->dir,e,fpe_dir_name(l->dir,e)))l->rows[k++]=i;
        }
        l->count=k;
        EnterCriticalSection(&w->lock);
        if(w->stop||(uint64_t)InterlockedCompareExchange64(&w->sequence,0,0)!=l->sequence||!w->target)imagebrowse_free(l);
        else{
            imagebrowse_free(w->result);w->result=l;
            if(!w->posted){
                if(PostMessageW(w->target,w->message,0,0))w->posted=1;
                else{imagebrowse_free(w->result);w->result=NULL;}
            }
        }
        LeaveCriticalSection(&w->lock);
    }
    return 0;
}
imagebrowse_worker *imagebrowse_start(HWND target,UINT message)
{
    imagebrowse_worker *w=calloc(1,sizeof *w);if(!w)return NULL;
    w->target=target;w->message=message;InitializeCriticalSection(&w->lock);w->wake=CreateEventW(NULL,FALSE,FALSE,NULL);
    if(w->wake)w->thread=CreateThread(NULL,0,scan,w,0,NULL);
    if(!w->thread){if(w->wake)CloseHandle(w->wake);DeleteCriticalSection(&w->lock);free(w);return NULL;}return w;
}
imagebrowse_list *imagebrowse_take(imagebrowse_worker *w)
{
    if(!w)return NULL;EnterCriticalSection(&w->lock);imagebrowse_list *l=w->result;w->result=NULL;w->posted=0;LeaveCriticalSection(&w->lock);return l;
}
void imagebrowse_cancel(imagebrowse_worker *w,uint64_t sequence)
{
    if(!w)return;EnterCriticalSection(&w->lock);InterlockedExchange64(&w->sequence,(LONG64)sequence);
    imagebrowse_free(w->pending);w->pending=NULL;imagebrowse_free(w->result);w->result=NULL;LeaveCriticalSection(&w->lock);SetEvent(w->wake);
}
DWORD imagebrowse_submit(imagebrowse_worker *w,const fpe_view *view,uint64_t sequence,uint64_t max_bytes)
{
    if(!w||!view)return ERROR_INVALID_PARAMETER;
    imagebrowse_cancel(w,sequence);
    uint32_t n=fpe_view_count(view);if(n>IMAGEBROWSE_MAX_ROWS)return ERROR_BUFFER_OVERFLOW;
    imagebrowse_list *l=calloc(1,sizeof *l+(size_t)n*sizeof(uint32_t));if(!l)return ERROR_NOT_ENOUGH_MEMORY;
    l->dir=fpe_dir_retain(fpe_view_dir(view));l->sequence=sequence;l->revision=fpe_view_revision(view);l->count=n;l->max_bytes=max_bytes;
    DWORD error=fpe_view_copy_order(view,l->rows,n);if(error){imagebrowse_free(l);return error;}
    EnterCriticalSection(&w->lock);InterlockedExchange64(&w->sequence,(LONG64)sequence);
    imagebrowse_free(w->pending);w->pending=l;imagebrowse_free(w->result);w->result=NULL;LeaveCriticalSection(&w->lock);SetEvent(w->wake);return 0;
}
void imagebrowse_stop(imagebrowse_worker *w)
{
    if(!w)return;EnterCriticalSection(&w->lock);InterlockedExchange(&w->stop,1);w->target=NULL;
    imagebrowse_free(w->pending);w->pending=NULL;imagebrowse_free(w->result);w->result=NULL;LeaveCriticalSection(&w->lock);SetEvent(w->wake);
    WaitForSingleObject(w->thread,INFINITE);CloseHandle(w->thread);CloseHandle(w->wake);DeleteCriticalSection(&w->lock);free(w);
}
static uint32_t lower(const imagebrowse_list *l,uint32_t row)
{
    uint32_t a=0,b=l?l->count:0;while(a<b){uint32_t m=a+(b-a)/2;if(l->rows[m]<row)a=m+1;else b=m;}return a;
}
uint32_t imagebrowse_index(const imagebrowse_list *l,uint32_t row)
{
    uint32_t i=lower(l,row);return l&&i<l->count&&l->rows[i]==row?i:UINT32_MAX;
}
uint32_t imagebrowse_neighbor(const imagebrowse_list *l,uint32_t row,int direction)
{
    if(!l||!l->count||row==UINT32_MAX||!direction)return UINT32_MAX;
    uint32_t i=lower(l,row);
    if(direction>0){if(i<l->count&&l->rows[i]==row)i++;return i<l->count?l->rows[i]:UINT32_MAX;}
    return i?l->rows[i-1]:UINT32_MAX;
}
int imagebrowse_remove(imagebrowse_list *l,uint32_t row)
{
    uint32_t i=imagebrowse_index(l,row);if(i==UINT32_MAX)return 0;
    memmove(l->rows+i,l->rows+i+1,(size_t)(l->count-i-1)*sizeof *l->rows);l->count--;return 1;
}
