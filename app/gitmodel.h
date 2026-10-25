#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#define GIT_ENTRIES 5000u
#define GIT_NAMES (1024u*1024u)
typedef struct git_meta {uint8_t kind,x,y,pad;uint32_t mode[3];char head[65],index[65];} git_meta;
typedef struct git_entry {wchar_t *path,*original;git_meta meta;} git_entry;
typedef struct git_model {wchar_t *root;git_entry *entries;uint32_t count,capacity,native,error;size_t names;wchar_t branch[256],upstream[256];char oid[65];int ahead,behind,tracking;} git_model;
void git_model_free(git_model *m);
DWORD git_parse_status(git_model *m,const wchar_t *root,const uint8_t *raw,size_t n,int native);
int git_path_safe(const wchar_t *path);
int git_oid_valid(const char *oid);
const git_entry *git_find(const git_model *m,const wchar_t *relative);
int git_badge(const git_model *m,const wchar_t *folder,const wchar_t *name,int directory,wchar_t out[4]);
