#pragma once
#include <stdint.h>
#include <wchar.h>
#define PANELP_SPACE_OPEN 60u
#define PANELP_SPACE_BROWSER 61u /* space_open configuration, without a path */
#define PANELP_SPACE_VOLUME 162u /* space_action (action/flags zero) plus catalog root */
#define PANELP_SPACE_ACTION 160u
#define PANELP_SPACE_STATE 161u
#define SPACE_LEFT 0u
#define SPACE_RIGHT 1u
#define SPACE_PREVIEW 2u
#define SPACE_COPY 3u
#define SPACE_ANALYZE 4u
#define SPACE_MENU 5u
/* OPEN: header plus terminated UTF-16 root. ACTION: header plus terminated UTF-16
 * full result path. session is a host-issued scan-root authority, not a job ID. */
typedef struct space_open {uint64_t session;uint32_t wsl,as_root;} space_open;
typedef struct space_action {uint64_t session;uint32_t action,flags;} space_action;
typedef struct space_state {uint64_t session;uint32_t state,error;uint64_t files,folders;} space_state;
static inline int space_target_valid(const wchar_t *root,const wchar_t *path){
 if(!root||!*root||!path||!*path)return 0;size_t n=wcslen(root),p=wcslen(path);if(p<n||wcsncmp(root,path,n)||(p>n&&root[n-1]!=L'\\'&&path[n]!=L'\\'))return 0;
 for(const wchar_t *at=path+n;*at;){if(*at==L'\\'){at++;continue;}const wchar_t *end=wcschr(at,L'\\');size_t len=end?(size_t)(end-at):wcslen(at);if((len==1&&at[0]==L'.')||(len==2&&at[0]==L'.'&&at[1]==L'.'))return 0;for(size_t i=0;i<len;i++)if(at[i]==L':'||at[i]==L'/')return 0;at+=len;}
 return 1;
}
