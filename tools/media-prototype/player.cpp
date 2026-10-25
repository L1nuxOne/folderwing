/* Independent, reversible engine experiment. No production app linkage.
 * Both engines render on a dedicated thread into the same D3D11 texture.
 * First-frame means verified non-black GPU readback, NOT DWM presentation. */
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <mferror.h>
#include <psapi.h>
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
static LONGLONG frequency(){LARGE_INTEGER f;QueryPerformanceFrequency(&f);return f.QuadPart;}
static double ms(LONGLONG a,LONGLONG b){return b?1000.0*(b-a)/frequency():-1;}
static unsigned long long cpu100(){FILETIME c,e,k,u;GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u);return ((uint64_t)k.dwHighDateTime<<32|k.dwLowDateTime)+((uint64_t)u.dwHighDateTime<<32|u.dwLowDateTime);}
struct Memory{SIZE_T private_bytes,working_set,peak_private,peak_working_set;};
static Memory memory(){PROCESS_MEMORY_COUNTERS_EX m={};m.cb=sizeof m;GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&m,sizeof m);return {m.PrivateUsage,m.WorkingSetSize,m.PeakPagefileUsage,m.PeakWorkingSetSize};}
static struct {
 HWND window,video;HANDLE quit,renderer,render_ready;HMODULE dll;bool mpv,visible,software,manual,audio_only;
 wchar_t file[32768],report[32768],bmp[32768];int width,height,observe_ms,native_width,native_height;double gpu_decode,gpu_3d;
 LONGLONG parent,entry,initialized,opened,ready,seek_start,seek_end;volatile LONG64 first,metadata,loaded,played,seeked;
 volatile LONG error,render_error,frames;uint64_t first_hash,seek_hash;uint32_t first_nonblack;
 ID3D11Device *device;ID3D11DeviceContext *context;IDXGISwapChain *swap;ID3D11Texture2D *target,*staging;ID3D11RenderTargetView *view;
 IMFDXGIDeviceManager *manager;IMFMediaEngine *mf;IMFMediaEngineEx *mfex;
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
 IDXGIDevice *dx=NULL;IDXGIAdapter *adapter=NULL;IDXGIFactory *factory=NULL;
 hr=A.device->QueryInterface(__uuidof(IDXGIDevice),(void**)&dx);if(SUCCEEDED(hr))hr=dx->GetAdapter(&adapter);if(SUCCEEDED(hr))hr=adapter->GetParent(__uuidof(IDXGIFactory),(void**)&factory);
 if(adapter){DXGI_ADAPTER_DESC d={};adapter->GetDesc(&d);WideCharToMultiByte(CP_UTF8,0,d.Description,-1,A.renderer_name,sizeof A.renderer_name,NULL,NULL);}
 DXGI_SWAP_CHAIN_DESC d={};d.BufferDesc.Width=A.width;d.BufferDesc.Height=A.height;d.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.SampleDesc.Count=1;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.BufferCount=2;d.OutputWindow=A.video;d.Windowed=TRUE;d.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
 if(SUCCEEDED(hr))hr=factory->CreateSwapChain(A.device,&d,&A.swap);drop(factory);drop(adapter);drop(dx);if(FAILED(hr))return hr;
 hr=A.swap->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&A.target);if(SUCCEEDED(hr))hr=A.device->CreateRenderTargetView(A.target,NULL,&A.view);if(FAILED(hr))return hr;
 D3D11_TEXTURE2D_DESC td;A.target->GetDesc(&td);td.BindFlags=0;td.MiscFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;hr=A.device->CreateTexture2D(&td,NULL,&A.staging);if(FAILED(hr))return hr;
 float black[4]={0,0,0,1};A.context->ClearRenderTargetView(A.view,black);return S_OK;
}
static uint64_t capture(int *nonblack,bool save){
 A.context->CopyResource(A.staging,A.target);D3D11_MAPPED_SUBRESOURCE mapped={};if(FAILED(A.context->Map(A.staging,0,D3D11_MAP_READ,0,&mapped)))return 0;
 uint64_t hash=14695981039346656037ull;int count=0;for(int y=0;y<A.height;y+=8){uint8_t *row=(uint8_t*)mapped.pData+(size_t)y*mapped.RowPitch;for(int x=0;x<A.width;x+=8){uint8_t *p=row+x*4;if(p[0]+p[1]+p[2]>24)count++;for(int c=0;c<3;c++){hash^=p[c];hash*=1099511628211ull;}}}
 if(save&&A.bmp[0]){HANDLE f=CreateFileW(A.bmp,GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);if(f!=INVALID_HANDLE_VALUE){BITMAPINFOHEADER bi={sizeof bi,A.width,-A.height,1,32,BI_RGB};BITMAPFILEHEADER fh={};fh.bfType=0x4d42;fh.bfOffBits=sizeof fh+sizeof bi;fh.bfSize=fh.bfOffBits+A.width*A.height*4;DWORD n;WriteFile(f,&fh,sizeof fh,&n,NULL);WriteFile(f,&bi,sizeof bi,&n,NULL);for(int y=0;y<A.height;y++)WriteFile(f,(uint8_t*)mapped.pData+(size_t)y*mapped.RowPitch,A.width*4,&n,NULL);CloseHandle(f);}}
 A.context->Unmap(A.staging,0);*nonblack=count;return hash;
}
static DWORD WINAPI render_thread(void*){
 HRESULT hr=S_OK;
 if(A.mpv){mpv_d3d11_init_params init={A.device};mpv_render_param parameters[]={{MPV_RENDER_PARAM_API_TYPE,(void*)MPV_RENDER_API_TYPE_D3D11},{MPV_RENDER_PARAM_D3D11_INIT_PARAMS,&init},{MPV_RENDER_PARAM_INVALID,NULL}};int error=p_mpv_render_context_create(&A.render,A.mp,parameters);if(error<0)hr=E_FAIL;}
 InterlockedExchange(&A.render_error,hr);SetEvent(A.render_ready);if(FAILED(hr))return 0;
 while(WaitForSingleObject(A.quit,0)!=WAIT_OBJECT_0){bool frame=false;
  if(A.mpv){if(p_mpv_render_context_update(A.render)&MPV_RENDER_UPDATE_FRAME){mpv_d3d11_fbo target={A.target};int block=0;mpv_render_param p[]={{MPV_RENDER_PARAM_D3D11_FBO,&target},{MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME,&block},{MPV_RENDER_PARAM_INVALID,NULL}};if(p_mpv_render_context_render(A.render,p)<0){InterlockedExchange(&A.render_error,E_FAIL);break;}frame=true;}}
  else{LONGLONG pts=0;if(A.mf->OnVideoStreamTick(&pts)==S_OK){MFVideoNormalizedRect source={0,0,1,1};RECT dest={0,0,A.width,A.height};MFARGB border={0,0,0,255};hr=A.mf->TransferVideoFrame(A.target,&source,&dest,&border);if(FAILED(hr)){InterlockedExchange(&A.render_error,hr);break;}frame=true;}}
  if(frame){if(!A.first){int nonblack=0;uint64_t hash=capture(&nonblack,false);if(nonblack>8){A.first_hash=hash;A.first_nonblack=(uint32_t)nonblack;InterlockedCompareExchange64(&A.first,now(),0);}}
   if(A.seeked&&!A.seek_hash){int ignored=0;A.seek_hash=capture(&ignored,A.bmp[0]!=0);}
   InterlockedIncrement(&A.frames);if(A.visible)A.swap->Present(0,0);if(A.mpv)p_mpv_render_context_report_swap(A.render);}
  Sleep(2);
 }
 if(A.render){p_mpv_render_context_free(A.render);A.render=NULL;}return 0;
}
static PDH_HQUERY gpu_query;static PDH_HCOUNTER gpu_counters[2];
static void gpu_begin(){if(PdhOpenQueryW(NULL,0,&gpu_query)!=ERROR_SUCCESS)return;const wchar_t *names[]={L"*Video*",L"3D"};for(int i=0;i<2;i++){wchar_t path[256];swprintf(path,256,L"\\GPU Engine(pid_%lu_*engtype_%ls)\\Utilization Percentage",(unsigned long)GetCurrentProcessId(),names[i]);PdhAddEnglishCounterW(gpu_query,path,0,&gpu_counters[i]);}PdhCollectQueryData(gpu_query);}
static void gpu_end(){A.gpu_decode=A.gpu_3d=-1;if(!gpu_query)return;PdhCollectQueryData(gpu_query);for(int i=0;i<2;i++){DWORD bytes=0,count=0;if(!gpu_counters[i])continue;PdhGetFormattedCounterArrayW(gpu_counters[i],PDH_FMT_DOUBLE|PDH_FMT_NOCAP100,&bytes,&count,NULL);if(!bytes)continue;auto *rows=(PDH_FMT_COUNTERVALUE_ITEM_W*)malloc(bytes);if(rows&&PdhGetFormattedCounterArrayW(gpu_counters[i],PDH_FMT_DOUBLE|PDH_FMT_NOCAP100,&bytes,&count,rows)==ERROR_SUCCESS){double total=0;bool valid=false;for(DWORD j=0;j<count;j++)if(rows[j].FmtValue.CStatus==PDH_CSTATUS_VALID_DATA||rows[j].FmtValue.CStatus==PDH_CSTATUS_NEW_DATA){total+=rows[j].FmtValue.doubleValue;valid=true;}if(valid)(i?A.gpu_3d:A.gpu_decode)=total;}free(rows);}PdhCloseQuery(gpu_query);gpu_query=NULL;}
static HRESULT init_mf(){HRESULT hr=MFStartup(MF_VERSION,MFSTARTUP_FULL);if(FAILED(hr))return hr;IMFAttributes *attributes=NULL;IMFMediaEngineClassFactory *factory=NULL;Notify *notify=new Notify;
 hr=MFCreateAttributes(&attributes,6);UINT reset=0;if(SUCCEEDED(hr)&&!A.audio_only)hr=MFCreateDXGIDeviceManager(&reset,&A.manager);if(SUCCEEDED(hr)&&!A.audio_only)hr=A.manager->ResetDevice(A.device,reset);
 if(SUCCEEDED(hr))hr=attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK,notify);if(SUCCEEDED(hr))hr=attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT,DXGI_FORMAT_B8G8R8A8_UNORM);if(SUCCEEDED(hr)&&!A.audio_only)hr=attributes->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER,A.manager);
 if(SUCCEEDED(hr))hr=CoCreateInstance(CLSID_MFMediaEngineClassFactory,NULL,CLSCTX_INPROC_SERVER,__uuidof(IMFMediaEngineClassFactory),(void**)&factory);
 if(SUCCEEDED(hr))hr=factory->CreateInstance(0,attributes,&A.mf);notify->Release();drop(attributes);drop(factory);if(FAILED(hr))return hr;
 A.mf->QueryInterface(__uuidof(IMFMediaEngineEx),(void**)&A.mfex);A.mf->SetAutoPlay(FALSE);A.mf->SetPreload(MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC);A.mf->SetMuted(TRUE);A.mf->SetVolume(.25);strcpy(A.version,"Windows Media Foundation / IMFMediaEngine");strcpy(A.decoder,A.audio_only?"audio only; no video device":"not exposed by IMFMediaEngine; hardware device supplied");return S_OK;
}
static HRESULT init_mpv(){wchar_t path[32768];GetModuleFileNameW(NULL,path,32768);wchar_t *slash=wcsrchr(path,L'\\');if(!slash)return E_FAIL;wcscpy(slash+1,L"libmpv-2.dll");A.dll=LoadLibraryExW(path,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);if(!A.dll)return HRESULT_FROM_WIN32(GetLastError());
#define LOAD(n) p_##n=(decltype(p_##n))GetProcAddress(A.dll,#n);if(!p_##n)return E_NOINTERFACE;
 MPV_API(LOAD)
#undef LOAD
 A.mp=p_mpv_create();if(!A.mp)return E_FAIL;if(A.audio_only)p_mpv_set_option_string(A.mp,"vid","no");
 const char *options[][2]={{"config","no"},{"load-scripts","no"},{"terminal","no"},{"osc","no"},{"osd-level","0"},{"input-default-bindings","no"},{"input-vo-keyboard","no"},{"pause","yes"},{"mute","yes"},{"volume","25"},{"keep-open","yes"},{"vo","libmpv"},{"hwdec",A.software?"no":"auto"},{"network-timeout","5"},{"ytdl","no"}};
 for(size_t i=0;i<sizeof options/sizeof *options;i++)if(p_mpv_set_option_string(A.mp,options[i][0],options[i][1])<0)return E_INVALIDARG;
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
static void events(){MSG m;while(PeekMessageW(&m,NULL,0,0,PM_REMOVE)){if(m.message==WM_QUIT)SetEvent(A.quit);else{TranslateMessage(&m);DispatchMessageW(&m);}}
 if(!A.mpv||!A.mp)return;for(;;){mpv_event *e=p_mpv_wait_event(A.mp,0);if(e->event_id==MPV_EVENT_NONE)break;if(e->event_id==MPV_EVENT_FILE_LOADED){InterlockedCompareExchange64(&A.metadata,now(),0);InterlockedCompareExchange64(&A.loaded,now(),0);}if(e->event_id==MPV_EVENT_PLAYBACK_RESTART){InterlockedExchange64(&A.played,now());if(A.seek_start)InterlockedExchange64(&A.seeked,now());}if(e->event_id==MPV_EVENT_END_FILE){mpv_event_end_file *end=(mpv_event_end_file*)e->data;if(end->error<0)InterlockedExchange(&A.error,end->error);}if(e->event_id==MPV_EVENT_LOG_MESSAGE){mpv_event_log_message *log=(mpv_event_log_message*)e->data;size_t used=strlen(A.log);if(used<sizeof A.log-2)snprintf(A.log+used,sizeof A.log-used,"%s: %s",log->prefix,log->text);}}
}
static void pump(int milliseconds){LONGLONG until=now()+frequency()*milliseconds/1000;while(now()<until&&!A.error&&!A.render_error&&WaitForSingleObject(A.quit,0)!=WAIT_OBJECT_0){events();Sleep(2);}}
static bool has_video(){if(A.mpv){int64_t w=0;p_mpv_get_property(A.mp,"width",MPV_FORMAT_INT64,&w);return w>0;}return A.mf->HasVideo()!=0;}
static bool has_audio(){if(A.mpv){char *v=p_mpv_get_property_string(A.mp,"audio-codec-name");bool yes=v&&*v;p_mpv_free(v);return yes;}return A.mf->HasAudio()!=0;}
static int get_dim(bool height){if(A.mpv){int64_t n=0;p_mpv_get_property(A.mp,height?"height":"width",MPV_FORMAT_INT64,&n);return (int)n;}DWORD w=0,h=0;A.mf->GetNativeVideoSize(&w,&h);return (int)(height?h:w);}
static void json_string(FILE *f,const char *s){fputc('"',f);for(;s&&*s;s++){unsigned char c=(unsigned char)*s;if(c=='"'||c=='\\'){fputc('\\',f);fputc(c,f);}else if(c<32)fprintf(f,"\\u%04x",c);else fputc(c,f);}fputc('"',f);}
static void save_report(bool ready,bool video,bool audio,double len,Memory initial,Memory loaded,unsigned long long cpu,double play_wall,double advance,bool volume_ok,bool pause_ok,bool seek_ok,bool resume_ok,double paused_drift,LONGLONG close_start,LONGLONG closed){
 FILE *f=_wfopen(A.report,L"wb");if(!f)return;Memory end=memory();fprintf(f,"{\n\"schema\":\"openpilot-media-prototype/v1\",\n\"engine\":\"%s\",\"ready\":%s,\"error\":%ld,\"render_error\":%ld,\n",A.mpv?"libmpv":"mf",ready?"true":"false",(long)A.error,(long)A.render_error);
 fprintf(f,"\"entry_from_launch_ms\":%.4f,\"init_ms\":%.4f,\"metadata_from_load_ms\":%.4f,\"loaded_from_load_ms\":%.4f,\"ready_from_launch_ms\":%.4f,\"first_verified_render_from_load_ms\":%.4f,\"first_verified_render_from_launch_ms\":%.4f,\n",ms(A.parent,A.entry),ms(A.entry,A.initialized),ms(A.opened,A.metadata),ms(A.opened,A.loaded),ms(A.parent,A.ready),ms(A.opened,A.first),ms(A.parent,A.first));
 fprintf(f,"\"has_video\":%s,\"has_audio\":%s,\"duration_s\":%.6f,\"output_width\":%d,\"output_height\":%d,\"first_frame_hash\":\"%016llx\",\"seek_frame_hash\":\"%016llx\",\"nonblack_samples\":%u,\"render_calls\":%ld,\n",video?"true":"false",audio?"true":"false",isfinite(len)?len:0,A.width,A.height,(unsigned long long)A.first_hash,(unsigned long long)A.seek_hash,A.first_nonblack,(long)A.frames);
 fprintf(f,"\"initial_private_bytes\":%llu,\"ready_private_bytes\":%llu,\"ready_working_set\":%llu,\"peak_private_bytes\":%llu,\"peak_working_set\":%llu,\"cpu_play_ms\":%.4f,\"play_wall_ms\":%.4f,\"play_position_advance_s\":%.6f,\"volume_mute_ok\":%s,\"pause_ok\":%s,\"pause_drift_s\":%.6f,\"seek_ok\":%s,\"resume_ok\":%s,\"seek_ms\":%.4f,\"engine_close_ms\":%.4f,\n",(unsigned long long)initial.private_bytes,(unsigned long long)loaded.private_bytes,(unsigned long long)loaded.working_set,(unsigned long long)end.peak_private,(unsigned long long)end.peak_working_set,cpu/10000.0,play_wall,advance,volume_ok?"true":"false",pause_ok?"true":"false",paused_drift,seek_ok?"true":"false",resume_ok?"true":"false",ms(A.seek_start,A.seek_end),ms(close_start,closed));
 fprintf(f,"\"native_width\":%d,\"native_height\":%d,\"gpu_video_engine_percent\":%.6f,\"gpu_3d_percent\":%.6f,\"audio_only_mode\":%s,",A.native_width,A.native_height,A.gpu_decode,A.gpu_3d,A.audio_only?"true":"false");fprintf(f,"\"decoder\":");json_string(f,A.decoder);fprintf(f,",\"adapter\":");json_string(f,A.renderer_name);fprintf(f,",\"version\":");json_string(f,A.version);fprintf(f,",\"warnings\":");json_string(f,A.log);fprintf(f,",\"dwm_presentation_measured\":false,\"audio_muted\":true,\"visible\":%s}\n",A.visible?"true":"false");fclose(f);
}
static LRESULT CALLBACK window_proc(HWND h,UINT m,WPARAM w,LPARAM l){if(m==WM_CLOSE){SetEvent(A.quit);return 0;}if(m==WM_COMMAND&&A.initialized){int id=LOWORD(w);if(id==101){bool paused=A.mpv?false:A.mf->IsPaused()!=0;if(A.mpv){int p=0;p_mpv_get_property(A.mp,"pause",MPV_FORMAT_FLAG,&p);paused=p!=0;}pause(!paused);}if(id==102)seek(fmax(0,position()-5));if(id==103)seek(fmin(duration()-.1,position()+5));if(id==104)mute(!muted());if(id==105)volume(fmax(0,volume_value()-.1));if(id==106)volume(fmin(1,volume_value()+.1));return 0;}return DefWindowProcW(h,m,w,l);}
int wmain(int argc,wchar_t **argv){A.entry=now();A.parent=A.entry;A.width=640;A.height=360;A.observe_ms=500;A.gpu_decode=A.gpu_3d=-1;A.quit=CreateEventW(NULL,TRUE,FALSE,NULL);A.render_ready=CreateEventW(NULL,TRUE,FALSE,NULL);
 for(int i=1;i<argc;i++){if(!wcscmp(argv[i],L"--mpv"))A.mpv=true;else if(!wcscmp(argv[i],L"--visible"))A.visible=true;else if(!wcscmp(argv[i],L"--manual"))A.manual=true;else if(!wcscmp(argv[i],L"--audio-only"))A.audio_only=true;else if(!wcscmp(argv[i],L"--software"))A.software=true;else if(i+1<argc&&!wcscmp(argv[i],L"--file"))wcsncpy(A.file,argv[++i],32767);else if(i+1<argc&&!wcscmp(argv[i],L"--report"))wcsncpy(A.report,argv[++i],32767);else if(i+1<argc&&!wcscmp(argv[i],L"--bmp"))wcsncpy(A.bmp,argv[++i],32767);else if(i+1<argc&&!wcscmp(argv[i],L"--parent-qpc"))A.parent=_wcstoi64(argv[++i],NULL,10);else if(i+1<argc&&!wcscmp(argv[i],L"--observe-ms"))A.observe_ms=_wtoi(argv[++i]);}
 if(!A.file[0]||!A.report[0])return 2;CoInitializeEx(NULL,COINIT_MULTITHREADED);SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
 WNDCLASSW cls={};cls.lpfnWndProc=window_proc;cls.hInstance=GetModuleHandleW(NULL);cls.hCursor=LoadCursorW(NULL,IDC_ARROW);cls.lpszClassName=L"OpenPilotMediaPrototype";RegisterClassW(&cls);
 A.window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,cls.lpszClassName,A.mpv?L"libmpv prototype":L"Media Foundation prototype",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,80,80,A.width+16,A.height+100,NULL,NULL,cls.hInstance,NULL);
 A.video=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE,0,0,A.width,A.height,A.window,NULL,cls.hInstance,NULL);
 const wchar_t *labels[]={L"Play / pause",L"Back 5s",L"Forward 5s",L"Mute / unmute",L"Volume −",L"Volume +"};for(int i=0;i<6;i++)CreateWindowExW(0,L"BUTTON",labels[i],WS_CHILD|WS_VISIBLE|WS_TABSTOP,8+i*105,A.height+8,100,28,A.window,(HMENU)(INT_PTR)(101+i),cls.hInstance,NULL);if(A.visible)ShowWindow(A.window,SW_SHOWNOACTIVATE);
 Memory initial=memory();HRESULT hr=A.audio_only?S_OK:graphics();if(SUCCEEDED(hr))hr=A.mpv?init_mpv():init_mf();if(FAILED(hr))A.error=hr;
 if(!A.error&&!A.audio_only){A.renderer=CreateThread(NULL,0,render_thread,NULL,0,NULL);if(!A.renderer||WaitForSingleObject(A.render_ready,15000)!=WAIT_OBJECT_0)A.error=E_FAIL;}
 A.initialized=now();A.opened=now();if(!A.error&&!A.render_error){if(A.mpv){int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,A.file,-1,NULL,0,NULL,NULL);char *path=(char*)malloc(n?n:1);if(n&&WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,A.file,-1,path,n,NULL,NULL)){const char *cmd[]={"loadfile",path,"replace",NULL};int e=p_mpv_command(A.mp,cmd);if(e<0)A.error=e;}else A.error=E_INVALIDARG;free(path);}else{BSTR source=SysAllocString(A.file);hr=A.mf->SetSource(source);SysFreeString(source);if(FAILED(hr))A.error=hr;}}
 bool ready=false,video=false,audio=false;LONGLONG limit=now()+frequency()*12;
 while(!A.error&&!A.render_error&&now()<limit){pump(5);if(A.loaded){video=has_video();audio=has_audio();if(!video||A.first){ready=true;A.ready=now();break;}}}
 if(!ready&&!A.error&&!A.render_error)A.error=HRESULT_FROM_WIN32(ERROR_TIMEOUT);Memory loaded=memory();double len=ready?duration():0,advance=0,playwall=0,drift=0;unsigned long long cpu=0;bool volume_ok=false,pause_ok=false,seek_ok=false,resume_ok=false;
 if(ready){A.native_width=get_dim(false);A.native_height=get_dim(true);volume(.4);volume_ok=fabs(volume_value()-.4)<.001;mute(false);volume_ok=volume_ok&&!muted();mute(true);volume(.25);volume_ok=volume_ok&&muted()&&fabs(volume_value()-.25)<.001;if(video)gpu_begin();
  double before=position();unsigned long long c0=cpu100();LONGLONG t0=now();pause(false);pump(A.observe_ms);playwall=ms(t0,now());cpu=cpu100()-c0;advance=position()-before;if(video)gpu_end();pause(true);pump(60);double paused=position();pump(180);drift=fabs(position()-paused);pause_ok=drift<.08;
  double target=len*.6;LONG frame_before=A.frames;A.seek_start=now();seek(target);limit=now()+frequency()*5;while(!A.error&&now()<limit){pump(5);if(A.seeked&&fabs(position()-target)<.2&&(!video||A.frames>frame_before)){seek_ok=true;A.seek_end=now();break;}}
  double p0=position();pause(false);pump(250);resume_ok=position()>p0+.1;pause(true);
  if(A.mpv){char *decoder=p_mpv_get_property_string(A.mp,"hwdec-current");if(decoder){strncpy(A.decoder,decoder,sizeof A.decoder-1);p_mpv_free(decoder);}}
 }
 if(A.manual&&ready){while(WaitForSingleObject(A.quit,0)!=WAIT_OBJECT_0&&!A.error){pump(10);}}
 LONGLONG close_start=now();SetEvent(A.quit);if(A.renderer){if(WaitForSingleObject(A.renderer,5000)!=WAIT_OBJECT_0){/* Owned prototype process is killed by the outer timeout if its backend hangs. */return 3;}CloseHandle(A.renderer);}
 if(A.mf){A.mf->Shutdown();drop(A.mfex);drop(A.mf);MFShutdown();}if(A.mp){p_mpv_terminate_destroy(A.mp);A.mp=NULL;}LONGLONG closed=now();
 save_report(ready,video,audio,len,initial,loaded,cpu,playwall,advance,volume_ok,pause_ok,seek_ok,resume_ok,drift,close_start,closed);
 drop(A.manager);drop(A.view);drop(A.staging);drop(A.target);drop(A.swap);drop(A.context);drop(A.device);DestroyWindow(A.window);if(A.dll)FreeLibrary(A.dll);CoUninitialize();CloseHandle(A.quit);CloseHandle(A.render_ready);return ready&&!A.error&&!A.render_error&&volume_ok&&pause_ok&&seek_ok&&resume_ok&&advance>.1?0:1;
}
