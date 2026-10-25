/* Included by fpspace.c. One cancellable page worker, bounded LRU, two pinned scopes. */
static focus_entry *focus_find(uint32_t scope){for(unsigned i=0;i<SPACE_CACHE_SLOTS;i++)if(F.entries[i].valid&&F.entries[i].scope==scope)return F.entries+i;return NULL;}
static int focus_receive(void *user,const sp_record *r,const wchar_t *name){focus_job *job=user;if(WaitForSingleObject(job->cancel,0)==WAIT_OBJECT_0)return 0;
 if(r->type==SP_ENTRY){if(job->page_seen){job->error=ERROR_INVALID_DATA;return 0;}if(!r->id)return space_model_record(&job->model,r,name);if(r->flags&SP_DIR){job->error=ERROR_INVALID_DATA;return 0;}
  size_t bytes=(wcslen(name)+1)*2;if(job->model.count>SPACE_LIST_FILES||job->model.name_bytes+bytes>SPACE_LIST_NAMES+2){job->error=ERROR_BUFFER_OVERFLOW;return 0;}
  sp_record file=*r;file.id=job->model.count;file.parent=0;int ok=space_model_record(&job->model,&file,name);if(ok&&r->reserved&&!(file.flags&SP_SKIP))job->model.nodes[file.id].flags|=SP_HARDLINK;return ok;
 }if(r->type==SP_PAGE){if(job->page_seen||!job->model.count||r->magic!=SP_MAGIC||r->logical!=job->request.page||r->reserved!=job->model.count-1||r->id>=1000000000||r->parent>=1000000000||r->error||r->flags&~SP_PAGE_MORE||r->allocated>UINT64_MAX-r->reserved){job->error=ERROR_INVALID_DATA;return 0;}
  job->page_seen=1;job->first=r->allocated;job->more=!!(r->flags&SP_PAGE_MORE);job->result=(sp_page_request){.page=r->logical,.device=job->model.root_device,.identity=job->model.root_identity,.mtime=r->device,.ctime=r->identity,.mtime_ns=r->id,.ctime_ns=r->parent};return 1;
 }if(r->type==SP_END)return space_model_record(&job->model,r,NULL);return 1;
}
static DWORD WINAPI focus_worker(void *arg){focus_job *job=arg;DWORD error=space_scan_page(job->path,job->wsl,job->as_root,job->cancel,focus_receive,job,&job->request);if(!job->error)job->error=error;if(!job->error)job->error=job->model.error;if(!job->error&&!job->page_seen)job->error=ERROR_INVALID_DATA;if(!job->error&&job->model.state==SP_PARTIAL)job->error=ERROR_PARTIAL_COPY;return 0;}
static void focus_evict(unsigned i){focus_entry *entry=F.entries+i;if(!entry->valid)return;space_model_list_clear(&A.model,entry->scope);F.files-=entry->count;F.names-=entry->names;memset(entry,0,sizeof *entry);if(A.selected<A.model.count&&A.model.nodes[A.selected].flags&SP_REMOVED)A.selected=SP_NONE;if(B.left_selected<A.model.count&&A.model.nodes[B.left_selected].flags&SP_REMOVED)B.left_selected=SP_NONE;if(B.right_selected<A.model.count&&A.model.nodes[B.right_selected].flags&SP_REMOVED)B.right_selected=SP_NONE;}
static int focus_pinned(uint32_t scope){return scope==A.scope||scope==B.right_scope;}
static int focus_slot(uint32_t count,uint64_t names){for(;;){int empty=-1,old=-1;for(unsigned i=0;i<SPACE_CACHE_SLOTS;i++){focus_entry *e=F.entries+i;if(!e->valid)empty=(int)i;else if(!focus_pinned(e->scope)&&(old<0||e->used<F.entries[old].used))old=(int)i;}
  if(empty>=0&&F.files+count<=SPACE_CACHE_FILES&&F.names+names<=SPACE_CACHE_NAMES)return empty;if(old<0)return -1;focus_evict((unsigned)old);}}
static void focus_reset(void){if(F.job){SetEvent(F.job->cancel);CancelSynchronousIo(F.job->thread);}memset(F.entries,0,sizeof F.entries);F.clock=F.names=F.files=F.error=0;}
static int focus_page(uint32_t scope,int direction){focus_entry *e=focus_find(scope);if(!e||e->pending||(F.job&&F.job->scope==scope))return 0;if(direction&&(e->error==ERROR_FILE_INVALID||(direction<0?!e->page.page:!e->more)))return 0;
 e->target=direction<0?e->page.page-1:direction>0?e->page.page+1:0;e->fresh=!direction;e->pending=1;e->error=0;F.error=0;return 1;}
