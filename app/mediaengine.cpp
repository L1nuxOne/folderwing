/* Windows media engines, owned solely by the disposable fpmedia process.
 * Decode/render calls never run in the explorer process. */
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include "mediaengine.h"

#include <dxgi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <mferror.h>
/* Exported desktop interop API omitted by this MinGW header snapshot. */
extern "C" HRESULT WINAPI MFCreateMFByteStreamOnStream(IStream *,IMFByteStream **);

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_d3d11.h>

void *operator new(size_t n){return malloc(n);}
void operator delete(void *p) noexcept{free(p);}
void operator delete(void *p,size_t) noexcept{free(p);}
template<class T> static void drop(T *&p){if(p){p->Release();p=NULL;}}
static LONGLONG now(){LARGE_INTEGER t;QueryPerformanceCounter(&t);return t.QuadPart;}
static struct {
 HANDLE quit,renderer,render_ready;HMODULE dll;bool mpv,audio_only;media_frame_callback callback;void *callback_user;
 wchar_t file[32768],report[32768],bmp[32768];int width,height,observe_ms,native_width,native_height;double gpu_decode,gpu_3d;
 LONGLONG parent,entry,initialized,opened,ready,seek_start,seek_end;volatile LONG64 first,metadata,loaded,played,seeked;
 volatile LONG error,render_error,frames;uint64_t first_hash,seek_hash;uint32_t first_nonblack;
 ID3D11Device *device;ID3D11DeviceContext *context;IDXGISwapChain *swap;ID3D11Texture2D *target,*staging;ID3D11RenderTargetView *view;
 IMFByteStream *stream;IMFDXGIDeviceManager *manager;IMFMediaEngine *mf;IMFMediaEngineEx *mfex;
 mpv_handle *mp;mpv_render_context *render;char renderer_name[256],decoder[128],version[256],log[4096];
} A;
#define MPV_API(X) X(mpv_create) X(mpv_initialize) X(mpv_terminate_destroy) X(mpv_set_option_string) X(mpv_set_property) X(mpv_get_property) X(mpv_get_property_string) X(mpv_free) X(mpv_wait_event) X(mpv_command) X(mpv_request_log_messages) X(mpv_render_context_create) X(mpv_render_context_update) X(mpv_render_context_render) X(mpv_render_context_free) X(mpv_render_context_report_swap)
#define DECLARE(n) static decltype(&n) p_##n;
MPV_API(DECLARE)
#undef DECLARE
struct Notify final:IMFMediaEngineNotify {
 LONG refs=1;
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void **out) override{if(id==__uuidof(IUnknown)||id==__uuidof(IMFMediaEngineNotify)){*out=this;AddRef();return S_OK;}*out=NULL;return E_NOINTERFACE;}
 ULONG STDMETHODCALLTYPE AddRef() override{return InterlockedIncrement(&refs);}
 ULONG STDMETHODCALLTYPE Release() override{LONG n=InterlockedDecrement(&refs);if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE EventNotify(DWORD event,DWORD_PTR p1,DWORD p2) override{
  if(event==MF_MEDIA_ENGINE_EVENT_NOTIFYSTABLESTATE){SetEvent((HANDLE)p1);return S_OK;}
  if(event==MF_MEDIA_ENGINE_EVENT_ERROR)InterlockedExchange(&A.error,p2?(LONG)p2:(LONG)E_FAIL);
  if(event==MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA)InterlockedCompareExchange64(&A.metadata,now(),0);
  if(event==MF_MEDIA_ENGINE_EVENT_LOADEDDATA)InterlockedCompareExchange64(&A.loaded,now(),0);
  if(event==MF_MEDIA_ENGINE_EVENT_PLAYING)InterlockedExchange64(&A.played,now());
  if(event==MF_MEDIA_ENGINE_EVENT_SEEKED)InterlockedExchange64(&A.seeked,now());
  return S_OK;
 }
};
static HRESULT graphics(){
 D3D_FEATURE_LEVEL level;UINT flags=D3D11_CREATE_DEVICE_BGRA_SUPPORT|D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
 HRESULT hr=D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,flags,NULL,0,D3D11_SDK_VERSION,&A.device,&level,&A.context);if(FAILED(hr))return hr;
 ID3D11Multithread *multi=NULL;if(SUCCEEDED(A.context->QueryInterface(__uuidof(ID3D11Multithread),(void**)&multi))){multi->SetMultithreadProtected(TRUE);drop(multi);}
 D3D11_TEXTURE2D_DESC td={};td.Width=A.width;td.Height=A.height;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_RENDER_TARGET;
 hr=A.device->CreateTexture2D(&td,NULL,&A.target);if(SUCCEEDED(hr))hr=A.device->CreateRenderTargetView(A.target,NULL,&A.view);if(FAILED(hr))return hr;
 td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;hr=A.device->CreateTexture2D(&td,NULL,&A.staging);
 float black[4]={0,0,0,1};A.context->ClearRenderTargetView(A.view,black);return hr;
}
static void publish_frame(){
 A.context->CopyResource(A.staging,A.target);D3D11_MAPPED_SUBRESOURCE mapped={};if(FAILED(A.context->Map(A.staging,0,D3D11_MAP_READ,0,&mapped)))return;
 if(A.callback)A.callback(mapped.pData,A.width,A.height,mapped.RowPitch,A.callback_user);
 A.context->Unmap(A.staging,0);InterlockedCompareExchange64(&A.first,now(),0);
}
static DWORD WINAPI render_thread(void*){
 HRESULT hr=S_OK;
 if(A.mpv){mpv_d3d11_init_params init={A.device};mpv_render_param parameters[]={{MPV_RENDER_PARAM_API_TYPE,(void*)MPV_RENDER_API_TYPE_D3D11},{MPV_RENDER_PARAM_D3D11_INIT_PARAMS,&init},{MPV_RENDER_PARAM_INVALID,NULL}};int error=p_mpv_render_context_create(&A.render,A.mp,parameters);if(error<0)hr=E_FAIL;}
 InterlockedExchange(&A.render_error,hr);SetEvent(A.render_ready);if(FAILED(hr))return 0;
 while(WaitForSingleObject(A.quit,0)!=WAIT_OBJECT_0){bool frame=false;
  if(A.mpv){if(p_mpv_render_context_update(A.render)&MPV_RENDER_UPDATE_FRAME){mpv_d3d11_fbo target={A.target};int block=0;mpv_render_param p[]={{MPV_RENDER_PARAM_D3D11_FBO,&target},{MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME,&block},{MPV_RENDER_PARAM_INVALID,NULL}};if(p_mpv_render_context_render(A.render,p)<0){InterlockedExchange(&A.render_error,E_FAIL);break;}frame=true;}}
  else{LONGLONG pts=0;if(A.mf->OnVideoStreamTick(&pts)==S_OK){MFVideoNormalizedRect source={0,0,1,1};RECT dest={0,0,A.width,A.height};DWORD nw=0,nh=0;A.mf->GetNativeVideoSize(&nw,&nh);if(nw&&nh){double scale=fmin((double)A.width/nw,(double)A.height/nh);int w=(int)(nw*scale),h=(int)(nh*scale);dest={(A.width-w)/2,(A.height-h)/2,(A.width+w)/2,(A.height+h)/2};}MFARGB border={0,0,0,255};hr=A.mf->TransferVideoFrame(A.target,&source,&dest,&border);if(FAILED(hr)){InterlockedExchange(&A.render_error,hr);break;}frame=true;}}
  if(frame){publish_frame();InterlockedIncrement(&A.frames);if(A.mpv)p_mpv_render_context_report_swap(A.render);}
  Sleep(2);
 }
 if(A.render){p_mpv_render_context_free(A.render);A.render=NULL;}return 0;
}
/* Give MF a real read-only stream, not a URL interpretation of the filename.
 * This preserves extended paths, Unicode and literal #/% characters. */
