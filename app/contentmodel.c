#include "content.h"
#include <stdlib.h>
#include <string.h>
void content_model_free(cf_model *m){for(uint32_t i=0;i<m->count;i++)free(m->rows[i].path);free(m->rows);memset(m,0,sizeof *m);}
int content_model_add(cf_model *m,const cf_record *r,const wchar_t *relative){
 if(r->magic!=CF_MAGIC)return 0;
 if(r->type==CF_END||r->type==CF_PROGRESS){if(r->summary.state>CF_STOPPED)return 0;m->summary=r->summary;if(m->limited)m->summary.state=CF_PARTIAL;return 1;}
 if(r->type!=CF_MATCH||!content_relative_safe(relative)||!r->hit.line||!r->hit.column||!r->hit.length||r->hit.length>=CF_QUERY||r->hit.matched[r->hit.length]||r->hit.excerpt[CF_EXCERPT-1]||r->hit.highlight>=CF_EXCERPT||r->hit.highlight+r->hit.length>=CF_EXCERPT)return 0;
 size_t bytes=(wcslen(relative)+1)*2;if(m->count>=CF_RESULTS||m->names+bytes>CF_NAMES_BYTES){m->limited=1;m->summary.state=CF_PARTIAL;return 0;}
 if(m->count==m->capacity){uint32_t cap=m->capacity?m->capacity*2:64;if(cap>CF_RESULTS)cap=CF_RESULTS;cf_result *rows=realloc(m->rows,(size_t)cap*sizeof *rows);if(!rows){m->limited=1;return 0;}m->rows=rows;m->capacity=cap;}
 wchar_t *path=_wcsdup(relative);if(!path){m->limited=1;return 0;}m->rows[m->count++]=(cf_result){path,r->bytes,r->mtime,r->hit};m->names+=bytes;return 1;
}
wchar_t *content_result_path(const wchar_t *root,const wchar_t *relative){if(!root||!*root||!content_relative_safe(relative))return NULL;size_t n=wcslen(root),r=wcslen(relative);if(n+r+2>32768)return NULL;wchar_t *p=malloc((n+r+2)*2);if(p){wcscpy(p,root);if(root[n-1]!='\\')wcscat(p,L"\\");wcscat(p,relative);}return p;}