static void focus_request(uint32_t scope){if(A.thread||!A.shown||A.view||A.model.backend!=SP_BACKEND_SUMMARY||scope>=A.model.count||A.model.nodes[scope].flags&(SP_FAILED|SP_SKIP|SP_REMOVED))return;
 focus_entry *entry=focus_find(scope);if(entry){entry->used=++F.clock;if(!entry->pending)return;}if(F.job)return;
 focus_job *job=calloc(1,sizeof *job);if(!job)return;job->path=space_model_path(&A.model,scope,A.root);job->scope=scope;job->epoch=A.model_epoch;job->device=A.model.stamps[scope].device;job->identity=A.model.stamps[scope].identity;job->wsl=A.request.wsl;job->as_root=A.request.as_root;
 if(entry){if(!entry->fresh)job->request=entry->page;job->request.page=entry->target;}job->request.device=job->device;job->request.identity=job->identity;
 job->cancel=CreateEventW(NULL,TRUE,FALSE,NULL);if(job->path&&job->cancel)job->thread=CreateThread(NULL,0,focus_worker,job,0,NULL);
 if(!job->thread){if(job->cancel)CloseHandle(job->cancel);free(job->path);free(job);if(entry){entry->pending=0;entry->error=ERROR_NOT_ENOUGH_MEMORY;}F.error=ERROR_NOT_ENOUGH_MEMORY;return;}F.job=job;
}
static int focus_tick(void){int changed=0;focus_job *job=F.job;
 if(job&&WaitForSingleObject(job->thread,0)==WAIT_OBJECT_0){F.job=NULL;focus_entry *previous=focus_find(job->scope);int current=job->epoch==A.model_epoch&&A.model.backend==SP_BACKEND_SUMMARY&&job->scope<A.model.count&&focus_pinned(job->scope)&&!A.view&&WaitForSingleObject(job->cancel,0)!=WAIT_OBJECT_0&&(!previous||(previous->pending&&previous->target==job->request.page));
  if(current){DWORD error=job->error;if(!job->model.count||job->model.root_device!=job->device||job->model.root_identity!=job->identity)error=ERROR_FILE_INVALID;
   int usable=job->page_seen&&(!error||error==ERROR_PARTIAL_COPY);uint32_t count=usable?job->model.count-1:0;uint64_t names=usable&&job->model.name_bytes>=2?job->model.name_bytes-2:0;
   if(previous&&!usable){previous->pending=0;previous->error=error;}
   else{if(previous)focus_evict((unsigned)(previous-F.entries));int slot=focus_slot(count,names);
    if(slot>=0){focus_entry *entry=F.entries+slot;*entry=(focus_entry){.scope=job->scope,.used=++F.clock,.error=error,.valid=1,.first=job->first,.more=usable&&job->more,.page=job->result};if(usable)for(uint32_t i=1;i<job->model.count;i++){if(space_model_list_add(&A.model,job->scope,job->model.nodes+i)==SP_NONE){entry->error=ERROR_NOT_ENOUGH_MEMORY;entry->more=0;break;}entry->count++;entry->names+=(wcslen(job->model.nodes[i].name)+1)*2;}F.files+=entry->count;F.names+=entry->names;error=entry->error;}else error=ERROR_BUFFER_OVERFLOW;
   }F.error=error;A.dirty=1;A.refreshed=0;changed=1;
  }else if(previous&&previous->pending&&previous->target==job->request.page)previous->pending=0;
  space_model_free(&job->model);free(job->path);CloseHandle(job->thread);CloseHandle(job->cancel);free(job);
 }
 if(F.job&&(!focus_pinned(F.job->scope)||A.view||!A.shown)){SetEvent(F.job->cancel);CancelSynchronousIo(F.job->thread);}focus_request(A.scope);focus_request(B.right_scope);return changed;
}
static int focus_shutdown(void){if(F.job){SetEvent(F.job->cancel);CancelSynchronousIo(F.job->thread);if(WaitForSingleObject(F.job->thread,3000)!=WAIT_OBJECT_0)return 0;focus_job *j=F.job;F.job=NULL;space_model_free(&j->model);free(j->path);CloseHandle(j->thread);CloseHandle(j->cancel);free(j);}return 1;}
