#include "diff.h"
#include <stdlib.h>
#include <string.h>
#define DIFF_EDITS 1024
#define DIFF_WORK 8000000u
static int stopped(HANDLE h){return h&&WaitForSingleObject(h,0)==WAIT_OBJECT_0;}
void diff_free(diff_result *r){for(int i=0;i<2;i++){text_data_free(&r->doc[i].data);free(r->doc[i].lines);}free(r->rows);memset(r,0,sizeof *r);}
static DWORD lines(diff_doc *d){
 uint32_t n=d->data.chars,at=0;for(uint32_t j=0;j<n;j++)if(d->data.text[j]=='\n')d->count++;
 if(n&&d->data.text[n-1]!='\n')d->count++;if(d->count>DIFF_LINES)return ERROR_BUFFER_OVERFLOW;
 d->lines=calloc(d->count?d->count:1,sizeof *d->lines);if(!d->lines)return ERROR_NOT_ENOUGH_MEMORY;
 for(uint32_t i=0;i<d->count;i++){uint32_t start=at;uint64_t hash=14695981039346656037ull;while(at<n&&d->data.text[at]!='\n'){hash^=(uint16_t)d->data.text[at++];hash*=1099511628211ull;}int nl=at<n;d->lines[i]=(diff_line){start,at-start,(uint32_t)nl,hash^(uint64_t)nl};if(nl)at++;}return 0;
}
static int equal(const diff_result *r,uint32_t a,uint32_t b){const diff_line *x=&r->doc[0].lines[a],*y=&r->doc[1].lines[b];return x->hash==y->hash&&x->length==y->length&&x->newline==y->newline&&!wmemcmp(r->doc[0].data.text+x->start,r->doc[1].data.text+y->start,x->length);}
static void row(diff_result *r,uint32_t a,uint32_t b,uint32_t kind){r->rows[r->count++]=(diff_row){a,b,kind,0};}
static void changed(diff_result *r,uint32_t a,uint32_t an,uint32_t b,uint32_t bn){uint32_t n=an>bn?an:bn;for(uint32_t i=0;i<n;i++)row(r,i<an?a+i:DIFF_NO_LINE,i<bn?b+i:DIFF_NO_LINE,i<an&&i<bn?DIFF_CHANGED:i<an?DIFF_REMOVED:DIFF_ADDED);}
static DWORD align(diff_result *r,HANDLE cancel){
 uint32_t n=r->doc[0].count,m=r->doc[1].count,prefix=0,suffix=0,work=0;while(prefix<n&&prefix<m&&equal(r,prefix,prefix)){if((++work&1023)==0&&stopped(cancel))return ERROR_CANCELLED;prefix++;}
 while(suffix<n-prefix&&suffix<m-prefix&&equal(r,n-suffix-1,m-suffix-1)){if((++work&1023)==0&&stopped(cancel))return ERROR_CANCELLED;suffix++;}
 r->rows=calloc((size_t)n+m+1,sizeof *r->rows);if(!r->rows)return ERROR_NOT_ENOUGH_MEMORY;
 for(uint32_t i=0;i<prefix;i++)row(r,i,i,DIFF_SAME);
 int nn=(int)(n-prefix-suffix),mm=(int)(m-prefix-suffix),found=-1;
 int *v=NULL,**trace=NULL;uint8_t *ops=NULL;
 if(!nn||!mm){changed(r,prefix,(uint32_t)nn,prefix,(uint32_t)mm);goto finish;}
 v=malloc((2*DIFF_EDITS+3)*sizeof *v);trace=calloc(DIFF_EDITS+1,sizeof *trace);ops=malloc((size_t)nn+mm);
 if(!v||!trace||!ops)goto nomem;for(int i=0;i<2*DIFF_EDITS+3;i++)v[i]=-1;int off=DIFF_EDITS+1;v[off+1]=0;
 for(int d=0;d<=DIFF_EDITS&&work<DIFF_WORK;d++){
  if(stopped(cancel))goto cancelled;
  trace[d]=malloc((size_t)(2*d+1)*sizeof **trace);if(!trace[d])goto nomem;
  for(int k=-d;k<=d;k+=2){
   int x=(k==-d||(k!=d&&v[off+k-1]<v[off+k+1]))?v[off+k+1]:v[off+k-1]+1;int y=x-k;
   work++;while(x<nn&&y<mm&&equal(r,prefix+(uint32_t)x,prefix+(uint32_t)y)){x++;y++;work++;if((work&1023)==0&&(stopped(cancel)||work>=DIFF_WORK))break;}
   v[off+k]=x;trace[d][k+d]=x;
   if(x>=nn&&y>=mm){found=d;break;}if(work>=DIFF_WORK)break;
  }if(found>=0)break;
 }
 if(found<0){r->coarse=1;changed(r,prefix,(uint32_t)nn,prefix,(uint32_t)mm);goto finish;}
 int x=nn,y=mm;uint32_t used=0;
 for(int d=found;d>0;d--){int k=x-y;int *prev=trace[d-1];int pk=(k==-d||(k!=d&&prev[k-1+d-1]<prev[k+1+d-1]))?k+1:k-1;int px=prev[pk+d-1],py=px-pk;
  while(x>px&&y>py){ops[used++]=0;x--;y--;}if(x==px){ops[used++]=2;y--;}else{ops[used++]=1;x--;}}
 while(x>0&&y>0){ops[used++]=0;x--;y--;}
 uint32_t a=prefix,b=prefix,da=0,db=0;for(uint32_t i=used;i>0;i--){uint8_t op=ops[i-1];if(!op){if(da||db){changed(r,a-da,da,b-db,db);da=db=0;}row(r,a++,b++,DIFF_SAME);}else if(op==1){a++;da++;}else{b++;db++;}}
 if(da||db)changed(r,a-da,da,b-db,db);
finish:
 for(uint32_t i=0;i<suffix;i++)row(r,n-suffix+i,m-suffix+i,DIFF_SAME);
 for(uint32_t i=0;i<r->count;i++){if(r->rows[i].kind!=DIFF_SAME){if(!i||r->rows[i-1].kind==DIFF_SAME)r->hunks++;r->rows[i].hunk=r->hunks;}}
 if(trace)for(int i=0;i<=DIFF_EDITS;i++)free(trace[i]);free(trace);free(v);free(ops);return stopped(cancel)?ERROR_CANCELLED:0;
nomem:if(trace)for(int i=0;i<=DIFF_EDITS;i++)free(trace[i]);free(trace);free(v);free(ops);return ERROR_NOT_ENOUGH_MEMORY;
cancelled:if(trace)for(int i=0;i<=DIFF_EDITS;i++)free(trace[i]);free(trace);free(v);free(ops);return ERROR_CANCELLED;
}
DWORD diff_buffers(const uint8_t *a,size_t na,const uint8_t *b,size_t nb,HANDLE cancel,diff_result *r){
 r->doc[0].bytes=na;r->doc[1].bytes=nb;r->bytes_equal=na==nb&&(!na||!memcmp(a,b,na));
 if(stopped(cancel))return r->error=ERROR_CANCELLED;
 if(na>DIFF_TEXT_BYTES||nb>DIFF_TEXT_BYTES){r->mode=DIFF_LARGE;return 0;}
 int x=text_decode(a,na,0,TEXT_AUTO,&r->doc[0].data),y=text_decode(b,nb,0,TEXT_AUTO,&r->doc[1].data);
 if(x==-1||y==-1)return r->error=ERROR_NOT_ENOUGH_MEMORY;
 if(x==-2||y==-2){r->mode=DIFF_BINARY;return 0;}
 if((r->doc[0].data.flags|r->doc[1].data.flags)&TEXTP_PARTIAL){r->mode=DIFF_LARGE;return 0;}
 DWORD e=lines(&r->doc[0]);if(!e)e=lines(&r->doc[1]);if(e==ERROR_BUFFER_OVERFLOW){r->mode=DIFF_LARGE;return 0;}if(e)return r->error=e;
 return r->error=align(r,cancel);
}
int diff_change(const diff_result *r,int at,int direction){
 if(!r->count||!r->hunks)return -1;int n=(int)r->count;
 if(direction>0){for(int i=at<0?0:at+1;i<n;i++)if(r->rows[i].hunk&&(!i||r->rows[i].hunk!=r->rows[i-1].hunk))return i;for(int i=0;i<n;i++)if(r->rows[i].hunk)return i;}
 else{int current=at>=0&&at<n?(int)r->rows[at].hunk:0;for(int i=at>=n?n-1:at-1;i>=0;i--)if(r->rows[i].hunk&&(int)r->rows[i].hunk!=current){while(i>0&&r->rows[i-1].hunk==r->rows[i].hunk)i--;return i;}for(int i=n-1;i>=0;i--)if(r->rows[i].hunk){while(i>0&&r->rows[i-1].hunk==r->rows[i].hunk)i--;return i;}}
 return -1;
}
