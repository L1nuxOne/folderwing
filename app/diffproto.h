#pragma once
#include <stdint.h>
#define PANELP_DIFF_OPEN 62u
#define PANELP_DIFF_STATE 164u
/* Fixed header, then two individually NUL-terminated exact UTF-16 paths.
 * Counts include terminators. A session always freezes a left/right pair. */
typedef struct diff_open {uint64_t session;uint32_t wsl,left_chars,right_chars,reserved;} diff_open;
typedef struct diff_state {uint64_t session;uint32_t state,hunks,mode,error;int32_t bytes_equal;uint32_t coarse;} diff_state;
enum { DIFF_LOADING=1,DIFF_READY,DIFF_STOPPED,DIFF_FAILED };

/* Host policy update: uint32_t WSL enabled (0/1); future work only. */
#define PANELP_DIFF_WSL_POLICY 66u
