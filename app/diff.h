/* Read-only file comparison. Work and allocations are bounded; no prefix is
 * ever reported as an equal whole file. All result ownership is explicit. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "textutil.h"
#define DIFF_TEXT_BYTES (2u*1024u*1024u)
#define DIFF_LINES 100000u
#define DIFF_NO_LINE UINT32_MAX
enum { DIFF_SAME, DIFF_CHANGED, DIFF_REMOVED, DIFF_ADDED };
enum { DIFF_TEXT, DIFF_BINARY, DIFF_LARGE };
typedef struct diff_line {uint32_t start,length,newline;uint64_t hash;} diff_line;
typedef struct diff_doc {text_data data;diff_line *lines;uint32_t count;uint64_t bytes;} diff_doc;
typedef struct diff_row {uint32_t left,right,kind,hunk;} diff_row;
typedef struct diff_result {
 diff_doc doc[2];diff_row *rows;uint32_t count,hunks,mode,coarse;
 int bytes_equal;DWORD error;uint32_t error_side;
} diff_result;
void diff_free(diff_result *r);
/* Decodes complete buffers, aligns normalized lines, and keeps byte equality
 * separate. Returns Windows error code; caller releases even a failed result. */
DWORD diff_buffers(const uint8_t *a,size_t na,const uint8_t *b,size_t nb,HANDLE cancel,diff_result *r);
DWORD diff_files(const wchar_t *left,const wchar_t *right,int wsl,HANDLE cancel,diff_result *r);
int diff_change(const diff_result *r,int row,int direction); /* next/previous hunk, wraps */
