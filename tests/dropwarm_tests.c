/* Real OLE worker lifecycle with fault injection confined to this executable. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <stdio.h>
static DWORD owner_thread;
static int fail_event,events,fail_thread,fail_ole,fail_window,fail_register;
static volatile LONG registrations,revocations,ole_starts,ole_stops,wrong_thread;
static HANDLE entered,release_register;
static HANDLE test_event(LPSECURITY_ATTRIBUTES a,BOOL manual,BOOL initial,LPCWSTR name)
{ if(++events==fail_event){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return NULL;}return CreateEventW(a,manual,initial,name); }
static HANDLE test_thread(LPSECURITY_ATTRIBUTES a,SIZE_T size,LPTHREAD_START_ROUTINE f,LPVOID p,DWORD flags,LPDWORD id)
{ if(fail_thread){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return NULL;}return CreateThread(a,size,f,p,flags,id); }
static HRESULT test_ole(LPVOID p)
{ if(GetCurrentThreadId()==owner_thread)InterlockedIncrement(&wrong_thread);if(fail_ole)return E_FAIL;HRESULT hr=OleInitialize(p);if(SUCCEEDED(hr))InterlockedIncrement(&ole_starts);return hr; }
static void test_unole(void){InterlockedIncrement(&ole_stops);OleUninitialize();}
static HWND test_window(DWORD ex,LPCWSTR cls,LPCWSTR name,DWORD style,int x,int y,int w,int h,HWND parent,HMENU menu,HINSTANCE inst,LPVOID param)
{ if(fail_window){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return NULL;}return CreateWindowExW(ex,cls,name,style,x,y,w,h,parent,menu,inst,param); }
static HRESULT test_register(HWND h,IDropTarget *target)
{
    if(GetCurrentThreadId()==owner_thread)InterlockedIncrement(&wrong_thread);
    if(entered){SetEvent(entered);if(WaitForSingleObject(release_register,5000)!=WAIT_OBJECT_0)return E_ABORT;}
    if(fail_register)return E_FAIL;
    HRESULT hr=RegisterDragDrop(h,target);if(SUCCEEDED(hr))InterlockedIncrement(&registrations);return hr;
}
static HRESULT test_revoke(HWND h)
{if(GetCurrentThreadId()==owner_thread)InterlockedIncrement(&wrong_thread);HRESULT hr=RevokeDragDrop(h);if(SUCCEEDED(hr))InterlockedIncrement(&revocations);return hr;}
#define CreateEventW test_event
#define CreateThread test_thread
#define OleInitialize test_ole
#define OleUninitialize test_unole
#define CreateWindowExW test_window
#define RegisterDragDrop test_register
#define RevokeDragDrop test_revoke
#include "../app/dropwarm.c"
#undef CreateEventW
#undef CreateThread
#undef OleInitialize
#undef OleUninitialize
#undef CreateWindowExW
#undef RegisterDragDrop
#undef RevokeDragDrop
static int passed,failed;
#define CHECK(name,x) do {if(x){passed++;printf("  ok    %s\n",name);}else{failed++;printf("  FAIL  %s line %d\n",name,__LINE__);}}while(0)
static int collect_until(int expected)
{
    uint64_t end=GetTickCount64()+5000;
    while(GetTickCount64()<end){
        HANDLE wait=dropwarm_wait_handle();if(!wait)return 0;
        DWORD result=MsgWaitForMultipleObjectsEx(1,&wait,50,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        if(result==WAIT_OBJECT_0){HRESULT error;int status=dropwarm_collect(&error);if(status==expected)return 1;if(status==DROPWARM_DONE||status==DROPWARM_FAILED)return 0;}
        MSG msg;while(PeekMessageW(&msg,NULL,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
    }
    return 0;
}
int wmain(void)
{
    owner_thread=GetCurrentThreadId();if(FAILED(OleInitialize(NULL)))return 2;
    CHECK("disabled/unrequested warmup owns no wait handle",!dropwarm_wait_handle()&&!dropwarm_stop());
    for(int fault=1;fault<=2;fault++){
        DWORD before=0,after=0;GetProcessHandleCount(GetCurrentProcess(),&before);events=0;fail_event=fault;
        CHECK("event creation failure retains no worker",dropwarm_start()<0&&!dropwarm_wait_handle());
        GetProcessHandleCount(GetCurrentProcess(),&after);CHECK("event failure closes all created handles",before==after);
    }
    fail_event=0;fail_thread=1;
    CHECK("thread creation failure retains no resources",dropwarm_start()<0&&!dropwarm_wait_handle());fail_thread=0;
    fail_ole=1;CHECK("OLE failure starts asynchronously",dropwarm_start()==0);
    CHECK("OLE failure is collected only after worker exit",collect_until(DROPWARM_FAILED)&&!dropwarm_wait_handle());fail_ole=0;
    fail_window=1;CHECK("window failure starts asynchronously",dropwarm_start()==0);
    CHECK("window failure releases the worker apartment",collect_until(DROPWARM_FAILED)&&ole_starts==ole_stops);fail_window=0;
    fail_register=1;CHECK("registration failure starts asynchronously",dropwarm_start()==0);
    CHECK("registration failure releases the worker apartment",collect_until(DROPWARM_FAILED)&&ole_starts==ole_stops);fail_register=0;
    entered=CreateEventW(NULL,TRUE,FALSE,NULL);release_register=CreateEventW(NULL,TRUE,FALSE,NULL);
    CHECK("held registration starts on the worker",dropwarm_start()==0&&WaitForSingleObject(entered,5000)==WAIT_OBJECT_0);
    LARGE_INTEGER first,last,freq;QueryPerformanceFrequency(&freq);QueryPerformanceCounter(&first);
    int cancelled=dropwarm_stop();QueryPerformanceCounter(&last);
    CHECK("cancellation returns while native registration is held",cancelled&&dropwarm_wait_handle()&&(last.QuadPart-first.QuadPart)*1000.0/freq.QuadPart<16);
    CHECK("cancellation is idempotent and blocks a replacement generation",dropwarm_stop()&&dropwarm_start()<0);
    SetEvent(release_register);
    CHECK("early cancellation drains registration and revocation",collect_until(DROPWARM_DONE)&&!dropwarm_wait_handle()&&registrations==revocations&&ole_starts==ole_stops);
    CloseHandle(entered);CloseHandle(release_register);entered=release_register=NULL;
    CHECK("reenable creates one new owned worker",dropwarm_start()==0&&collect_until(DROPWARM_READY));
    CHECK("ready worker retains its pumping apartment",dropwarm_wait_handle()&&registrations==revocations+1&&ole_starts==ole_stops+1);
    CHECK("stopping a ready worker is asynchronous",dropwarm_stop()&&collect_until(DROPWARM_DONE));
    CHECK("completed cleanup releases every native target and apartment",!dropwarm_wait_handle()&&registrations==revocations&&ole_starts==ole_stops);
    for(int i=0;i<8;i++){
        CHECK("rapid enable-disable can cancel before readiness",dropwarm_start()==0&&dropwarm_stop()&&collect_until(DROPWARM_DONE));
    }
    CHECK("all OLE registration and cleanup stayed off the UI thread",wrong_thread==0);
    CHECK("rapid generations leave no targets or apartments behind",registrations==revocations&&ole_starts==ole_stops&&!dropwarm_wait_handle());
    OleUninitialize();printf("%d passed, %d failed\n",passed,failed);return failed?1:0;
}
