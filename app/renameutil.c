#include "../src/namecodec.h"
#include "renameutil.h"
#include "linkdialog.h"
#include <string.h>
#include <stdio.h>
static void decode(const wchar_t *in,int linux,wchar_t out[260])
{
    if(linux){if(!fpe_lx_literal(in,out,260,0))out[0]=0;}
    else{wcsncpy(out,in,259);out[259]=0;}
}
int rename_decimal(const wchar_t *text,uint32_t min,uint32_t max,uint32_t *out)
{
    if(!text||!*text)return 0;uint64_t n=0;
    for(;*text;text++){if(*text<L'0'||*text>L'9')return 0;n=n*10+*text-L'0';if(n>max)return 0;}
    if(n<min)return 0;*out=(uint32_t)n;return 1;
}
static int append(wchar_t out[260],size_t *at,const wchar_t *text,size_t n)
{
    if(*at+n>255)return 0;memcpy(out+*at,text,n*sizeof(wchar_t));*at+=n;out[*at]=0;return 1;
}
int rename_transform(const wchar_t *from,int directory,int linux,uint32_t index,const rename_rules *r,wchar_t out[260])
{
    wchar_t raw[260],built[260]=L"",number[40]=L"";decode(from,linux,raw);
    size_t size=wcslen(raw),body=size,find=wcslen(r->find),at=0;
    if(r->keep_extension&&!directory){const wchar_t *dot=wcsrchr(raw,L'.');if(dot&&dot>raw)body=(size_t)(dot-raw);}
    if(!append(built,&at,r->prefix,wcslen(r->prefix)))return 0;
    for(size_t i=0;i<body;){
        if(find&&find<=body-i&&!wmemcmp(raw+i,r->find,find)){
            if(!append(built,&at,r->replace,wcslen(r->replace)))return 0;i+=find;
        }else if(!append(built,&at,raw+i++,1))return 0;
    }
    if(!append(built,&at,r->suffix,wcslen(r->suffix)))return 0;
    if(r->numbering){
        if(r->digits<1||r->digits>9||r->start>999999999u||index>999999999u-r->start)return 0;
        swprintf(number,40,L"_%0*u",(int)r->digits,r->start+index);if(!append(built,&at,number,wcslen(number)))return 0;
    }
    if(!append(built,&at,raw+body,size-body)||!at||!wcscmp(built,L".")||!wcscmp(built,L".."))return 0;
    if(linux)return linkdialog_name(built,out);
    if(built[at-1]==L'.'||built[at-1]==L' '||wcspbrk(built,L"<>:\"/\\|?*"))return 0;
    for(size_t i=0;i<at;i++)if(built[i]<32)return 0;
    wcscpy(out,built);return 1; /* Engine also rejects reserved Windows names. */
}
void rename_display(const wchar_t *name,int linux,wchar_t *out,size_t cap)
{
    if(linux){fpe_lx_display(name,out,cap,0);return;}
    wchar_t raw[260];decode(name,linux,raw);size_t at=0;
    for(size_t i=0;raw[i]&&at+1<cap;i++){
        wchar_t c=raw[i];
        if(c>=0xd800&&c<=0xdbff&&raw[i+1]>=0xdc00&&raw[i+1]<=0xdfff){
            if(at+2>=cap)break;out[at++]=c;out[at++]=raw[++i];continue;
        }
        if(c<32||c==127||(c>=0xdc80&&c<=0xdcff)){
            wchar_t escaped[12];swprintf(escaped,12,L"\\u%04x",c);size_t n=wcslen(escaped);if(at+n>=cap)break;memcpy(out+at,escaped,n*sizeof *out);at+=n;
        }else out[at++]=c;
    }out[at]=0;
}