class FileStream final:public IStream {
 LONG refs=1;HANDLE file;CRITICAL_SECTION lock;
public:
 explicit FileStream(HANDLE f):file(f){InitializeCriticalSection(&lock);}
 ~FileStream(){CloseHandle(file);DeleteCriticalSection(&lock);}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void **out) override{if(!out)return E_POINTER;if(id==__uuidof(IUnknown)||id==__uuidof(IStream)||id==__uuidof(ISequentialStream)){*out=this;AddRef();return S_OK;}*out=NULL;return E_NOINTERFACE;}
 ULONG STDMETHODCALLTYPE AddRef() override{return InterlockedIncrement(&refs);}
 ULONG STDMETHODCALLTYPE Release() override{LONG n=InterlockedDecrement(&refs);if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE Read(void *p,ULONG n,ULONG *read) override{DWORD got=0;EnterCriticalSection(&lock);BOOL ok=ReadFile(file,p,n,&got,NULL);DWORD error=ok?0:GetLastError();LeaveCriticalSection(&lock);if(read)*read=got;return !ok?HRESULT_FROM_WIN32(error):got==n?S_OK:S_FALSE;}
 HRESULT STDMETHODCALLTYPE Write(const void *,ULONG,ULONG *written) override{if(written)*written=0;return STG_E_ACCESSDENIED;}
 HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move,DWORD origin,ULARGE_INTEGER *position) override{if(origin>STREAM_SEEK_END)return STG_E_INVALIDFUNCTION;LARGE_INTEGER out;EnterCriticalSection(&lock);BOOL ok=SetFilePointerEx(file,move,&out,origin);DWORD error=ok?0:GetLastError();LeaveCriticalSection(&lock);if(ok&&position)position->QuadPart=out.QuadPart;return ok?S_OK:HRESULT_FROM_WIN32(error);}
 HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override{return STG_E_ACCESSDENIED;}
 HRESULT STDMETHODCALLTYPE CopyTo(IStream *,ULARGE_INTEGER,ULARGE_INTEGER *,ULARGE_INTEGER *) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE Commit(DWORD) override{return S_OK;}
 HRESULT STDMETHODCALLTYPE Revert() override{return STG_E_INVALIDFUNCTION;}
 HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER,ULARGE_INTEGER,DWORD) override{return STG_E_INVALIDFUNCTION;}
 HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER,ULARGE_INTEGER,DWORD) override{return STG_E_INVALIDFUNCTION;}
 HRESULT STDMETHODCALLTYPE Stat(STATSTG *s,DWORD) override{if(!s)return E_POINTER;memset(s,0,sizeof *s);LARGE_INTEGER size;if(!GetFileSizeEx(file,&size))return HRESULT_FROM_WIN32(GetLastError());s->type=STGTY_STREAM;s->cbSize.QuadPart=size.QuadPart;s->grfMode=STGM_READ|STGM_SHARE_DENY_NONE;return S_OK;}
 HRESULT STDMETHODCALLTYPE Clone(IStream **out) override{if(out)*out=NULL;return E_NOTIMPL;}
};
static HRESULT init_mf(){HRESULT hr=MFStartup(MF_VERSION,MFSTARTUP_FULL);if(FAILED(hr))return hr;IMFAttributes *attributes=NULL;IMFMediaEngineClassFactory *factory=NULL;Notify *notify=new Notify;
 hr=MFCreateAttributes(&attributes,6);UINT reset=0;if(SUCCEEDED(hr)&&!A.audio_only)hr=MFCreateDXGIDeviceManager(&reset,&A.manager);if(SUCCEEDED(hr)&&!A.audio_only)hr=A.manager->ResetDevice(A.device,reset);
 if(SUCCEEDED(hr))hr=attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK,notify);if(SUCCEEDED(hr))hr=attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT,DXGI_FORMAT_B8G8R8A8_UNORM);if(SUCCEEDED(hr)&&!A.audio_only)hr=attributes->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER,A.manager);
 if(SUCCEEDED(hr))hr=CoCreateInstance(CLSID_MFMediaEngineClassFactory,NULL,CLSCTX_INPROC_SERVER,__uuidof(IMFMediaEngineClassFactory),(void**)&factory);
 if(SUCCEEDED(hr))hr=factory->CreateInstance(0,attributes,&A.mf);notify->Release();drop(attributes);drop(factory);if(FAILED(hr))return hr;
 hr=A.mf->QueryInterface(__uuidof(IMFMediaEngineEx),(void**)&A.mfex);if(FAILED(hr))return hr;A.mf->SetAutoPlay(FALSE);A.mf->SetPreload(MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC);A.mf->SetMuted(TRUE);A.mf->SetVolume(.25);strcpy(A.version,"Windows Media Foundation / IMFMediaEngine");strcpy(A.decoder,A.audio_only?"audio only; no video device":"not exposed by IMFMediaEngine; hardware device supplied");return S_OK;
}
static HRESULT init_mpv(){wchar_t path[32768];GetModuleFileNameW(NULL,path,32768);wchar_t *slash=wcsrchr(path,L'\\');if(!slash)return E_FAIL;wcscpy(slash+1,L"libmpv-2.dll");A.dll=LoadLibraryExW(path,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);if(!A.dll)return HRESULT_FROM_WIN32(GetLastError());
#define LOAD(n) p_##n=(decltype(p_##n))GetProcAddress(A.dll,#n);if(!p_##n)return E_NOINTERFACE;
 MPV_API(LOAD)
#undef LOAD
 A.mp=p_mpv_create();if(!A.mp)return E_FAIL;if(A.audio_only)p_mpv_set_option_string(A.mp,"vid","no");
 const char *options[][2]={{"access-references","no"},{"autoload-files","no"},{"load-unsafe-playlists","no"},{"config","no"},{"load-scripts","no"},{"terminal","no"},{"osc","no"},{"osd-level","0"},{"input-default-bindings","no"},{"input-vo-keyboard","no"},{"pause","yes"},{"mute","yes"},{"volume","25"},{"keep-open","yes"},{"vo","libmpv"},{"hwdec","auto"},{"network-timeout","5"},{"ytdl","no"}};
 for(size_t i=0;i<sizeof options/sizeof *options;i++){
  int error=p_mpv_set_option_string(A.mp,options[i][0],options[i][1]);
  // These controls only exist when their script interpreters are compiled in.
  // A minimal DLL has no interpreter to disable. All other failures stay fatal.
  if(error==MPV_ERROR_OPTION_NOT_FOUND&&(!strcmp(options[i][0],"load-scripts")||!strcmp(options[i][0],"osc")||!strcmp(options[i][0],"ytdl")))continue;
  if(error<0){snprintf(A.log,sizeof A.log,"mpv option %s rejected (%d)",options[i][0],error);return E_INVALIDARG;}
 }
 if(p_mpv_initialize(A.mp)<0)return E_FAIL;p_mpv_request_log_messages(A.mp,"warn");char *version=p_mpv_get_property_string(A.mp,"mpv-version");if(version){strncpy(A.version,version,sizeof A.version-1);p_mpv_free(version);}return S_OK;
}
static double position(){double value=0;if(A.mpv){p_mpv_get_property(A.mp,"time-pos",MPV_FORMAT_DOUBLE,&value);}else value=A.mf->GetCurrentTime();return value;}
static double duration(){double value=0;if(A.mpv)p_mpv_get_property(A.mp,"duration",MPV_FORMAT_DOUBLE,&value);else value=A.mf->GetDuration();return value;}
static void pause(bool value){if(A.mpv){int flag=value;p_mpv_set_property(A.mp,"pause",MPV_FORMAT_FLAG,&flag);}else{if(value)A.mf->Pause();else A.mf->Play();}}
static void volume(double value){if(A.mpv){value*=100;p_mpv_set_property(A.mp,"volume",MPV_FORMAT_DOUBLE,&value);}else A.mf->SetVolume(value);}
static double volume_value(){double value=0;if(A.mpv){p_mpv_get_property(A.mp,"volume",MPV_FORMAT_DOUBLE,&value);return value/100;}return A.mf->GetVolume();}
static void mute(bool value){if(A.mpv){int flag=value;p_mpv_set_property(A.mp,"mute",MPV_FORMAT_FLAG,&flag);}else A.mf->SetMuted(value);}
static bool muted(){if(!A.mpv)return A.mf->GetMuted()!=0;int value=0;p_mpv_get_property(A.mp,"mute",MPV_FORMAT_FLAG,&value);return value!=0;}
static void seek(double value){InterlockedExchange64(&A.seeked,0);A.seek_hash=0;if(A.mpv){char time[80];snprintf(time,sizeof time,"%.6f",value);const char *command[]={"seek",time,"absolute+exact",NULL};p_mpv_command(A.mp,command);}else A.mf->SetCurrentTime(value);}
static void events(){ if(!A.mpv||!A.mp)return;for(;;){mpv_event *e=p_mpv_wait_event(A.mp,0);if(e->event_id==MPV_EVENT_NONE)break;if(e->event_id==MPV_EVENT_FILE_LOADED){InterlockedCompareExchange64(&A.metadata,now(),0);InterlockedCompareExchange64(&A.loaded,now(),0);}if(e->event_id==MPV_EVENT_PLAYBACK_RESTART){InterlockedExchange64(&A.played,now());if(A.seek_start)InterlockedExchange64(&A.seeked,now());}if(e->event_id==MPV_EVENT_END_FILE){mpv_event_end_file *end=(mpv_event_end_file*)e->data;if(end->error<0)InterlockedExchange(&A.error,end->error);}if(e->event_id==MPV_EVENT_LOG_MESSAGE){mpv_event_log_message *log=(mpv_event_log_message*)e->data;size_t used=strlen(A.log);if(used<sizeof A.log-2)snprintf(A.log+used,sizeof A.log-used,"%s: %s",log->prefix,log->text);}}
}
static bool has_video(){if(A.mpv){int64_t w=0;p_mpv_get_property(A.mp,"width",MPV_FORMAT_INT64,&w);return w>0;}return A.mf->HasVideo()!=0;}
static bool has_audio(){if(A.mpv){int64_t count=0;p_mpv_get_property(A.mp,"track-list/count",MPV_FORMAT_INT64,&count);if(count>128)count=128;for(int64_t i=0;i<count;i++){char property[64];snprintf(property,sizeof property,"track-list/%lld/type",(long long)i);char *kind=p_mpv_get_property_string(A.mp,property);bool audio=kind&&!strcmp(kind,"audio");p_mpv_free(kind);if(audio)return true;}return false;}return A.mf->HasAudio()!=0;}
static int get_dim(bool height){if(A.mpv){int64_t n=0;p_mpv_get_property(A.mp,height?"height":"width",MPV_FORMAT_INT64,&n);return (int)n;}DWORD w=0,h=0;A.mf->GetNativeVideoSize(&w,&h);return (int)(height?h:w);}

