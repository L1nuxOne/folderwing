#pragma once
#include "duplicateread.h"
#define DUP_ROOTS 16u
#define DUP_FILES 100000u
#define DUP_PATH_BYTES (64u*1024u*1024u)
enum { DUP_ENUMERATE=1,DUP_HASHING,DUP_VERIFYING,DUP_COMPLETE,DUP_PARTIAL,DUP_STOPPED };
typedef struct duplicate_file {wchar_t *relative;du_stamp scan,hashed;uint8_t hash[32];uint32_t root,group,candidate,valid;} duplicate_file;
typedef struct duplicate_result {duplicate_file *files;uint32_t count,capacity,groups,state,error,entries;uint64_t members,duplicate_bytes,hashed,compared,skipped,errors,aliases,path_bytes;} duplicate_result;
typedef struct duplicate_progress {uint32_t phase,files,groups;uint64_t hashed,compared,errors;} duplicate_progress;
typedef void (*duplicate_notify)(void *,const duplicate_progress *);
void duplicates_free(duplicate_result *r);
void duplicates_scan(const wchar_t *const *roots,uint32_t count,int wsl,int as_root,HANDLE cancel,duplicate_notify notify,void *user,duplicate_result *out);
/* Exact compare checks the stamps captured by hashing. 1 equal, 0 different,
 * -1 incomplete; errors never become equality. */
int duplicates_equal(du_reader *a,du_reader *b,const duplicate_file *left,const duplicate_file *right,HANDLE cancel,DWORD *error);
