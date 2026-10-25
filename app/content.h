#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "contentmatch.h"
typedef struct cf_result {wchar_t *path;uint64_t bytes,mtime;cf_hit hit;} cf_result;
typedef struct cf_model {cf_result *rows;uint32_t count,capacity;uint64_t names;cf_summary summary;int limited;} cf_model;
void content_model_free(cf_model *model);
int content_model_add(cf_model *model,const cf_record *record,const wchar_t *relative);
wchar_t *content_result_path(const wchar_t *root,const wchar_t *relative);
typedef int (*content_emit)(void *,const cf_record *,const wchar_t *relative);
DWORD content_scan(const wchar_t *root,const cf_options *options,int wsl,int as_root,HANDLE cancel,content_emit emit,void *user);
