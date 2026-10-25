#include "../src/namecodec.h"
/* Live search: shared query semantics, bounded traversal and borrowed hits. */
#include "internal.h"
#include <wctype.h>
#include <stdlib.h>

typedef struct term {wchar_t text[256];int kind,op;uint64_t number;} term;
typedef struct query {term terms[32];unsigned count;} query;
enum {Q_NAME,Q_EXT,Q_FILE_ONLY,Q_FOLDER_ONLY,Q_SIZE,Q_DATE};
static int number(const wchar_t *p,uint64_t *out)
{
    if(!iswdigit(*p))return 0;uint64_t n=0;
    while(iswdigit(*p)){unsigned d=*p++-L'0';if(n>(UINT64_MAX-d)/10)return 0;n=n*10+d;}
    uint64_t unit=1;
    if(*p&&!_wcsicmp(p,L"b"))unit=1;
    else if(*p){const wchar_t *u=L"kmgt";const wchar_t *at=wcschr(u,towlower(*p));if(!at)return 0;
        for(int i=0;i<=at-u;i++)unit*=1024;p++;if(!_wcsicmp(p,L"ib"))p+=2;else if(towlower(*p)==L'b')p++;if(*p)return 0;}
    if(n>UINT64_MAX/unit)return 0;*out=n*unit;return 1;
}
static int date(const wchar_t *p,uint64_t *out)
{
    if(wcslen(p)!=10||p[4]!=L'-'||p[7]!=L'-')return 0;
    for(int i=0;i<10;i++)if(i!=4&&i!=7&&!iswdigit(p[i]))return 0;
    SYSTEMTIME t={0};t.wYear=(p[0]-48)*1000+(p[1]-48)*100+(p[2]-48)*10+p[3]-48;
    t.wMonth=(p[5]-48)*10+p[6]-48;t.wDay=(p[8]-48)*10+p[9]-48;
    FILETIME ft;SYSTEMTIME back;if(!SystemTimeToFileTime(&t,&ft)||!FileTimeToSystemTime(&ft,&back)||back.wMonth!=t.wMonth||back.wDay!=t.wDay)return 0;
    *out=((uint64_t)ft.dwHighDateTime<<32)|ft.dwLowDateTime;return 1;
}
static fpe_err parse(const wchar_t *text,query *q)
{
    memset(q,0,sizeof *q);if(!text||!*text||wcslen(text)>255)return ERROR_INVALID_PARAMETER;
    const wchar_t *p=text;
    while(*p){while(iswspace(*p))p++;if(!*p)break;if(q->count==32)return ERROR_INVALID_PARAMETER;
        term *t=&q->terms[q->count++];unsigned n=0;int quote=0,colon_at=-1;
        while(*p&&(quote||!iswspace(*p))){if(*p==L'"'){quote=!quote;p++;continue;}if(n==255||*p<32)return ERROR_INVALID_PARAMETER;
            if(*p==L':'&&!quote&&colon_at<0)colon_at=(int)n;t->text[n++]=*p++;}
        if(quote||!n)return ERROR_INVALID_PARAMETER;t->text[n]=0;
        wchar_t *colon=colon_at>=0?t->text+colon_at:NULL;
        if(colon){*colon=0;const wchar_t *value=colon+1;
            if(!_wcsicmp(t->text,L"ext")){t->kind=Q_EXT;if(!*value)return ERROR_INVALID_PARAMETER;}
            else if(!_wcsicmp(t->text,L"file")&&!*value)t->kind=Q_FILE_ONLY;
            else if(!_wcsicmp(t->text,L"folder")&&!*value)t->kind=Q_FOLDER_ONLY;
            else if(!_wcsicmp(t->text,L"size")||!_wcsicmp(t->text,L"dm")){
                t->kind=!_wcsicmp(t->text,L"size")?Q_SIZE:Q_DATE;
                if(*value==L'>'||*value==L'<'){t->op=*value++==L'>'?1:-1;if(*value==L'='){t->op*=2;value++;}}
                else if(*value==L'=')value++;
                if(!(t->kind==Q_SIZE?number(value,&t->number):date(value,&t->number)))return ERROR_INVALID_PARAMETER;
            }else return ERROR_INVALID_PARAMETER;
            memmove(t->text,value,(wcslen(value)+1)*sizeof(wchar_t));
        }else if(wcspbrk(t->text,L"<>|!"))return ERROR_INVALID_PARAMETER;
    }
    return q->count?0:ERROR_INVALID_PARAMETER;
}
FPE_API fpe_err fpe_search_validate(const wchar_t *text){query q;return parse(text,&q);}
FPE_API fpe_err fpe_search_index_query(const wchar_t *text,wchar_t *out,size_t cap)
{
    query q;fpe_err err=parse(text,&q);if(err)return err;if(!out||!cap)return ERROR_INVALID_PARAMETER;out[0]=0;
    for(unsigned i=0;i<q.count;i++){
        term *t=&q.terms[i];if(t->kind==Q_SIZE||t->kind==Q_DATE)return ERROR_NOT_SUPPORTED;
        const wchar_t *prefix=t->kind==Q_FILE_ONLY?L"file:":t->kind==Q_FOLDER_ONLY?L"folder:":t->kind==Q_EXT?L"ext:":
            wcspbrk(t->text,L"*?")?L"nopath:diacritics:noregex:wfn:wildcards:":L"nopath:diacritics:noregex:nowfn:nowildcards:";
        size_t n=wcslen(out),p=wcslen(prefix),v=(t->kind==Q_FILE_ONLY||t->kind==Q_FOLDER_ONLY)?0:wcslen(t->text);
        if(n+p+v+5>=cap)return ERROR_BUFFER_OVERFLOW;
        wcscat(out,prefix);if(v){wcscat(out,L"\"");wcscat(out,t->text);wcscat(out,L"\"");}wcscat(out,L" ");
    }return 0;
}
static wchar_t ch(wchar_t c,int linux)
{
    if(linux&&c>=0xf001&&c<0xf080&&(c<0xf020||wcschr(L"\\:*?\"<>|",c-0xf000)))c-=0xf000;
    return c;
}
static int same(wchar_t a,wchar_t b,int linux)
{b=ch(b,linux);if(a<128&&b<128)return towlower(a)==towlower(b);return CompareStringOrdinal(&a,1,&b,1,TRUE)==CSTR_EQUAL;}
static int wildcard(const wchar_t *pattern,const wchar_t *name,int linux)
{
    const wchar_t *star=NULL,*retry=NULL;
    while(*name){if(*pattern==L'?'||(*pattern&&same(*pattern,*name,linux))){pattern++;name++;}
        else if(*pattern==L'*'){star=pattern++;retry=name;}
        else if(star){pattern=star+1;name=++retry;}else return 0;}
    while(*pattern==L'*')pattern++;return !*pattern;
}
static int match(const query *q,const wchar_t *name,const fpe_entry *e,int linux)
{
    wchar_t literal[256];
    if(linux){if(!fpe_lx_literal(name,literal,256,0))return 0;name=literal;linux=0;}
    int folder=fpe_entry_is_dir(e);
    for(unsigned i=0;i<q->count;i++){const term *t=&q->terms[i];
        if(t->kind==Q_FILE_ONLY){if(folder)return 0;}
        else if(t->kind==Q_FOLDER_ONLY){if(!folder)return 0;}
        else if(t->kind==Q_SIZE||t->kind==Q_DATE){
            if(t->kind==Q_SIZE&&folder)return 0;uint64_t n=t->kind==Q_SIZE?e->size:(uint64_t)e->mtime,v=t->number;
            if(t->kind==Q_DATE&&!t->op){if(n<v||n-v>=864000000000ULL)return 0;}
            else if(!(t->op==1?n>v:t->op==2?n>=v:t->op==-1?n<v:t->op==-2?n<=v:n==v))return 0;
        }else if(t->kind==Q_EXT){
            const wchar_t *ext=wcsrchr(name,L'.');int found=0;if(folder||!ext)return 0;ext++;
            for(const wchar_t *p=t->text;*p;){const wchar_t *end=wcschr(p,L';');size_t n=end?(size_t)(end-p):wcslen(p);
                if(n==wcslen(ext)){size_t k=0;while(k<n&&same(p[k],ext[k],linux))k++;if(k==n)found=1;}if(!end)break;p=end+1;}
            if(!found)return 0;
        }else if(wcspbrk(t->text,L"*?")){if(!wildcard(t->text,name,linux))return 0;}
        else{int found=0;for(const wchar_t *p=name;*p&&!found;p++){size_t k=0;while(t->text[k]&&p[k]&&same(t->text[k],p[k],linux))k++;found=!t->text[k];}if(!found)return 0;}
    }return 1;
}
typedef struct search {query q;fpe_search_stats r;fpe_search_cb cb;void *user;const wchar_t *root;int linux;} search;
static int pulse(void *user)
{
    search *s=user;if(s->r.cancelled||s->r.limited)return 0;
    if(s->cb&&!s->cb(s->user,NULL,NULL,&s->r))s->r.cancelled=1;return !s->r.cancelled;
}
static void failed(search *s,fpe_err err){s->r.errors++;if(!s->r.first_error)s->r.first_error=err;}
static wchar_t *join(const wchar_t *a,const wchar_t *b)
{
    size_t x=wcslen(a),y=wcslen(b),sep=x&&a[x-1]!=L'\\';if(x+sep+y>=32700)return NULL;
    wchar_t *p=fpe_alloc((x+sep+y+1)*sizeof *p);if(p){memcpy(p,a,x*sizeof *p);if(sep)p[x++]=L'\\';memcpy(p+x,b,(y+1)*sizeof *p);}return p;
}
static int hit(void *user,const wchar_t *relative,const fpe_entry *entry)
{
    search *s=user;if(!pulse(s))return 0;if(++s->r.scanned>100000){s->r.limited=1;return 0;}
    const wchar_t *name=wcsrchr(relative,L'\\');name=name?name+1:relative;
    if(match(&s->q,name,entry,s->linux)){
        wchar_t *path=join(s->root,relative);if(!path){failed(s,ERROR_FILENAME_EXCED_RANGE);return 1;}
        s->r.matched++;if(s->cb&&!s->cb(s->user,path,entry,&s->r))s->r.cancelled=1;fpe_free(path);
        if(s->r.matched==300)s->r.limited=1;
    }return !s->r.cancelled&&!s->r.limited;
}
typedef struct walk_level {search *s;fpe_path_guard *guard;const wchar_t *prefix;unsigned depth;} walk_level;
static void walk(search *s,fpe_path_guard *guard,const wchar_t *prefix,unsigned depth);
static int enum_hit(void *user,const FILE_FULL_DIR_INFO *fi)
{
    walk_level *level=user;search *s=level->s;size_t n=fi->FileNameLength/sizeof(wchar_t);
    if(n==1&&fi->FileName[0]==L'.')return 1;if(n==2&&fi->FileName[0]==L'.'&&fi->FileName[1]==L'.')return 1;
    if(!pulse(s))return 0;
    if(n>255||fi->FileNameLength%2){failed(s,ERROR_INVALID_DATA);return 0;}
    wchar_t name[256];memcpy(name,fi->FileName,n*sizeof(wchar_t));name[n]=0;
    wchar_t *relative=join(level->prefix,name);if(!relative){failed(s,ERROR_FILENAME_EXCED_RANGE);return 1;}
    fpe_entry entry={.size=(uint64_t)fi->EndOfFile.QuadPart,.mtime=fi->LastWriteTime.QuadPart,.attrs=fi->FileAttributes};
    int more=hit(s,relative,&entry);
    if(more&&fpe_entry_is_dir(&entry)){
        if(entry.attrs&FILE_ATTRIBUTE_REPARSE_POINT)s->r.skipped++;
        else if(level->depth>=127)s->r.limited=1;
        else{fpe_path_guard child={0};fpe_err err=fpe_guard_child(level->guard,name,0,0,pulse,s,&child);
            if(err==ERROR_NOT_SAME_DEVICE||err==ERROR_CANT_ACCESS_FILE)s->r.skipped++;
            else if(err==ERROR_CANCELLED)s->r.cancelled=1;
            else if(err)failed(s,err);else walk(s,&child,relative,level->depth+1);fpe_guard_close(&child);}
    }
    fpe_free(relative);return !s->r.cancelled&&!s->r.limited;
}
static void walk(search *s,fpe_path_guard *guard,const wchar_t *prefix,unsigned depth)
{
    if(!pulse(s))return;wchar_t *nt=NULL;fpe_err err=fpe_path_nt(guard->path,&nt);uint8_t *buffer=NULL;
    if(!err){buffer=fpe_alloc(65536);if(!buffer)err=ERROR_NOT_ENOUGH_MEMORY;}
    if(!err){walk_level level={s,guard,prefix,depth};err=fpe_enum(nt,buffer,65536,enum_hit,&level);}
    if(err)failed(s,err);fpe_free(nt);fpe_free(buffer);
}
FPE_API fpe_err fpe_search_folder(const wchar_t *folder,const wchar_t *text,fpe_search_cb cb,void *user,fpe_search_stats *out)
{
    if(!out)return ERROR_INVALID_PARAMETER;memset(out,0,sizeof *out);search s={.cb=cb,.user=user,.root=folder};
    fpe_err err=parse(text,&s.q);if(!folder||!*folder)err=ERROR_INVALID_PARAMETER;
    if(err){out->errors=1;return out->first_error=err;}
    s.linux=fpe_wsl_path_kind(folder);
    if(pulse(&s)){
        if(s.linux){uint32_t skipped=0,errors=0;err=fpe_wsl_search(folder,pulse,hit,&s,&skipped,&errors);s.r.skipped=skipped;
            if(err==ERROR_BUFFER_OVERFLOW)s.r.limited=1;
            if(err==ERROR_CANCELLED&&!s.r.limited)s.r.cancelled=1;else if(err&&err!=ERROR_CANCELLED)failed(&s,err);
            if(errors>s.r.errors)s.r.errors=errors;}
        else{fpe_path_guard root={0};err=fpe_guard_open(folder,0,0,0,0,pulse,&s,&root);
            if(err==ERROR_CANCELLED)s.r.cancelled=1;else if(err)failed(&s,err);else walk(&s,&root,L"",0);fpe_guard_close(&root);}
    }
    *out=s.r;return s.r.cancelled?ERROR_CANCELLED:s.r.first_error;
}

