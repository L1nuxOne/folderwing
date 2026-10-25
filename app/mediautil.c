#include "mediautil.h"
#include <wchar.h>
#include <wctype.h>
#include <math.h>
static int extension(const wchar_t *name,const wchar_t *const *extensions){const wchar_t *ext=name?wcsrchr(name,L'.'):NULL;if(!ext)return 0;for(int i=0;extensions[i];i++)if(!_wcsicmp(ext+1,extensions[i]))return 1;return 0;}
int media_audio_extension(const wchar_t *name){static const wchar_t *const exts[]={L"wav",L"mp3",L"m4a",L"aac",L"flac",L"ogg",L"oga",L"opus",L"wma",L"aiff",L"aif",L"alac",NULL};return extension(name,exts);}
int media_extension(const wchar_t *name){static const wchar_t *const exts[]={L"mp4",L"mkv",L"webm",L"mov",L"avi",L"m4v",L"wmv",L"mpg",L"mpeg",L"m2v",L"mts",L"m2ts",L"3gp",L"ogv",NULL};return media_audio_extension(name)||extension(name,exts);}
int media_fallback_error(uint32_t error){return error==0xc00d5212u||error==0xc00d36c4u||error==0xc00d36b4u;}
int media_path_valid(const wchar_t *p){if(!p||!p[0])return 0;size_t n=wcslen(p);if(n>=MEDIAP_PATH_MAX)return 0;if(n>7&&!wcsncmp(p,L"\\\\?\\",4)&&iswalpha(p[4])&&p[5]==L':'&&p[6]==L'\\'){p+=4;n-=4;}int drive=iswalpha(p[0])&&p[1]==L':'&&p[2]==L'\\';int unc=n>4&&p[0]==L'\\'&&p[1]==L'\\'&&p[2]!=L'?'&&p[2]!=L'.'&&p[2]!=L'\\';if(!drive&&!unc)return 0;for(size_t i=drive?2:0;i<n;i++)if(p[i]<32||p[i]==L':'||p[i]==L'/'||p[i]==L'"'||p[i]==L'*'||p[i]==L'?')return 0;return 1;}
int media_open_valid(const mediap_head *h,const void *payload){if(!h||!payload||h->type!=MEDIAP_OPEN||!h->doc||h->size<sizeof(mediap_open)+4||h->size>sizeof(mediap_open)+MEDIAP_PATH_MAX*2u||(h->size-sizeof(mediap_open))%2)return 0;const mediap_open *o=payload;const wchar_t *p=(const wchar_t *)(o+1);size_t chars=(h->size-sizeof *o)/2;if(o->reserved||!o->width||o->width>MEDIAP_MAX_W||!o->height||o->height>MEDIAP_MAX_H||(o->flags&~7u)||p[chars-1])return 0;for(size_t i=0;i+1<chars;i++)if(!p[i])return 0;return media_path_valid(p);}
int media_frame_valid(const mediap_frame *f){return f&&f->bank<2&&f->width&&f->width<=MEDIAP_MAX_W&&f->height&&f->height<=MEDIAP_MAX_H&&!f->reserved;}
