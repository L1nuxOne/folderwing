#pragma once
#include "gitmodel.h"
#define PANELP_GIT_OPEN 64u
#define PANELP_DIFF_GIT_OPEN 65u
#define PANELP_GIT_STATE 167u
#define PANELP_GIT_ACTION 168u
/* STATE: root UTF-16 (terminated), then exact porcelain-v2 bytes. */
enum {GIT_LOADING=1,GIT_READY,GIT_FAILED,GIT_STOPPED};
typedef struct git_open {uint64_t session;uint32_t wsl,as_root;} git_open;
typedef struct git_state {uint64_t session;uint32_t generation,state,error,native,root_chars,bytes;} git_state;
enum {GIT_DIFF=0,GIT_PREVIEW,GIT_LEFT,GIT_RIGHT,GIT_COPY,GIT_TERMINAL,GIT_MENU};
typedef struct git_action {uint64_t session;uint32_t generation,action,staged,reserved;} git_action;
typedef struct git_diff_open {uint64_t session;uint32_t wsl,as_root,staged,root_chars,path_chars,reserved;git_meta meta;} git_diff_open;

/* Host policy update: uint32_t WSL enabled (0/1); future work only. */
#define PANELP_GIT_WSL_POLICY 66u
