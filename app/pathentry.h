#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#define PATHENTRY_MAX 4096
#define PATHENTRY_BYTES 16384
/* 0 Windows syntax, 1 Linux absolute/home syntax, -1 unsupported ~user syntax. */
int pathentry_kind(const wchar_t *text);
int pathentry_display(const wchar_t *unc,wchar_t *distro,size_t dc,wchar_t *linux,size_t lc);
int pathentry_bytes(const wchar_t *text,char *out,size_t cap,uint32_t *len);
wchar_t *pathentry_unc(const char *distro,uint32_t dn,const char *linux,uint32_t pn);
/* Worker-only, synchronous; success returns a malloc-owned canonical UNC path. */
DWORD pathentry_resolve(const wchar_t *base,const wchar_t *typed,int root,HANDLE cancel,wchar_t **out);
typedef struct pathentry_worker pathentry_worker;
typedef struct pathentry_result {uint64_t sequence;DWORD error;wchar_t *path;wchar_t *typed;} pathentry_result;
pathentry_worker *pathentry_start(HWND target,UINT message);
int pathentry_submit(pathentry_worker *worker,const wchar_t *base,const wchar_t *typed,int root,uint64_t sequence);
void pathentry_cancel(pathentry_worker *worker);
/* Nonblocking; no later result posts after stop. The thread releases its state. */
void pathentry_stop(pathentry_worker *worker);
void pathentry_result_free(pathentry_result *result);
