/* Bounded, disposable media worker. The explorer owns its restricted token/job.
 * stdin/stdout carry fixed messages; acknowledged shared banks carry pixels. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <wchar.h>
#include "mediaengine.h"
#include "mediautil.h"
struct message {message *next;mediap_head h;unsigned char payload[1];};
static struct {HANDLE in,out,wake;CRITICAL_SECTION queue,output;message *first,*last;unsigned count;volatile LONG eof,waiting;unsigned char *pixels;uint32_t doc,sequence,bank;uint64_t last_frame;} Q;
static int io(HANDLE h,void *p,DWORD n,bool write){while(n){DWORD got=0;BOOL ok=write?WriteFile(h,p,n,&got,NULL):ReadFile(h,p,n,&got,NULL);if(!ok||!got)return 0;p=(char*)p+got;n-=got;}return 1;}
static void reply(uint32_t type,uint32_t ticket,const void *payload,uint32_t bytes){mediap_head h={type,bytes,Q.doc,ticket};EnterCriticalSection(&Q.output);if(!io(Q.out,&h,sizeof h,true)||(bytes&&!io(Q.out,(void*)payload,bytes,true)))InterlockedExchange(&Q.eof,1);LeaveCriticalSection(&Q.output);}
static void frame(const void *pixels,uint32_t w,uint32_t h,uint32_t stride,void *){uint64_t now=GetTickCount64();if(now-Q.last_frame<30||InterlockedCompareExchange(&Q.waiting,1,0))return;Q.last_frame=now;uint32_t bank=Q.bank;for(uint32_t y=0;y<h;y++)memcpy(Q.pixels+bank*MEDIAP_BANK_BYTES+(size_t)y*w*4,(const char*)pixels+(size_t)y*stride,w*4);MemoryBarrier();mediap_frame f={bank,w,h,0};reply(MEDIAP_FRAME,++Q.sequence,&f,sizeof f);}
static bool shape(const mediap_head *h){if(h->size>MEDIAP_MAX_PAYLOAD||!h->doc)return false;switch(h->type){case MEDIAP_OPEN:return h->size>=sizeof(mediap_open)+4;case MEDIAP_CONTROL:return h->size==sizeof(mediap_control);case MEDIAP_ACK:case MEDIAP_CLOSE:return !h->size;default:return false;}}
static DWORD WINAPI reader(void *){while(!Q.eof){mediap_head h;if(!io(Q.in,&h,sizeof h,false)||!shape(&h))break;message *m=(message*)malloc(sizeof(message)+h.size);if(!m)break;m->h=h;m->next=NULL;if(h.size&&!io(Q.in,m->payload,h.size,false)){free(m);break;}EnterCriticalSection(&Q.queue);if(Q.count>=64){LeaveCriticalSection(&Q.queue);free(m);break;}if(Q.last)Q.last->next=m;else Q.first=m;Q.last=m;Q.count++;LeaveCriticalSection(&Q.queue);SetEvent(Q.wake);}InterlockedExchange(&Q.eof,1);SetEvent(Q.wake);return 0;}
static message *take(){EnterCriticalSection(&Q.queue);message *m=Q.first;if(m){Q.first=m->next;if(!Q.first)Q.last=NULL;Q.count--;}LeaveCriticalSection(&Q.queue);return m;}
static DWORD regular(const wchar_t *path){HANDLE f=CreateFileW(path,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,NULL);if(f==INVALID_HANDLE_VALUE)return GetLastError();BY_HANDLE_FILE_INFORMATION info={};DWORD error=0;if(!GetFileInformationByHandle(f,&info))error=GetLastError();else if(GetFileType(f)!=FILE_TYPE_DISK||(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DEVICE|FILE_ATTRIBUTE_OFFLINE)))error=ERROR_NOT_SUPPORTED;CloseHandle(f);return error;}
static bool audio_available(){IMMDeviceEnumerator *e=NULL;IMMDevice *d=NULL;HRESULT hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),NULL,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),(void**)&e);if(SUCCEEDED(hr))hr=e->GetDefaultAudioEndpoint(eRender,eMultimedia,&d);if(d)d->Release();if(e)e->Release();return SUCCEEDED(hr);}
int wmain(int argc,wchar_t **argv){if(argc!=2)return 2;HANDLE section=(HANDLE)(uintptr_t)_wcstoui64(argv[1],NULL,10);Q.in=GetStdHandle(STD_INPUT_HANDLE);Q.out=GetStdHandle(STD_OUTPUT_HANDLE);Q.pixels=(unsigned char*)MapViewOfFile(section,FILE_MAP_WRITE,0,0,MEDIAP_SECTION_BYTES);CloseHandle(section);if(!Q.pixels)return 2;InitializeCriticalSection(&Q.queue);InitializeCriticalSection(&Q.output);Q.wake=CreateEventW(NULL,FALSE,FALSE,NULL);if(!Q.wake)return 2;HRESULT com=CoInitializeEx(NULL,COINIT_MULTITHREADED);if(FAILED(com))return 2;HANDLE thread=CreateThread(NULL,0,reader,NULL,0,NULL);if(!thread)return 2;CloseHandle(thread);
 wchar_t *path=NULL;mediap_open open={};mediap_state state={};uint32_t fallback=0,request_ticket=0,applied_ticket=0;bool opened=false,playing=false,muted=true,configured=false;double volume=.25,pending_seek=-1;uint64_t started=0,last_state=0,last_audio_check=0;bool output_available=true;double last_position=0;int exit_code=0;
 while(!Q.eof){message *m;while((m=take())!=NULL){if(m->h.type==MEDIAP_OPEN){if(opened||!media_open_valid(&m->h,m->payload)){free(m);exit_code=2;goto done;}open=*(mediap_open*)m->payload;path=_wcsdup((wchar_t*)(m->payload+sizeof open));if(!path){free(m);exit_code=2;goto done;}Q.doc=m->h.doc;opened=true;muted=(open.flags&(MEDIAP_MUTED|MEDIAP_THUMB))!=0;started=GetTickCount64();DWORD error=regular(path);if(error){state.state=MEDIAP_FAILED;state.error=error;state.backend=1;}else{state.error=(uint32_t)media_engine_open(path,0,open.width,open.height,open.flags,frame,NULL);state.state=state.error?MEDIAP_FAILED:MEDIAP_LOADING;state.backend=1;}}
 else if(m->h.doc==Q.doc){if(m->h.type==MEDIAP_CLOSE){free(m);goto done;}if(m->h.type==MEDIAP_ACK){if(m->h.ticket==Q.sequence){Q.bank^=1;InterlockedExchange(&Q.waiting,0);}}
 else if(m->h.type==MEDIAP_CONTROL){mediap_control c=*(mediap_control*)m->payload;if(c.reserved||c.action<MEDIA_PLAY||c.action>MEDIA_MUTE||!isfinite(c.value)){free(m);exit_code=2;goto done;}request_ticket=m->h.ticket;switch(c.action){case MEDIA_PLAY:playing=true;break;case MEDIA_PAUSE:playing=false;break;case MEDIA_SEEK:pending_seek=fmax(0,c.value);break;case MEDIA_VOLUME:volume=fmax(0,fmin(1,c.value));break;case MEDIA_MUTE:muted=c.value!=0;break;}configured=false;last_state=0;}}
 free(m);}
 if(opened){if(state.state!=MEDIAP_FAILED){if(state.state==MEDIAP_READY)last_position=state.position;media_engine_poll(&state);}
  if(state.state==MEDIAP_FAILED&&state.backend==1&&media_fallback_error(state.error)){fallback=state.error;if(last_position>0&&pending_seek<0)pending_seek=last_position;media_engine_close();configured=false;state={};state.backend=2;state.error=(uint32_t)media_engine_open(path,1,open.width,open.height,open.flags,frame,NULL);state.state=state.error?MEDIAP_FAILED:MEDIAP_LOADING;started=GetTickCount64();}
  if(state.state==MEDIAP_LOADING&&GetTickCount64()-started>12000){state.state=MEDIAP_FAILED;state.error=HRESULT_FROM_WIN32(ERROR_TIMEOUT);}
  if(state.state==MEDIAP_READY&&!configured){media_engine_control(MEDIA_VOLUME,volume);media_engine_control(MEDIA_MUTE,muted);if(pending_seek>=0){double target=state.duration>0?fmin(pending_seek,fmax(0,state.duration-.01)):pending_seek;media_engine_control(MEDIA_SEEK,target);pending_seek=-1;}media_engine_control(playing?MEDIA_PLAY:MEDIA_PAUSE,0);configured=true;applied_ticket=request_ticket;media_engine_poll(&state);}
  if(GetTickCount64()-last_audio_check>1000){output_available=audio_available();last_audio_check=GetTickCount64();}state.audio_unavailable=state.has_audio&&!output_available;
  state.fallback_error=fallback;if(GetTickCount64()-last_state>=100){reply(MEDIAP_STATE,applied_ticket,&state,sizeof state);last_state=GetTickCount64();}
 }
 WaitForSingleObject(Q.wake,10);
 }
 done:media_engine_close();free(path);/* A blocked stdin reader belongs to this disposable process. */
 CoUninitialize();return exit_code;
}
