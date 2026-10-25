#include "contentmatch.h"
#include "textutil.h"
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
static size_t units(const uint16_t *s,size_t cap){size_t n=0;while(n<cap&&s[n])n++;return n;}
int content_options_valid(const cf_options *o){if(!o||o->flags&~7u)return 0;size_t q=units(o->query,CF_QUERY),p=units(o->pattern,CF_QUERY);if(!q||q>=CF_QUERY||p>=CF_QUERY)return 0;for(size_t i=0;i<q;i++)if(o->query[i]<32||o->query[i]=='\r'||o->query[i]=='\n')return 0;return 1;}
static uint32_t fold(uint32_t c,int sensitive){return sensitive?c:(uint32_t)towlower((wint_t)c);}
static int glob(const wchar_t *name,const wchar_t *pattern){const wchar_t *star=NULL,*retry=NULL;while(*name){if(*pattern=='?'||(*pattern&&fold(*name,0)==fold(*pattern,0))){pattern++;name++;}else if(*pattern=='*'){star=pattern++;retry=name;}else if(star){pattern=star+1;name=++retry;}else return 0;}while(*pattern=='*')pattern++;return !*pattern;}
int content_name_matches(const wchar_t *name,const uint16_t pattern[CF_QUERY]){wchar_t part[CF_QUERY];size_t at=0;for(size_t i=0;i<CF_QUERY;i++){uint16_t c=pattern[i];if(!c||c==';'){part[at]=0;if(at&&glob(name,part))return 1;if(!c)return i==0;at=0;}else part[at++]=(wchar_t)c;}return 0;}
int content_match_bytes(const unsigned char *bytes,size_t size,const cf_options *o,uint32_t capacity,cf_hit **out,uint32_t *count,int *more){
 *out=NULL;*count=0;*more=0;if(!content_options_valid(o))return 0;text_data d;int decoded=text_decode(bytes,size,0,TEXT_AUTO,&d);if(decoded<0)return decoded==-1?-1:0;
 uint32_t q=0,prefix[CF_QUERY]={0},query[CF_QUERY];while(o->query[q]){query[q]=fold(o->query[q],o->flags&CF_CASE);q++;}
 for(uint32_t i=1,k=0;i<q;i++){while(k&&query[i]!=query[k])k=prefix[k-1];if(query[i]==query[k])k++;prefix[i]=k;}
 uint32_t line=1,start=0,k=0,allocated=0;
 for(uint32_t i=0;i<d.chars;i++){
  if(d.text[i]=='\n'){line++;start=i+1;k=0;continue;}
  uint32_t c=fold(d.text[i],o->flags&CF_CASE);while(k&&c!=query[k])k=prefix[k-1];if(c==query[k])k++;
  if(k==q){uint32_t at=i+1-q;int before=at>start&&(iswalnum(d.text[at-1])||d.text[at-1]=='_'),after=i+1<d.chars&&(iswalnum(d.text[i+1])||d.text[i+1]=='_');
   if(!(o->flags&CF_WORD)||(!before&&!after)){
    if(*count>=capacity){*more=1;break;}
    if(*count==allocated){uint32_t next=allocated?allocated*2:16;if(next>capacity)next=capacity;cf_hit *h=realloc(*out,(size_t)next*sizeof **out);if(!h){free(*out);*out=NULL;*count=0;text_data_free(&d);return -1;}*out=h;allocated=next;}
    cf_hit *h=&(*out)[(*count)++];memset(h,0,sizeof *h);h->line=line;h->column=at-start+1;h->length=q;h->encoding=d.encoding;h->flags=d.flags;
    for(uint32_t j=0;j<q;j++)h->matched[j]=(uint16_t)d.text[at+j];uint32_t excerpt=at-start>48?at-48:start;
    if(excerpt>start&&d.text[excerpt]>=0xdc00&&d.text[excerpt]<=0xdfff)excerpt--;
    h->excerpt_column=excerpt-start+1;h->highlight=at-excerpt;uint32_t j=0;while(j<CF_EXCERPT-1&&excerpt+j<d.chars&&d.text[excerpt+j]!='\n'){h->excerpt[j]=(uint16_t)d.text[excerpt+j];j++;}
    if(j&&h->excerpt[j-1]>=0xd800&&h->excerpt[j-1]<=0xdbff)h->excerpt[j-1]=0;
   }k=prefix[k-1];
  }
 }
 if(d.flags&TEXTP_PARTIAL)*more=1;text_data_free(&d);return 1;
}
