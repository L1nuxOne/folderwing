/* Read-only NTFS MFT prototype. No volume writes, locks, dismounts or flushes. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>
#include <inttypes.h>
#include "mft.h"
#define READ_CHUNK (4u*1024u*1024u)
#define MAX_SLOTS 16000000u
static double frequency;
static uint64_t ticks(void){LARGE_INTEGER n;QueryPerformanceCounter(&n);return n.QuadPart;}
static double ms(uint64_t n){return n*1000.0/frequency;}
static int read_at(HANDLE h,uint64_t offset,void *p,DWORD size){LARGE_INTEGER at;at.QuadPart=(LONGLONG)offset;DWORD done=0;if(offset>INT64_MAX||!SetFilePointerEx(h,at,NULL,FILE_BEGIN)||!ReadFile(h,p,size,&done,NULL)||done!=size)return 0;return 1;}
static int bootstrap(HANDLE volume,NTFS_VOLUME_DATA_BUFFER *geometry,mft_run *runs,uint32_t *count){
 BYTE data[sizeof(NTFS_VOLUME_DATA_BUFFER)+64];DWORD returned=0;
 if(!DeviceIoControl(volume,FSCTL_GET_NTFS_VOLUME_DATA,NULL,0,data,sizeof data,&returned,NULL)||returned<sizeof *geometry)return 0;memcpy(geometry,data,sizeof *geometry);
 if(returned<sizeof *geometry+8||mft_u16(data+sizeof *geometry+4)!=3||mft_u16(data+sizeof *geometry+6)>1){SetLastError(ERROR_NOT_SUPPORTED);return 0;}
 uint32_t size=geometry->BytesPerFileRecordSegment,sector=geometry->BytesPerSector,cluster=geometry->BytesPerCluster;
 if(size<512||size>65536||(size&(size-1))||sector<256||cluster<sector||size%sector||geometry->MftValidDataLength.QuadPart<=0||geometry->MftValidDataLength.QuadPart%size||geometry->MftStartLcn.QuadPart<0){SetLastError(ERROR_INVALID_DATA);return 0;}
 uint8_t *record=malloc(size);if(!record)return 0;int ok=read_at(volume,(uint64_t)geometry->MftStartLcn.QuadPart*cluster,record,size)&&mft_fixup(record,size,sector);uint32_t used=ok?mft_u32(record+24):0,at=ok?mft_u16(record+20):0;int found=0;
 if(used>size||at<48||at>used)ok=0;
 while(ok&&at+16<=used){uint8_t *a=record+at;uint32_t type=mft_u32(a),length=mft_u32(a+4);if(type==UINT32_MAX)break;if(length<24||length>used-at||length%8){ok=0;break;}
  /* This bounded first prototype rejects a fragmented bootstrap requiring an
   * attribute-list chase instead of silently missing any part of the MFT. */
  if(type==0x20){ok=0;SetLastError(ERROR_NOT_SUPPORTED);break;}
  if(type==0x80&&!a[9]){if(found||!a[8]||mft_u64(a+16)||mft_u16(a+12)||!mft_runs(a,length,runs,count,4096,(uint64_t)geometry->TotalClusters.QuadPart)){ok=0;break;}found=1;}
  at+=length;
 }
 uint64_t expected=0;for(uint32_t i=0;ok&&i<*count;i++){if(runs[i].vcn!=expected||expected>UINT64_MAX-runs[i].clusters)ok=0;expected+=runs[i].clusters;}
 if(!found||expected<((uint64_t)geometry->MftValidDataLength.QuadPart+cluster-1)/cluster)ok=0;
 free(record);if(!ok&&GetLastError()==ERROR_SUCCESS)SetLastError(ERROR_INVALID_DATA);return ok;
}
typedef struct directory {uint32_t id,depth;} directory;
static int deepest(const void *a,const void *b){const directory *x=a,*y=b;return x->depth>y->depth?-1:x->depth<y->depth?1:0;}
static uint32_t parent_id(const mft_model *m,uint32_t id){if(id>=m->slots||m->nodes[id].first==MFT_NONE)return MFT_NONE;return (uint32_t)(m->names[m->nodes[id].first].parent&MFT_INDEX_MASK);}
static uint32_t directory_depth(const mft_model *m,uint32_t id,uint16_t *depth){uint32_t stack[512],n=0,at=id;
 while(at<m->slots&&at!=5&&!(depth[at])){if(n==512||!(m->nodes[at].flags&MF_DIRECTORY))return 0;for(uint32_t i=0;i<n;i++)if(stack[i]==at)return 0;stack[n++]=at;at=parent_id(m,at);}
 if(at>=m->slots)return 0;uint32_t d=at==5?1:depth[at];if(!d)return 0;while(n)depth[stack[--n]]=(uint16_t)++d;return depth[id];}
