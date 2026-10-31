/* Cached-list matching only: no source reads, shell queries or helper startup. */
#pragma once
#include "typeicons.h"
#include <wchar.h>

enum { PF_IMAGES, PF_VIDEO, PF_AUDIO, PF_DOCUMENTS, PF_ARCHIVES, PF_TEXT,
       PF_DATA, PF_OTHER, PF_GROUPS };
static const wchar_t *const pf_labels[]={L"Images",L"Video",L"Audio",L"Documents",L"Archives",L"Text/code",L"Data",L"Other"};
static inline unsigned pf_group(ti_kind k){
 switch(k){case TI_IMAGE:return PF_IMAGES;case TI_VIDEO:return PF_VIDEO;case TI_AUDIO:return PF_AUDIO;
 case TI_DOC:case TI_PDF:case TI_SHEET:case TI_SLIDES:return PF_DOCUMENTS;
 case TI_ARCHIVE:return PF_ARCHIVES;case TI_TEXT:case TI_CODE:case TI_SCRIPT:return PF_TEXT;
 case TI_DATA:case TI_DATABASE:return PF_DATA;default:return PF_OTHER;}
}
static inline int pf_equal(wchar_t a,wchar_t b){
 if(a==b)return 1;if(a<128&&b<128){if(a>=L'A'&&a<=L'Z')a+=32;if(b>=L'A'&&b<=L'Z')b+=32;return a==b;}
 return CompareStringOrdinal(&a,1,&b,1,TRUE)==CSTR_EQUAL;
}
static inline int pf_glob(const wchar_t *name,const wchar_t *pattern,size_t n){
 size_t i=0;const wchar_t *star=NULL,*retry=NULL;size_t after=0;
 while(*name){if(i<n&&pattern[i]!=L'*'&&(pattern[i]==L'?'||pf_equal(*name,pattern[i]))){name++;i++;}
 else if(i<n&&pattern[i]==L'*'){star=pattern+i++;after=i;retry=name;}
 else if(star){i=after;name=++retry;}else return 0;}
 while(i<n&&pattern[i]==L'*')i++;return i==n;
}
static inline int pf_text_matches(const wchar_t *name,const wchar_t *query){
 int any=0;size_t length=wcslen(name);
 while(*query){while(*query==L' '||*query==L'\t'||*query==L';')query++;const wchar_t *end=wcschr(query,L';');if(!end)end=query+wcslen(query);
 while(end>query&&(end[-1]==L' '||end[-1]==L'\t'))end--;size_t n=(size_t)(end-query);
 if(n){any=1;int glob=0;for(size_t i=0;i<n;i++)if(query[i]==L'*'||query[i]==L'?')glob=1;
 if(glob){if(pf_glob(name,query,n))return 1;}
 else if(query[0]==L'.'){if(length>=n&&CompareStringOrdinal(name+length-n,(int)n,query,(int)n,TRUE)==CSTR_EQUAL)return 1;}
 else for(size_t i=0;i+n<=length;i++)if(pf_equal(name[i],query[0])&&CompareStringOrdinal(name+i,(int)n,query,(int)n,TRUE)==CSTR_EQUAL)return 1;
 }
 query=wcschr(query,L';');if(!query)break;query++;
 }
 return !any;
}
