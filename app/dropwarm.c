#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <stdlib.h>
#include "fpe.h"
#include "dropwarm.h"

typedef struct warm_target { IDropTarget iface; LONG refs; } warm_target;
static HRESULT STDMETHODCALLTYPE warm_qi(IDropTarget *p,REFIID id,void **out)
{
    if(!out)return E_POINTER;*out=NULL;
    if(!IsEqualIID(id,&IID_IUnknown)&&!IsEqualIID(id,&IID_IDropTarget))return E_NOINTERFACE;
    *out=p;IDropTarget_AddRef(p);return S_OK;
}
static ULONG STDMETHODCALLTYPE warm_addref(IDropTarget *p){return InterlockedIncrement(&((warm_target *)p)->refs);}
static ULONG STDMETHODCALLTYPE warm_release(IDropTarget *p)
{
    ULONG n=InterlockedDecrement(&((warm_target *)p)->refs);if(!n)free(p);return n;
}
static HRESULT STDMETHODCALLTYPE warm_enter(IDropTarget *p,IDataObject *data,DWORD keys,POINTL pt,DWORD *effect){*effect=DROPEFFECT_NONE;return S_OK;}
static HRESULT STDMETHODCALLTYPE warm_over(IDropTarget *p,DWORD keys,POINTL pt,DWORD *effect){*effect=DROPEFFECT_NONE;return S_OK;}
static HRESULT STDMETHODCALLTYPE warm_leave(IDropTarget *p){return S_OK;}
static HRESULT STDMETHODCALLTYPE warm_drop(IDropTarget *p,IDataObject *data,DWORD keys,POINTL pt,DWORD *effect){*effect=DROPEFFECT_NONE;return S_OK;}
static IDropTargetVtbl warm_vtbl={warm_qi,warm_addref,warm_release,warm_enter,warm_over,warm_leave,warm_drop};

typedef struct warm_job {
    HANDLE thread,ready,stop;
    volatile LONG result;
    int observed,cancelling; /* UI thread only; worker never touches the owner/window */
} warm_job;
static warm_job *warm;

static DWORD WINAPI warm_work(void *arg)
{
    warm_job *j=arg;HRESULT hr=S_OK;HWND h=NULL;int ole=0,registered=0;
    if(WaitForSingleObject(j->stop,0)==WAIT_OBJECT_0)goto finish;
    int64_t t=fpe_prof_begin();hr=OleInitialize(NULL);FPE_PROF_END("drop.warm.ole",t);
    if(FAILED(hr))goto finish;ole=1;
    if(WaitForSingleObject(j->stop,0)==WAIT_OBJECT_0)goto finish;
    h=CreateWindowExW(0,L"STATIC",L"",WS_POPUP,0,0,1,1,NULL,NULL,GetModuleHandleW(NULL),NULL);
    if(!h){hr=HRESULT_FROM_WIN32(GetLastError());goto finish;}
    warm_target *target=calloc(1,sizeof *target);
    if(!target){hr=E_OUTOFMEMORY;goto finish;}
    target->iface.lpVtbl=&warm_vtbl;target->refs=1;
    t=fpe_prof_begin();hr=RegisterDragDrop(h,&target->iface);FPE_PROF_END("drop.warm.register",t);
    registered=SUCCEEDED(hr);IDropTarget_Release(&target->iface);
    if(FAILED(hr))goto finish;
    InterlockedExchange(&j->result,S_OK);SetEvent(j->ready);
    for(;;){
        DWORD result=MsgWaitForMultipleObjectsEx(1,&j->stop,INFINITE,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        if(result==WAIT_OBJECT_0)break;
        if(result==WAIT_FAILED){hr=HRESULT_FROM_WIN32(GetLastError());break;}
        MSG msg;int quit=0;
        while(PeekMessageW(&msg,NULL,0,0,PM_REMOVE)){
            if(msg.message==WM_QUIT){quit=1;break;}
            TranslateMessage(&msg);DispatchMessageW(&msg);
        }
        if(quit){hr=E_ABORT;break;}
    }
finish:
    if(registered){int64_t t=fpe_prof_begin();HRESULT revoked=RevokeDragDrop(h);FPE_PROF_END("drop.warm.revoke",t);if(FAILED(revoked))hr=revoked;}
    if(h)DestroyWindow(h);
    if(ole)OleUninitialize();
    InterlockedExchange(&j->result,hr);SetEvent(j->ready);
    return 0;
}
int dropwarm_start(void)
{
    if(warm)return -1;
    warm_job *j=calloc(1,sizeof *j);if(!j)return -1;
    j->result=E_PENDING;
    j->ready=CreateEventW(NULL,TRUE,FALSE,NULL);j->stop=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(j->ready&&j->stop)j->thread=CreateThread(NULL,0,warm_work,j,0,NULL);
    if(!j->thread){if(j->ready)CloseHandle(j->ready);if(j->stop)CloseHandle(j->stop);free(j);return -1;}
    warm=j;return 0;
}
int dropwarm_stop(void)
{
    if(!warm)return 0;
    warm->cancelling=1;SetEvent(warm->stop);return 1;
}
HANDLE dropwarm_wait_handle(void)
{
    return !warm?NULL:warm->observed?warm->thread:warm->ready;
}
int dropwarm_collect(HRESULT *error)
{
    warm_job *j=warm;if(!j)return DROPWARM_NONE;
    if(WaitForSingleObject(j->thread,0)==WAIT_OBJECT_0){
        HRESULT hr=(HRESULT)InterlockedCompareExchange(&j->result,0,0);int cancelled=j->cancelling;
        CloseHandle(j->thread);CloseHandle(j->ready);CloseHandle(j->stop);free(j);warm=NULL;
        if(error)*error=hr;
        return cancelled?DROPWARM_DONE:DROPWARM_FAILED;
    }
    if(!j->observed&&WaitForSingleObject(j->ready,0)==WAIT_OBJECT_0){
        j->observed=1;
        if(!j->cancelling&&SUCCEEDED((HRESULT)InterlockedCompareExchange(&j->result,0,0)))return DROPWARM_READY;
    }
    return DROPWARM_NONE;
}
