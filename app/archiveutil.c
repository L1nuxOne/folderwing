#include "archiveutil.h"
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
static wchar_t *copy_n(const wchar_t *s,size_t n){wchar_t *p=malloc((n+1)*sizeof *p);if(p){memcpy(p,s,n*sizeof *p);p[n]=0;}return p;}
int arc_name_safe(const wchar_t *s){
 if(!s||!*s||wcslen(s)>=ARCP_NAME||*s==L'/'||*s==L'\\')return 0;
 const wchar_t *part=s;
 for(const wchar_t *p=s;;p++){
  if(*p<L' '&&*p)return 0;
  if(*p==L'\\'||(*p==L':'&&p==s+1&&p[1]==L'/'))return 0;
  if(!*p||*p==L'/'){
   size_t n=(size_t)(p-part);if((!n&&*p)||(n==1&&part[0]==L'.')||(n==2&&part[0]==L'.'&&part[1]==L'.'))return 0;
   if(!*p)return 1;part=p+1;
  }
 }
}
int arc_leaf_safe(const wchar_t *s){
 if(!arc_name_safe(s)||wcschr(s,L'/')||wcspbrk(s,L"<>:\"\\|?*")||wcslen(s)>240)return 0;
 size_t n=wcslen(s);if(s[n-1]==L'.'||s[n-1]==L' ')return 0;
 wchar_t base[16];size_t k=wcscspn(s,L".");if(k>=16)return 1;for(size_t i=0;i<k;i++)base[i]=towupper(s[i]);base[k]=0;
 if(!wcscmp(base,L"CON")||!wcscmp(base,L"PRN")||!wcscmp(base,L"AUX")||!wcscmp(base,L"NUL"))return 0;
 if(k==4&&(!wcsncmp(base,L"COM",3)||!wcsncmp(base,L"LPT",3))&&base[3]>=L'1'&&base[3]<=L'9')return 0;
 return 1;
}
int arc_extension(const wchar_t *s){
 const wchar_t *e=s?wcsrchr(s,L'.'):NULL;if(!e)return 0;
 const wchar_t *ext[]={L".zip",L".zipx",L".7z",L".rar",L".tar",L".tgz",L".tbz",L".tbz2",L".txz",L".gz",L".bz2",L".xz",L".zst",L".cab",L".iso",L".cpio",L".jar",L".whl",L".nupkg",L".apk",L".cbz"};
 for(size_t i=0;i<sizeof ext/sizeof *ext;i++)if(!_wcsicmp(e,ext[i]))return 1;return 0;
}
void arc_entries_free(arc_entry *a,uint32_t n){if(a){for(uint32_t i=0;i<n;i++)free(a[i].name);free(a);}}
void arc_rows_free(arc_row *a,uint32_t n){if(a){for(uint32_t i=0;i<n;i++){free(a[i].name);free(a[i].path);}free(a);}}
static int row_cmp(const void *a_,const void *b_){const arc_row *a=a_,*b=b_;int ad=!!(a->flags&ARCP_DIR),bd=!!(b->flags&ARCP_DIR);if(ad!=bd)return bd-ad;int c=_wcsicmp(a->name,b->name);if(!c)c=wcscmp(a->name,b->name);if(!c)c=(a->index>b->index)-(a->index<b->index);return c;}
static int path_cmp(const void *a_,const void *b_){const arc_row *a=a_,*b=b_;int c=wcscmp(a->path,b->path);if(!c)c=(!!(b->flags&ARCP_DIR))-(!!(a->flags&ARCP_DIR));return c;}
int arc_rows_build(const arc_entry *a,uint32_t count,const wchar_t *prefix,arc_row **out,uint32_t *n){
 *out=NULL;*n=0;size_t plen=wcslen(prefix);arc_row *r=calloc(count?count:1,sizeof *r);if(!r)return 0;uint32_t used=0;
 for(uint32_t i=0;i<count;i++){
  if(a[i].info.flags&ARCP_UNSAFE){if(plen)continue;}else if(wcsncmp(a[i].name,prefix,plen))continue;
  const wchar_t *start=a[i].name+((a[i].info.flags&ARCP_UNSAFE)?0:plen);if(!*start)continue;
  const wchar_t *slash=(a[i].info.flags&ARCP_UNSAFE)?NULL:wcschr(start,L'/');size_t len=slash?(size_t)(slash-start):wcslen(start);
  arc_row *p=&r[used++];p->index=a[i].info.index;p->flags=a[i].info.flags;p->size=a[i].info.size;
  p->name=copy_n(start,len);p->path=copy_n(a[i].name,slash?(size_t)(slash-a[i].name)+1:wcslen(a[i].name));
  if(slash)p->flags=ARCP_DIR;
  if(!p->name||!p->path){arc_rows_free(r,used);return 0;}
 }
 qsort(r,used,sizeof *r,path_cmp);uint32_t at=0;
 for(uint32_t i=0;i<used;i++){
  if(at&&(r[i].flags&ARCP_DIR)&&(r[at-1].flags&ARCP_DIR)&&!wcscmp(r[i].path,r[at-1].path)){free(r[i].name);free(r[i].path);continue;}
  if(at!=i)r[at]=r[i];at++;
 }
 qsort(r,at,sizeof *r,row_cmp);*out=r;*n=at;return 1;
}
