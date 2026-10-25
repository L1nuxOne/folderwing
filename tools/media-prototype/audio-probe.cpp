#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>
static void describe(IMMDevice *d){LPWSTR id=NULL;DWORD state=0;IPropertyStore *s=NULL;PROPVARIANT v;PropVariantInit(&v);d->GetId(&id);d->GetState(&state);if(SUCCEEDED(d->OpenPropertyStore(STGM_READ,&s))){s->GetValue(PKEY_Device_FriendlyName,&v);s->Release();}wprintf(L"state=%lu name=%ls id=%ls\n",state,v.vt==VT_LPWSTR?v.pwszVal:L"?",id?id:L"?");PropVariantClear(&v);CoTaskMemFree(id);}
int main(){DWORD session=0;ProcessIdToSessionId(GetCurrentProcessId(),&session);printf("pid=%lu session=%lu active_console=%lu\n",GetCurrentProcessId(),session,WTSGetActiveConsoleSessionId());HRESULT hr=CoInitializeEx(NULL,COINIT_MULTITHREADED);printf("COM=%08lx\n",hr);IMMDeviceEnumerator *e=NULL;hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),NULL,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),(void**)&e);printf("enumerator=%08lx\n",hr);if(!e)return 1;bool multimedia=false;for(int role=0;role<3;role++){IMMDevice *d=NULL;hr=e->GetDefaultAudioEndpoint(eRender,(ERole)role,&d);if(role==1)multimedia=SUCCEEDED(hr);printf("default_role=%d hr=%08lx ",role,hr);if(d){describe(d);d->Release();}else printf("\n");}IMMDeviceCollection *c=NULL;hr=e->EnumAudioEndpoints(eRender,DEVICE_STATEMASK_ALL,&c);printf("enumerate_all=%08lx\n",hr);if(c){UINT n=0;c->GetCount(&n);printf("count=%u\n",n);for(UINT i=0;i<n;i++){IMMDevice *d=NULL;if(SUCCEEDED(c->Item(i,&d))){describe(d);d->Release();}}c->Release();}e->Release();CoUninitialize();return multimedia?0:2;}
