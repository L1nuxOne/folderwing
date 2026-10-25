#pragma once
#include "gitmodel.h"
#include "gitproto.h"
#include "diff.h"
typedef struct git_result {git_model model;uint8_t *raw;uint32_t bytes;DWORD error,code;} git_result;
void git_result_free(git_result *r);
DWORD git_status_read(const wchar_t *folder,int wsl,int as_root,HANDLE cancel,git_result *result);
DWORD git_diff_read(const wchar_t *root,const wchar_t *relative,const git_diff_open *request,HANDLE cancel,diff_result *result);