FPE_API fpe_err fpe_search_folder_indexed(const wchar_t *folder,const wchar_t *text,fpe_search_cb cb,void *user,fpe_search_stats *out,fpe_index_status *index)
{
    if(!out||!index)return ERROR_INVALID_PARAMETER;memset(out,0,sizeof *out);memset(index,0,sizeof *index);
    search s={.cb=cb,.user=user,.root=folder,.linux=1};fpe_err err=parse(text,&s.q);
    if(err||!folder||!*folder)return ERROR_INVALID_PARAMETER;
    if(!fpe_wsl_native(folder))return ERROR_NOT_SUPPORTED;
    /* Only an all-ASCII name term containing a literal trigram is used to
     * narrow candidates. Every non-ASCII basename is additionally included by
     * the helper, preserving Windows ordinal case equivalences and raw bytes.
     * The existing matcher still evaluates every term on current metadata. */
    const wchar_t *chosen=NULL;
    for(unsigned i=0;i<s.q.count;i++){
        term *t=&s.q.terms[i];if(t->kind==Q_SIZE||t->kind==Q_DATE)return ERROR_NOT_SUPPORTED;
        if(t->kind!=Q_NAME)continue;unsigned run=0,best=0;int ascii=1;
        for(const wchar_t *p=t->text;*p;p++){if(*p>=128)ascii=0;if(*p==L'*'||*p==L'?'){run=0;}else if(++run>best)best=run;}
        if(ascii&&best>=3&&!chosen)chosen=t->text;
    }
    if(!chosen)return ERROR_NOT_SUPPORTED;
    char glob[2048];size_t n=0;int wild=wcspbrk(chosen,L"*?")!=NULL;if(!wild)glob[n++]='*';
    for(const wchar_t *p=chosen;*p;p++){
        char c=(char)*p;if(c>='A'&&c<='Z')c+=32;
        if(c=='['){memcpy(glob+n,"[[]",3);n+=3;}else glob[n++]=c;
    }if(!wild)glob[n++]='*';glob[n]=0;
    uint32_t skipped=0,errors=0;
    err=fpe_wsl_index_search(folder,glob,pulse,hit,&s,&skipped,&errors,index);s.r.skipped=skipped;
    if(err==ERROR_RETRY||err==ERROR_NOT_SUPPORTED){*out=s.r;return err;}
    if(err==ERROR_BUFFER_OVERFLOW)s.r.limited=1;
    else if(err==ERROR_CANCELLED){if(!s.r.limited)s.r.cancelled=1;}
    else if(err)failed(&s,err);
    if(errors>s.r.errors&&!s.r.limited&&!s.r.cancelled)s.r.errors=errors;
    *out=s.r;return s.r.cancelled?ERROR_CANCELLED:s.r.first_error;
}
