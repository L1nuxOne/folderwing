#pragma once
#include <stdint.h>
#include <wchar.h>
#include "archiveproto.h"
typedef struct arc_entry {arcp_entry info;wchar_t *name;} arc_entry;
typedef struct arc_row {uint32_t index,flags;uint64_t size;wchar_t *name,*path;} arc_row;
int arc_name_safe(const wchar_t *name);
int arc_leaf_safe(const wchar_t *name);
int arc_extension(const wchar_t *name);
void arc_entries_free(arc_entry *entries,uint32_t count);
void arc_rows_free(arc_row *rows,uint32_t count);
int arc_rows_build(const arc_entry *entries,uint32_t count,const wchar_t *prefix,arc_row **out,uint32_t *n);
