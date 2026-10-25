/* Lossless Linux byte names carried in Windows UTF-16 paths.
 * Keep WSL's legacy ASCII aliases. Escape genuine alias characters and the
 * escape itself with U+F080, so path identity survives UTF-8 persistence too.
 * Invalid UTF-8 bytes retain the existing U+DC80..U+DCFF representation.
 * Escaped names require native Linux I/O, never the Windows WSL provider. */
#ifndef FPE_NAMECODEC_H
#define FPE_NAMECODEC_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <wchar.h>
#define FPE_LX_ESCAPE 0xf080u
static inline int fpe_lx_mapped(uint32_t c)
{return (c>=1&&c<32)||(c>=32&&c<128&&strchr("\\:*?\"<>|",(int)c));}
static inline int fpe_lx_quoted(uint32_t c)
{return c==FPE_LX_ESCAPE||(c>=0xf000&&c<0xf080&&fpe_lx_mapped(c-0xf000));}
/* Output never needs more UTF-16 units than input bytes; cap includes NUL. */
static inline int fpe_lx_from_bytes(const uint8_t *s,size_t n,wchar_t *out,size_t cap,int map,int path,size_t *length)
{
    size_t k=0;
    for(size_t i=0;i<n;){
        uint32_t b=s[i],u=b;size_t take=1;
        if(!b)return 0;
        if(b>=128){
            unsigned extra=b>=0xc2&&b<=0xdf?1:b>=0xe0&&b<=0xef?2:b>=0xf0&&b<=0xf4?3:0;
            u=b&(0x7fu>>(extra+1));int valid=extra&&extra<n-i;
            for(unsigned j=1;valid&&j<=extra;j++){if((s[i+j]&0xc0)!=0x80)valid=0;else u=(u<<6)|(s[i+j]&63);}
            if(valid&&(u<(extra==1?0x80u:extra==2?0x800u:0x10000u)||u>0x10ffff||(u>=0xd800&&u<=0xdfff)))valid=0;
            if(valid)take=extra+1;else u=0xdc00+b;
        }
        size_t units=u>=0x10000?2:map&&fpe_lx_quoted(u)?2:1;
        if(k>=cap||units>=cap-k)return 0;
        if(u>=0x10000){u-=0x10000;out[k++]=(wchar_t)(0xd800+(u>>10));out[k++]=(wchar_t)(0xdc00+(u&1023));}
        else if(map&&fpe_lx_quoted(u)){out[k++]=FPE_LX_ESCAPE;out[k++]=(wchar_t)u;}
        else{if(path&&u=='/')u='\\';else if(map&&fpe_lx_mapped(u))u+=0xf000;out[k++]=(wchar_t)u;}
        i+=take;
    }
    if(k>=cap)return 0;out[k]=0;if(length)*length=k;return 1;
}
/* Decode one scalar. Distinguish separators from mapped literal backslashes. */
static inline int fpe_lx_next(const wchar_t **text,uint32_t *value,int path)
{
    const wchar_t *p=*text;uint32_t c=(uint16_t)*p++;
    if(c==FPE_LX_ESCAPE){c=(uint16_t)*p++;if(!fpe_lx_quoted(c))return 0;}
    else if(path&&c=='\\')c='/';
    else if(c>=0xf000&&c<0xf080&&fpe_lx_mapped(c-0xf000))c-=0xf000;
    else if(c>=0xd800&&c<=0xdbff){uint32_t low=(uint16_t)*p++;if(low<0xdc00||low>0xdfff)return 0;c=0x10000+((c-0xd800)<<10)+low-0xdc00;}
    else if(c>=0xdc00&&c<=0xdfff&&!(c>=0xdc80&&c<=0xdcff))return 0;
    *text=p;*value=c;return 1;
}
static inline int fpe_lx_to_bytes(const wchar_t *text,uint8_t *out,size_t cap,int path,size_t *length)
{
    size_t k=0;
    while(*text){
        uint32_t c;uint8_t b[4];size_t n;
        if(!fpe_lx_next(&text,&c,path))return 0;
        if(c>=0xdc80&&c<=0xdcff){b[0]=(uint8_t)(c-0xdc00);n=1;}
        else if(c<128){b[0]=(uint8_t)c;n=1;}
        else if(c<2048){b[0]=0xc0|(c>>6);b[1]=0x80|(c&63);n=2;}
        else if(c<65536){b[0]=0xe0|(c>>12);b[1]=0x80|((c>>6)&63);b[2]=0x80|(c&63);n=3;}
        else{b[0]=0xf0|(c>>18);b[1]=0x80|((c>>12)&63);b[2]=0x80|((c>>6)&63);b[3]=0x80|(c&63);n=4;}
        if(k>cap||n>cap-k)return 0;memcpy(out+k,b,n);k+=n;
    }
    if(length)*length=k;return 1;
}
/* Literal edit/search text; contraction only, also safe in place. */
static inline int fpe_lx_literal(const wchar_t *text,wchar_t *out,size_t cap,int path)
{
    size_t k=0;
    while(*text){uint32_t c;if(!fpe_lx_next(&text,&c,path))return 0;size_t n=c>=0x10000?2:1;
        if(k>=cap||n>=cap-k)return 0;
        if(n==2){c-=0x10000;out[k++]=(wchar_t)(0xd800+(c>>10));out[k++]=(wchar_t)(0xdc00+(c&1023));}else out[k++]=(wchar_t)c;
    }
    if(k>=cap)return 0;out[k]=0;return 1;
}
/* Display only: quoted private-use characters stay visible as \uXXXX.
 * mode=1 uses control pictures; mode=0 uses the existing hex diagnostics. */
static inline int fpe_lx_display(const wchar_t *text,wchar_t *out,size_t cap,int mode)
{
    static const wchar_t hex[]=L"0123456789abcdef";size_t k=0;
    while(*text){const wchar_t *token=text;uint32_t c;if(!fpe_lx_next(&text,&c,0))break;
        int escape=fpe_lx_quoted(c)||(mode&&c>=0x2401&&c<=0x241f)||(c>=0xdc80&&c<=0xdcff)||(!mode&&(c<32||c==127));
        if(c=='\\'&&*token==0xf05c){if(k>=cap||2>=cap-k)break;out[k++]=L'\\';out[k++]=L'\\';}
        else if(escape){if(k>=cap||6>=cap-k)break;out[k++]=L'\\';out[k++]=L'u';for(int shift=12;shift>=0;shift-=4)out[k++]=hex[(c>>shift)&15];}
        else if(c>=0x10000){if(k>=cap||2>=cap-k)break;c-=0x10000;out[k++]=(wchar_t)(0xd800+(c>>10));out[k++]=(wchar_t)(0xdc00+(c&1023));}
        else{if(k+1>=cap)break;out[k++]=(wchar_t)(mode&&c<32?0x2400+c:c);}
    }
    if(cap)out[k]=0;return !*text;
}
#endif
