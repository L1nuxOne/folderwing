/* Folder-only traversal. File metadata is folded into direct totals and discarded.
 * Only multiply linked identities require scan-wide allocation attribution. */
typedef struct sp_link {uint64_t device,ino;int used;} sp_link;
typedef struct sp_links {sp_link *items;uint32_t count,capacity;} sp_links;
static uint32_t sp_link_hash(uint64_t device,uint64_t ino,uint32_t mask){uint64_t h=ino^(device*0x9e3779b97f4a7c15ull);h^=h>>33;h*=0xff51afd7ed558ccdull;return (uint32_t)h&mask;}
static int sp_link_seen(sp_links *l,const struct stat *st){
 if(st->st_nlink<2)return 0;
 if(!l->capacity||l->count>=l->capacity/2){uint32_t cap=l->capacity?l->capacity*2:1024;if(cap>(1u<<22))return -1;sp_link *p=calloc(cap,sizeof *p);if(!p)return -1;for(uint32_t i=0;i<l->capacity;i++)if(l->items[i].used){sp_link v=l->items[i];uint32_t at=sp_link_hash(v.device,v.ino,cap-1);while(p[at].used)at=(at+1)&(cap-1);p[at]=v;}free(l->items);l->items=p;l->capacity=cap;}
 uint32_t at=sp_link_hash(st->st_dev,st->st_ino,l->capacity-1);while(l->items[at].used){if(l->items[at].device==(uint64_t)st->st_dev&&l->items[at].ino==(uint64_t)st->st_ino)return 1;at=(at+1)&(l->capacity-1);}l->items[at]=(sp_link){st->st_dev,st->st_ino,1};l->count++;return 0;
}
static int sp_sum_add(space_scan *s,uint64_t *to,uint64_t value){if(UINT64_MAX-*to<value){s->error=EOVERFLOW;return 0;}*to+=value;return 1;}
/* TOTAL fields: logical/allocated bytes, device=file count, parent=excluded,
 * error=failed files/enumerations, flags=unknown allocations, reserved=hardlinks.
 * Child directory counts/errors are represented by their own ENTRY records. */
static void sp_summary_walk(space_scan *s,sp_links *links,int fd,const struct stat *st,uint32_t parent,const char *name,unsigned depth,int denied){
 if(!sp_live(s)||s->error||s->next>=SP_SUMMARY_LIMIT){if(fd>=0)close(fd);return;}uint32_t id=s->next++;
 if(depth>=128)denied=EOVERFLOW;
 sp_record entry={.type=SP_ENTRY,.id=id,.parent=parent,.flags=SP_DIR|(denied?SP_FAILED:0),.error=denied,.device=st?st->st_dev:0,.identity=st?st->st_ino:0};
 if(!sp_send(s,&entry,name)){if(fd>=0)close(fd);return;}
 sp_record total={.type=SP_TOTAL,.id=id};DIR *dir=NULL;
 if(fd>=0&&!denied){dir=fdopendir(fd);if(!dir){total.error++;s->errors++;close(fd);}}else if(fd>=0)close(fd);
 while(dir&&sp_live(s)&&!s->error&&s->next<SP_SUMMARY_LIMIT){errno=0;struct dirent *e=readdir(dir);if(!e){if(errno){total.error++;s->errors++;}break;}if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
  struct stat item;int child=fp_mount_open(dirfd(dir),e->d_name,O_PATH),err=child<0?errno:0;
  if(!err&&fstat(child,&item))err=errno;
  if(!err&&S_ISDIR(item.st_mode)){struct stat check;int opened=fp_mount_open(dirfd(dir),e->d_name,O_RDONLY|O_DIRECTORY);int denied_dir=opened<0?errno:0;
   if(opened>=0&&(fstat(opened,&check)||check.st_dev!=item.st_dev||check.st_ino!=item.st_ino)){denied_dir=ESTALE;close(opened);opened=-1;}
   close(child);sp_summary_walk(s,links,opened,&item,id,e->d_name,depth+1,denied_dir);continue;}
  if(child>=0)close(child);
  if(err){total.parent++;if(err!=EXDEV&&err!=ELOOP){total.error++;s->errors++;}continue;}
  if(!S_ISREG(item.st_mode)){total.parent++;continue;}
  if(!sp_sum_add(s,&total.device,1))break;
  if(item.st_size<0||item.st_blocks<0){total.error++;total.flags++;s->errors++;continue;}
  if(!sp_sum_add(s,&total.logical,(uint64_t)item.st_size))break;
  int seen=sp_link_seen(links,&item);if(seen<0){s->error=ENOMEM;total.flags++;break;}if(seen){total.reserved++;continue;}
  if(!sp_sum_add(s,&total.allocated,(uint64_t)item.st_blocks*512))break;
 }
 if(dir)closedir(dir);sp_send(s,&total,NULL);sp_record leave={.type=SP_LEAVE,.id=id};sp_send(s,&leave,NULL);
}
