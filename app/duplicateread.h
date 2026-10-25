#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "duplicatewire.h"
typedef struct du_reader {HANDLE root,file,process,job,to,from,cancel;int native;wchar_t *relative;du_stamp stamp;} du_reader;
DWORD duplicate_reader_init(du_reader *r,const wchar_t *root,int wsl,int as_root,HANDLE cancel);
void duplicate_reader_free(du_reader *r);
DWORD duplicate_reader_open(du_reader *r,const wchar_t *relative,const du_stamp *expected);
DWORD duplicate_reader_read(du_reader *r,void *buffer,DWORD *count);
DWORD duplicate_reader_hash(du_reader *r,uint8_t hash[32]);
