#pragma once
#include "contentmatch.h"
#define PANELP_CONTENT_OPEN 61u
#define PANELP_CONTENT_ACTION 162u
#define PANELP_CONTENT_STATE 163u
#define CONTENT_PREVIEW 0u
#define CONTENT_LEFT 1u
#define CONTENT_RIGHT 2u
#define CONTENT_COPY 3u
#define CONTENT_MENU 4u
/* OPEN + NUL-terminated UTF-16 root. ACTION + NUL-terminated UTF-16 relative path.
 * Host session binds the root; search_generation changes for each Start/Stop/edit. */
typedef struct content_open {uint64_t session;uint32_t wsl,as_root;} content_open;
typedef struct content_action {uint64_t session;uint32_t action,search_generation;uint64_t bytes,mtime;cf_hit hit;} content_action;
typedef struct content_state {uint64_t session;uint32_t generation,state;uint64_t matches;} content_state;

/* Host policy update: uint32_t WSL enabled (0/1); future work only. */
#define PANELP_CONTENT_WSL_POLICY 66u