extern "C" int32_t media_engine_open(const wchar_t *path,int mpv,uint32_t width,uint32_t height,uint32_t flags,media_frame_callback callback,void *user){
 memset(&A,0,sizeof A);A.mpv=mpv!=0;A.audio_only=(flags&MEDIAP_AUDIO)!=0;A.width=width;A.height=height;A.callback=callback;A.callback_user=user;
 A.quit=CreateEventW(NULL,TRUE,FALSE,NULL);A.render_ready=CreateEventW(NULL,TRUE,FALSE,NULL);if(!A.quit||!A.render_ready)return E_OUTOFMEMORY;
 HRESULT hr=A.audio_only?S_OK:graphics();if(SUCCEEDED(hr))hr=A.mpv?init_mpv():init_mf();if(FAILED(hr)){A.error=hr;return hr;}
 if(!A.audio_only){A.renderer=CreateThread(NULL,0,render_thread,NULL,0,NULL);if(!A.renderer||WaitForSingleObject(A.render_ready,10000)!=WAIT_OBJECT_0)return E_FAIL;if(A.render_error)return A.render_error;}
 if(A.mpv){int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path,-1,NULL,0,NULL,NULL);char *utf8=(char*)malloc(n?n:1);if(!utf8)return E_OUTOFMEMORY;int error=-1;if(n&&WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path,-1,utf8,n,NULL,NULL)){const char *cmd[]={"loadfile",utf8,"replace",NULL};error=p_mpv_command(A.mp,cmd);}free(utf8);if(error<0)return error;}
 else{HANDLE f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,NULL);if(f==INVALID_HANDLE_VALUE)return HRESULT_FROM_WIN32(GetLastError());FileStream *file=new FileStream(f);if(!file){CloseHandle(f);return E_OUTOFMEMORY;}hr=MFCreateMFByteStreamOnStream(file,&A.stream);file->Release();const wchar_t *leaf=wcsrchr(path,L'\\');BSTR hint=SysAllocString(leaf?leaf+1:path);if(SUCCEEDED(hr))hr=hint?A.mfex->SetSourceFromByteStream(A.stream,hint):E_OUTOFMEMORY;SysFreeString(hint);if(FAILED(hr))return hr;}
 return 0;
}
extern "C" void media_engine_poll(mediap_state *s){
 events();memset(s,0,sizeof *s);s->backend=A.mpv?2:1;s->error=A.error?A.error:A.render_error;s->state=s->error?MEDIAP_FAILED:MEDIAP_LOADING;
 if(s->error)return;
 if(A.loaded){s->has_video=has_video();s->has_audio=has_audio();s->width=get_dim(false);s->height=get_dim(true);double len=duration();if(isfinite(len)&&len>0)s->duration=len;
  if(s->has_video?A.first!=0:s->has_audio!=0)s->state=MEDIAP_READY;
  s->position=position();if(!isfinite(s->position)||s->position<0)s->position=0;s->volume=volume_value();s->muted=muted();
  if(A.mpv){int value=1;p_mpv_get_property(A.mp,"pause",MPV_FORMAT_FLAG,&value);s->paused=value;int eof=0;p_mpv_get_property(A.mp,"eof-reached",MPV_FORMAT_FLAG,&eof);s->ended=eof;}
  else{s->paused=A.mf->IsPaused();s->ended=A.mf->IsEnded();}
 }
}
extern "C" void media_engine_control(uint32_t action,double value){if(!isfinite(value))return;switch(action){case MEDIA_PLAY:pause(false);break;case MEDIA_PAUSE:pause(true);break;case MEDIA_SEEK:seek(fmax(0,value));break;case MEDIA_VOLUME:volume(fmax(0,fmin(1,value)));break;case MEDIA_MUTE:mute(value!=0);break;}}
extern "C" void media_engine_close(void){
 if(A.quit)SetEvent(A.quit);if(A.renderer){if(WaitForSingleObject(A.renderer,5000)!=WAIT_OBJECT_0)ExitProcess(3);CloseHandle(A.renderer);}
 if(A.mf)A.mf->Shutdown();drop(A.mfex);drop(A.mf);drop(A.stream);if(!A.mpv)MFShutdown();
 if(A.mp){p_mpv_terminate_destroy(A.mp);A.mp=NULL;}
 drop(A.manager);drop(A.view);drop(A.staging);drop(A.target);drop(A.swap);drop(A.context);drop(A.device);
 if(A.dll)FreeLibrary(A.dll);if(A.quit)CloseHandle(A.quit);if(A.render_ready)CloseHandle(A.render_ready);memset(&A,0,sizeof A);
}
