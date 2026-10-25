#include "mft.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
uint16_t mft_u16(const void *p){const uint8_t *b=p;return (uint16_t)(b[0]|b[1]<<8);}
uint32_t mft_u32(const void *p){const uint8_t *b=p;return (uint32_t)b[0]|(uint32_t)b[1]<<8|(uint32_t)b[2]<<16|(uint32_t)b[3]<<24;}
uint64_t mft_u64(const void *p){const uint8_t *b=p;return (uint64_t)mft_u32(b)|(uint64_t)mft_u32(b+4)<<32;}
static void put16(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put32(uint8_t *p,uint32_t v){for(int i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static void put64(uint8_t *p,uint64_t v){for(int i=0;i<8;i++)p[i]=(uint8_t)(v>>(8*i));}
static int grow(void **p,uint32_t *capacity,uint32_t need,size_t width){if(need<=*capacity)return 1;uint32_t cap=*capacity?*capacity:1024;while(cap<need){if(cap>UINT32_MAX/2){cap=need;break;}cap*=2;}if((size_t)cap>SIZE_MAX/width)return 0;void *q=realloc(*p,(size_t)cap*width);if(!q)return 0;*p=q;*capacity=cap;return 1;}
int mft_fixup(uint8_t *p,size_t size,uint32_t sector){
 if(size<48||memcmp(p,"FILE",4)||sector<256||sector>size||size%sector)return 0;
 uint32_t offset=mft_u16(p+4),count=mft_u16(p+6);
 if(count!=size/sector+1||offset<8||offset>size||count>(size-offset)/2)return 0;
 uint16_t sequence=mft_u16(p+offset);for(uint32_t i=1;i<count;i++)if(mft_u16(p+(size_t)i*sector-2)!=sequence)return 0;
 for(uint32_t i=1;i<count;i++)put16(p+(size_t)i*sector-2,mft_u16(p+offset+i*2));return 1;
}
int mft_runs(const uint8_t *a,size_t size,mft_run *out,uint32_t *count,uint32_t capacity,uint64_t total){
 if(size<64||a[8]!=1)return 0;uint64_t vcn=mft_u64(a+16),last=mft_u64(a+24);uint32_t at=mft_u16(a+32);int64_t lcn=0;
 if(at<64||at>=size||last<vcn)return 0;
 while(at<size){uint8_t head=a[at++];if(!head)return vcn&&vcn-1==last;unsigned n=head&15,d=head>>4;
  /* MFT itself cannot contain sparse runs. No arbitrary file extents are read. */
  if(!n||n>8||!d||d>8||n+d>size-at||*count>=capacity)return 0;
  uint64_t length=0,raw=0;for(unsigned i=0;i<n;i++)length|=(uint64_t)a[at++]<<(8*i);for(unsigned i=0;i<d;i++)raw|=(uint64_t)a[at++]<<(8*i);
  if(d<8&&(raw&(UINT64_C(1)<<(d*8-1))))raw|=UINT64_MAX<<(d*8);int64_t delta;memcpy(&delta,&raw,8);
  if((delta>0&&lcn>INT64_MAX-delta)||(delta<0&&lcn<INT64_MIN-delta)||!length||vcn>UINT64_MAX-length)return 0;lcn+=delta;
  if(lcn<0||(uint64_t)lcn>=total||length>total-(uint64_t)lcn)return 0;
  out[(*count)++]=(mft_run){vcn,(uint64_t)lcn,length};vcn+=length;
 }return 0;
}
static int add_name(mft_model *m,uint64_t child,const uint8_t *p,size_t size){
 if(size<66||p[64]==0||size<66u+(size_t)p[64]*2)return 0;
 if(p[65]==2){m->dos_names++;return 1;}if(p[65]>3)return 0;
 uint32_t length=p[64];if(m->text_count>UINT32_MAX-length-1||m->name_count==UINT32_MAX)return 0;
 if(!grow((void**)&m->names,&m->name_capacity,m->name_count+1,sizeof *m->names)||!grow((void**)&m->text,&m->text_capacity,m->text_count+length+1,sizeof *m->text))return 0;
 uint32_t offset=m->text_count;for(uint32_t i=0;i<length;i++){uint16_t ch=mft_u16(p+66+i*2);if(!ch||ch=='\\'||ch=='/')return 0;m->text[m->text_count++]=ch;}m->text[m->text_count++]=0;
 m->names[m->name_count++]=(mft_name){.child=child,.parent=mft_u64(p),.next=MFT_NONE,.offset=offset,.length=(uint16_t)length,.space=p[65]};return 1;
}
/* Some metadata streams (notably $BadClus:$Bad) encode holes without the
 * ordinary sparse flag. For a complete mapping, count physical runs directly. */
static int complete_sparse_allocation(const uint8_t *a,uint32_t size,uint32_t cluster,uint64_t *allocation){
 if(size<64||!cluster||mft_u64(a+16))return 0;uint64_t allocated=mft_u64(a+40),highest=mft_u64(a+24);uint32_t at=mft_u16(a+32);
 if(at<64||at>=size||allocated%cluster||highest==UINT64_MAX||highest+1!=allocated/cluster)return 0;
 uint64_t covered=0,physical=0;int hole=0;
 while(at<size){uint8_t head=a[at++];if(!head){if(!hole||covered!=highest+1||physical>UINT64_MAX/cluster)return 0;*allocation=physical*cluster;return 1;}
  unsigned n=head&15,d=head>>4;if(!n||n>8||d>8||n+d>size-at)return 0;uint64_t length=0;for(unsigned i=0;i<n;i++)length|=(uint64_t)a[at++]<<(8*i);
  if(!length||covered>UINT64_MAX-length)return 0;covered+=length;if(d){if(physical>UINT64_MAX-length)return 0;physical+=length;}else hole=1;at+=d;
 }return 0;
}
static int apply_data(mft_node *n,uint64_t logical,uint64_t allocated,int unnamed){
 if(n->all_logical>UINT64_MAX-logical||n->all_allocated>UINT64_MAX-allocated||n->streams==UINT16_MAX)return 0;
 if(unnamed){if(n->flags&MF_DATA)return 0;n->logical=logical;n->allocated=allocated;n->flags|=MF_DATA;}
 n->all_logical+=logical;n->all_allocated+=allocated;n->streams++;return 1;
}
int mft_record(mft_model *m,const uint8_t *p,size_t size,uint32_t index){
 if(index>=m->slots||size<48||memcmp(p,"FILE",4))return 0;m->records++;
 uint16_t flags=mft_u16(p+22);if(!(flags&1)){m->unused++;return 1;}
 uint64_t base=mft_u64(p+32),ref=base?base:((uint64_t)mft_u16(p+16)<<48)|index;uint64_t target=ref&MFT_INDEX_MASK;
 if(target>=m->slots)return 0;mft_node *node=&m->nodes[target];
 if(base)m->extensions++;else{m->active++;node->reference=ref;node->flags|=MF_ACTIVE|((flags&2)?MF_DIRECTORY:0);node->links=mft_u16(p+18);}
 uint32_t at=mft_u16(p+20),used=mft_u32(p+24);if(at<48||at%8||used>size||at>used)return 0;
 while(at+4<=used){const uint8_t *a=p+at;uint32_t type=mft_u32(a);if(type==UINT32_MAX)return 1;if(used-at<16)return 0;uint32_t length=mft_u32(a+4);
  if(length<24||length%8||length>used-at||a[8]>1)return 0;
  uint32_t name_length=a[9],name_offset=mft_u16(a+10);if(name_length&&(name_offset<16||name_offset>length||name_length>(length-name_offset)/2))return 0;
  const uint8_t *value=NULL;uint32_t value_length=0;
  if(!a[8]){uint32_t offset=mft_u16(a+20);value_length=mft_u32(a+16);if(offset<24||offset>length||value_length>length-offset)return 0;value=a+offset;}
  else if(length<64||mft_u64(a+24)<mft_u64(a+16))return 0;
  if(type==0x10){if(!value||value_length<36)return 0;if(!base)node->attributes=mft_u32(value+32);}
  else if(type==0x20){if(!(node->flags&MF_ATTRLIST))m->attribute_lists++;node->flags|=MF_ATTRLIST;}
  else if(type==0x30){if(!value||!add_name(m,ref,value,value_length))return 0;}
  else if(type==0x80&&(!a[8]||!mft_u64(a+16))){
   uint64_t logical=value?value_length:mft_u64(a+48),allocated=0;
   if(!value){allocated=mft_u64(a+40);if(mft_u16(a+12)&0x80ff){if(length<72||mft_u16(a+32)<72)return 0;allocated=mft_u64(a+64);}}
   if(!value&&name_length)complete_sparse_allocation(a,length,m->cluster_size,&allocated);
   if(logical>INT64_MAX||allocated>INT64_MAX)return 0;
   if(base){if(!grow((void**)&m->pending,&m->pending_capacity,m->pending_count+1,sizeof *m->pending))return 0;m->pending[m->pending_count++]=(mft_data){ref,logical,allocated,!name_length};}
   else if(!apply_data(node,logical,allocated,!name_length))return 0;
  }
  at+=length;
 }return 0;
}
void mft_resolve(mft_model *m){
 for(uint32_t i=0;i<m->pending_count;i++){mft_data *d=&m->pending[i];mft_node *n=&m->nodes[d->child&MFT_INDEX_MASK];if(!(n->flags&MF_ACTIVE)||n->reference!=d->child){m->bad_sequence++;continue;}if(!apply_data(n,d->logical,d->allocated,d->unnamed)){n->flags|=MF_BAD;m->bad_attributes++;}}
 for(uint32_t i=0;i<m->slots;i++)m->nodes[i].first=MFT_NONE;
 for(uint32_t i=0;i<m->name_count;i++){mft_name *name=&m->names[i];uint64_t c=name->child&MFT_INDEX_MASK,p=name->parent&MFT_INDEX_MASK;
  if(c>=m->slots||p>=m->slots||m->nodes[c].reference!=name->child||m->nodes[p].reference!=name->parent||!(m->nodes[c].flags&MF_ACTIVE)||!(m->nodes[p].flags&MF_DIRECTORY)){m->bad_sequence++;continue;}
  mft_node *n=&m->nodes[c];int duplicate=0;for(uint32_t at=n->first;at!=MFT_NONE;at=m->names[at].next){mft_name *prior=&m->names[at];if(prior->parent==name->parent&&prior->length==name->length&&!memcmp(m->text+prior->offset,m->text+name->offset,name->length*2)){duplicate=1;break;}}
  if(duplicate)continue;name->valid=1;name->next=n->first;n->first=i;
 }
}
void mft_free(mft_model *m){free(m->nodes);free(m->names);free(m->text);free(m->pending);memset(m,0,sizeof *m);}
/* Parser checks stay with the standalone prototype; no product suite is replaced. */
int mft_selftest(void){int pass=0,fail=0;
#define VERIFY(x) do{if(x)pass++;else{printf("FAIL parser line %d\n",__LINE__);fail++;}}while(0)
 uint8_t record[1024]={0};memcpy(record,"FILE",4);put16(record+4,48);put16(record+6,3);put16(record+48,0xaabb);put16(record+50,0x1122);put16(record+52,0x3344);put16(record+510,0xaabb);put16(record+1022,0xaabb);
 VERIFY(mft_fixup(record,1024,512));VERIFY(mft_u16(record+510)==0x1122&&mft_u16(record+1022)==0x3344);VERIFY(!mft_fixup(record,1024,512));put16(record+6,1000);VERIFY(!mft_fixup(record,1024,512));
 uint8_t attr[96]={0};attr[8]=1;put16(attr+32,64);put64(attr+24,11);attr[64]=0x11;attr[65]=8;attr[66]=100;attr[67]=0x11;attr[68]=4;attr[69]=0xf6;attr[70]=0; mft_run runs[8];uint32_t count=0;
 VERIFY(mft_runs(attr,96,runs,&count,8,1000));VERIFY(count==2&&runs[0].lcn==100&&runs[1].lcn==90&&runs[1].vcn==8);count=0;VERIFY(!mft_runs(attr,96,runs,&count,1,1000));attr[64]=0x01;count=0;VERIFY(!mft_runs(attr,96,runs,&count,8,1000));attr[64]=0x11;put64(attr+24,15);count=0;VERIFY(!mft_runs(attr,96,runs,&count,8,1000));
 mft_model m={.slots=10};m.nodes=calloc(m.slots,sizeof *m.nodes);memset(record,0,sizeof record);memcpy(record,"FILE",4);put16(record+16,2);put16(record+18,1);put16(record+20,56);put16(record+22,1);put32(record+24,128);
 uint8_t *a=record+56;put32(a,0x80);put32(a+4,64);a[8]=1;put16(a+32,64);put64(a+40,4096);put64(a+48,1234);put32(record+120,UINT32_MAX);
 VERIFY(mft_record(&m,record,sizeof record,6));VERIFY(m.nodes[6].logical==1234&&m.nodes[6].allocated==4096&&m.nodes[6].reference==(UINT64_C(2)<<48|6));put32(a+4,65536);VERIFY(!mft_record(&m,record,sizeof record,7));
 put32(a+4,64);put16(a+12,0x8000);VERIFY(!mft_record(&m,record,sizeof record,7));put32(a+4,72);put16(a+32,72);put64(a+64,512);put32(record+128,UINT32_MAX);put32(record+24,136);VERIFY(mft_record(&m,record,sizeof record,8));VERIFY(m.nodes[8].allocated==512);
 memset(record,0,sizeof record);memcpy(record,"FILE",4);put16(record+16,3);put16(record+20,56);put16(record+22,1);put32(record+24,96);a=record+56;put32(a,0x80);put32(a+4,32);put32(a+16,3);put16(a+20,24);put32(record+88,UINT32_MAX);VERIFY(mft_record(&m,record,sizeof record,9));VERIFY(m.nodes[9].logical==3&&m.nodes[9].allocated==0);

 /* Literal file names, DOS aliases, hard links and sequence validation. */
 uint8_t name[80]={0};put64(name,(UINT64_C(4)<<48)|5);name[64]=1;name[65]=1;put16(name+66,'x');
 m.nodes[5].reference=(UINT64_C(4)<<48)|5;m.nodes[5].flags=MF_ACTIVE|MF_DIRECTORY;
 VERIFY(add_name(&m,m.nodes[6].reference,name,68));name[65]=2;VERIFY(add_name(&m,m.nodes[6].reference,name,68)&&m.dos_names==1);name[65]=3;VERIFY(add_name(&m,m.nodes[6].reference,name,68));
 put16(name+66,'y');VERIFY(add_name(&m,m.nodes[6].reference,name,68));put64(name,(UINT64_C(8)<<48)|5);VERIFY(add_name(&m,m.nodes[6].reference,name,68));name[64]=0;VERIFY(!add_name(&m,m.nodes[6].reference,name,68));name[64]=1;put16(name+66,'/');VERIFY(!add_name(&m,m.nodes[6].reference,name,68));
 mft_resolve(&m);VERIFY(m.bad_sequence==1);VERIFY(m.nodes[6].first!=MFT_NONE&&m.names[m.nodes[6].first].next!=MFT_NONE);VERIFY(m.name_count==4);
 memset(attr,0,sizeof attr);attr[8]=1;put16(attr+32,64);put64(attr+24,7);put64(attr+40,32768);attr[64]=0x01;attr[65]=8;attr[66]=0;uint64_t physical=123;
 VERIFY(complete_sparse_allocation(attr,96,4096,&physical)&&physical==0);attr[64]=0x11;attr[65]=4;attr[66]=10;attr[67]=0x01;attr[68]=4;attr[69]=0;VERIFY(complete_sparse_allocation(attr,96,4096,&physical)&&physical==16384);put64(attr+40,65536);VERIFY(!complete_sparse_allocation(attr,96,4096,&physical));
 mft_free(&m);m=(mft_model){.slots=10};m.nodes=calloc(m.slots,sizeof *m.nodes);m.nodes[6].reference=(UINT64_C(2)<<48)|6;m.nodes[6].flags=MF_ACTIVE;
 m.pending=calloc(2,sizeof *m.pending);m.pending_capacity=m.pending_count=2;m.pending[0]=(mft_data){m.nodes[6].reference,42,4096,1};m.pending[1]=(mft_data){(UINT64_C(3)<<48)|6,99,8192,0};mft_resolve(&m);VERIFY(m.nodes[6].logical==42&&m.nodes[6].allocated==4096&&m.bad_sequence==1);VERIFY(!apply_data(&m.nodes[6],7,9,1));
 mft_free(&m);printf("%d parser checks passed, %d failed\n",pass,fail);return fail?1:0;
#undef VERIFY
}