static int path_for(const mft_model *m,const mft_name *name,wchar_t drive,wchar_t *path,size_t cap){uint32_t chain[512],n=0;uint64_t parent=name->parent;
 while((parent&MFT_INDEX_MASK)!=5){uint64_t at=parent&MFT_INDEX_MASK;if(at>=m->slots||m->nodes[at].reference!=parent||!(m->nodes[at].flags&MF_DIRECTORY)||m->nodes[at].first==MFT_NONE||n>=512)return 0;uint32_t link=m->nodes[at].first;chain[n++]=link;parent=m->names[link].parent;}
 size_t used=3;path[0]=drive;path[1]=':';path[2]='\\';path[3]=0;
 while(n){const mft_name *p=&m->names[chain[--n]];if(used+p->length+1>=cap)return 0;memcpy(path+used,m->text+p->offset,p->length*2);used+=p->length;path[used++]='\\';}
 if(used+name->length>=cap)return 0;memcpy(path+used,m->text+name->offset,name->length*2);used+=name->length;path[used]=0;return 1;
}
static int csv_string(FILE *f,const wchar_t *text){char utf8[131072];int length=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,-1,utf8,sizeof utf8,NULL,NULL);if(!length)return 0;fputc('"',f);for(int i=0;i<length-1;i++){if(utf8[i]=='"')fputc('"',f);fputc(utf8[i],f);}fputc('"',f);return !ferror(f);}
static void stat_path(const wchar_t *path){HANDLE h=CreateFileW(path,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);BY_HANDLE_FILE_INFORMATION i;FILE_STANDARD_INFO s;FILE_COMPRESSION_INFO c;
 if(h==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(h,&i)||!GetFileInformationByHandleEx(h,FileStandardInfo,&s,sizeof s)){printf("{\"error\":%lu}\n",GetLastError());if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);return;}
 uint64_t allocation=(uint64_t)s.AllocationSize.QuadPart;if(i.dwFileAttributes&(FILE_ATTRIBUTE_COMPRESSED|FILE_ATTRIBUTE_SPARSE_FILE)){if(GetFileInformationByHandleEx(h,FileCompressionInfo,&c,sizeof c))allocation=(uint64_t)c.CompressedFileSize.QuadPart;}
 printf("{\"reference\":%" PRIu64 ",\"record\":%" PRIu64 ",\"size\":%" PRIu64 ",\"allocated\":%" PRIu64 ",\"links\":%lu,\"attributes\":%lu}\n",((uint64_t)i.nFileIndexHigh<<32)|i.nFileIndexLow,(((uint64_t)i.nFileIndexHigh<<32)|i.nFileIndexLow)&MFT_INDEX_MASK,((uint64_t)i.nFileSizeHigh<<32)|i.nFileSizeLow,allocation,i.nNumberOfLinks,i.dwFileAttributes);CloseHandle(h);
}
int wmain(int argc,wchar_t **argv){
 if(argc==2&&!wcscmp(argv[1],L"--selftest"))return mft_selftest();if(argc==3&&!wcscmp(argv[1],L"--stat")){stat_path(argv[2]);return 0;}
 if(argc<2||wcslen(argv[1])!=2||argv[1][1]!=':'||!((argv[1][0]>='A'&&argv[1][0]<='Z')||(argv[1][0]>='a'&&argv[1][0]<='z'))){fprintf(stderr,"Usage: mftprobe C: [--csv=owned-output.csv] [--json=owned-output.json]\n");return 2;}
 const wchar_t *csv=NULL,*json=NULL;for(int i=2;i<argc;i++){if(!wcsncmp(argv[i],L"--csv=",6))csv=argv[i]+6;else if(!wcsncmp(argv[i],L"--json=",7))json=argv[i]+7;else return 2;}
 LARGE_INTEGER f;QueryPerformanceFrequency(&f);frequency=(double)f.QuadPart;uint64_t started=ticks(),phase=started,read_ticks=0,io_bytes=0;wchar_t volume_path[]=L"\\\\.\\C:";volume_path[4]=argv[1][0];HANDLE volume=CreateFileW(volume_path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
 if(volume==INVALID_HANDLE_VALUE){fprintf(stderr,"Read-only volume open failed: %lu\n",GetLastError());return 1;}
 NTFS_VOLUME_DATA_BUFFER geometry;mft_run runs[4096];uint32_t run_count=0;if(!bootstrap(volume,&geometry,runs,&run_count)){fprintf(stderr,"MFT bootstrap failed (NTFS 3.x, base-record extent map required): %lu\n",GetLastError());CloseHandle(volume);return 1;}
 uint64_t bytes=(uint64_t)geometry.MftValidDataLength.QuadPart,slots=bytes/geometry.BytesPerFileRecordSegment;if(slots<=5||slots>MAX_SLOTS){fprintf(stderr,"Prototype record-slot budget exceeded: %" PRIu64 "\n",slots);CloseHandle(volume);return 1;}
 mft_model m={.slots=(uint32_t)slots,.cluster_size=geometry.BytesPerCluster};m.nodes=calloc(m.slots,sizeof *m.nodes);uint8_t *buffer=malloc(READ_CHUNK);if(!m.nodes||!buffer){fprintf(stderr,"Metadata allocation failed\n");CloseHandle(volume);free(buffer);mft_free(&m);return 1;}
 double setup_ms=ms(ticks()-phase);phase=ticks();uint64_t deadline=GetTickCount64()+180000,scanned=0,blank=0;int fatal=0;
 for(uint32_t r=0;r<run_count&&scanned<bytes;r++){uint64_t take=runs[r].clusters*geometry.BytesPerCluster;if(take>bytes-scanned)take=bytes-scanned;uint64_t offset=runs[r].lcn*geometry.BytesPerCluster;
  while(take){DWORD n=(DWORD)(take>READ_CHUNK?READ_CHUNK:take);uint64_t began=ticks();if(!read_at(volume,offset,buffer,n)){fprintf(stderr,"MFT read failed at %" PRIu64 ": %lu\n",offset,GetLastError());fatal=1;break;}read_ticks+=ticks()-began;io_bytes+=n;
   for(uint32_t at=0;at<n;at+=geometry.BytesPerFileRecordSegment){uint8_t *record=buffer+at;uint32_t id=(uint32_t)((scanned+at)/geometry.BytesPerFileRecordSegment);
    if(memcmp(record,"FILE",4)){blank++;continue;}if(!mft_fixup(record,geometry.BytesPerFileRecordSegment,geometry.BytesPerSector)){if(mft_u16(record+22)&1)m.bad_fixup++;continue;}
    if(!mft_record(&m,record,geometry.BytesPerFileRecordSegment,id)){m.bad_attributes++;uint64_t target=mft_u64(record+32);target=target?target&MFT_INDEX_MASK:id;if(target<m.slots)m.nodes[target].flags|=MF_BAD;}
   }
   take-=n;offset+=n;scanned+=n;if(GetTickCount64()>deadline){fprintf(stderr,"Bounded MFT read deadline exceeded\n");fatal=1;break;}
  }if(fatal)break;
 }
 CloseHandle(volume);free(buffer);if(fatal){mft_free(&m);return 1;}mft_resolve(&m);double scan_ms=ms(ticks()-phase);phase=ticks();
 uint64_t *sum_logical=calloc(m.slots,sizeof *sum_logical),*sum_allocated=calloc(m.slots,sizeof *sum_allocated);uint16_t *depth=calloc(m.slots,sizeof *depth);uint8_t *attributed=calloc(m.slots,1);directory *dirs=malloc((size_t)m.slots*sizeof *dirs);
 if(!sum_logical||!sum_allocated||!depth||!attributed||!dirs){fprintf(stderr,"Aggregation allocation failed\n");mft_free(&m);free(sum_logical);free(sum_allocated);free(depth);free(attributed);free(dirs);return 1;}
 depth[5]=1;uint64_t files=0,folders=0,logical_unique=0,allocated_unique=0,all_logical=0,all_allocated=0,hardlinks=0,resident=0,orphans=0,unknown=0,missing_default=0;uint32_t dir_count=0;
 for(uint32_t id=0;id<m.slots;id++){mft_node *node=&m.nodes[id];if(!(node->flags&MF_ACTIVE))continue;if(node->flags&MF_DIRECTORY){folders++;uint32_t d=directory_depth(&m,id,depth);if(d)dirs[dir_count++]=(directory){id,d};else orphans++;}else{files++;logical_unique+=node->logical;allocated_unique+=node->allocated;all_logical+=node->all_logical;all_allocated+=node->all_allocated;hardlinks+=node->first!=MFT_NONE&&m.names[node->first].next!=MFT_NONE;resident+=(node->flags&MF_DATA)&&!node->allocated;missing_default+=!(node->flags&MF_DATA);unknown+=!!(node->flags&MF_BAD);}}
 uint64_t entries=0;for(uint32_t i=0;i<m.name_count;i++){mft_name *name=&m.names[i];if(!name->valid)continue;uint32_t child=(uint32_t)(name->child&MFT_INDEX_MASK),parent=(uint32_t)(name->parent&MFT_INDEX_MASK);mft_node *node=&m.nodes[child];if(child==5)continue;if(!depth[parent]){orphans++;continue;}if(node->flags&MF_DIRECTORY)continue;entries++;sum_logical[parent]+=node->logical;if(!attributed[child]){sum_allocated[parent]+=node->allocated;attributed[child]=1;}}
 qsort(dirs,dir_count,sizeof *dirs,deepest);for(uint32_t i=0;i<dir_count;i++){uint32_t id=dirs[i].id;if(id==5)continue;uint32_t p=parent_id(&m,id);if(p<m.slots&&depth[p]){sum_logical[p]+=sum_logical[id];sum_allocated[p]+=sum_allocated[id];}}
 double aggregate_ms=ms(ticks()-phase);double ready_ms=ms(ticks()-started);phase=ticks();uint64_t exported=0,export_errors=0;
 if(csv){FILE *out=_wfopen(csv,L"wb");wchar_t *path=malloc(32768*sizeof *path);if(!out||!path){fprintf(stderr,"CSV output could not be opened\n");if(out)fclose(out);free(path);fatal=1;}
  else{setvbuf(out,NULL,_IOFBF,1024*1024);fprintf(out,"\xef\xbb\xbf\"File Name\",\"Size\",\"Allocated\",\"MFT Record\",\"Parent MFT Record\",\"Directory\",\"Sequence\",\"All Stream Size\",\"All Stream Allocated\",\"Attributes\",\"Data Known\"\r\n");
   for(uint32_t i=0;i<m.name_count;i++){mft_name *name=&m.names[i];if(!name->valid)continue;uint32_t id=(uint32_t)(name->child&MFT_INDEX_MASK),parent=(uint32_t)(name->parent&MFT_INDEX_MASK);mft_node *node=&m.nodes[id];if(id==5||!depth[parent])continue;
    if(!path_for(&m,name,argv[1][0],path,32768)||!csv_string(out,path)){export_errors++;continue;}
    int dir=!!(node->flags&MF_DIRECTORY);fprintf(out,",%" PRIu64 ",%" PRIu64 ",%u,%u,%d,%u,%" PRIu64 ",%" PRIu64 ",%u,%d\r\n",dir?sum_logical[id]:node->logical,dir?sum_allocated[id]:node->allocated,id,parent,dir,(unsigned)(name->child>>48),node->all_logical,node->all_allocated,node->attributes,!(node->flags&MF_BAD));exported++;
   }
   if(ferror(out))fatal=1;if(fclose(out))fatal=1;free(path);
  }
 }
 double export_ms=ms(ticks()-phase),wall_ms=ms(ticks()-started);PROCESS_MEMORY_COUNTERS memory={.cb=sizeof memory};GetProcessMemoryInfo(GetCurrentProcess(),&memory,sizeof memory);
 char report[8192];snprintf(report,sizeof report,"{\"schema\":\"openpilot-mft-prototype/v1\",\"volume\":\"%c:\",\"live_volume\":true,\"snapshot\":false,\"sector_bytes\":%lu,\"cluster_bytes\":%lu,\"record_bytes\":%lu,\"mft_bytes\":%" PRIu64 ",\"extent_count\":%u,\"slots\":%u,\"records\":%" PRIu64 ",\"active_base_records\":%" PRIu64 ",\"extension_records\":%" PRIu64 ",\"blank_slots\":%" PRIu64 ",\"files_unique\":%" PRIu64 ",\"folders_unique\":%" PRIu64 ",\"file_name_entries\":%" PRIu64 ",\"hardlinked_records\":%" PRIu64 ",\"resident_or_empty_records\":%" PRIu64 ",\"logical_unique\":%" PRIu64 ",\"allocated_unique\":%" PRIu64 ",\"all_stream_logical_unique\":%" PRIu64 ",\"all_stream_allocated_unique\":%" PRIu64 ",\"root_logical_entries\":%" PRIu64 ",\"root_allocated_unique\":%" PRIu64 ",\"bad_fixups\":%" PRIu64 ",\"bad_attributes\":%" PRIu64 ",\"stale_references\":%" PRIu64 ",\"orphan_entries\":%" PRIu64 ",\"unknown_data_records\":%" PRIu64 ",\"missing_default_stream_records\":%" PRIu64 ",\"attribute_list_records\":%" PRIu64 ",\"dos_aliases_skipped\":%" PRIu64 ",\"setup_ms\":%.3f,\"read_and_parse_ms\":%.3f,\"raw_read_ms\":%.3f,\"aggregation_ms\":%.3f,\"ready_ms\":%.3f,\"export_ms\":%.3f,\"wall_ms\":%.3f,\"io_bytes\":%" PRIu64 ",\"exported_rows\":%" PRIu64 ",\"export_errors\":%" PRIu64 ",\"peak_working_set_bytes\":%zu,\"partial\":%s}\n",(char)argv[1][0],geometry.BytesPerSector,geometry.BytesPerCluster,geometry.BytesPerFileRecordSegment,bytes,run_count,m.slots,m.records,m.active,m.extensions,blank,files,folders,entries,hardlinks,resident,logical_unique,allocated_unique,all_logical,all_allocated,sum_logical[5],sum_allocated[5],m.bad_fixup,m.bad_attributes,m.bad_sequence,orphans,unknown,missing_default,m.attribute_lists,m.dos_names,setup_ms,scan_ms,ms(read_ticks),aggregate_ms,ready_ms,export_ms,wall_ms,io_bytes,exported,export_errors,(size_t)memory.PeakWorkingSetSize,(m.bad_fixup||m.bad_attributes||m.bad_sequence||orphans||unknown||export_errors)?"true":"false");
 fputs(report,stdout);if(json){FILE *out=_wfopen(json,L"wb");if(!out)fatal=1;else{if(fputs(report,out)<0)fatal=1;if(fclose(out))fatal=1;}}
 mft_free(&m);free(sum_logical);free(sum_allocated);free(depth);free(attributed);free(dirs);return fatal?1:0;
}
