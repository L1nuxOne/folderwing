#pragma once
#include <stdint.h>
#define MEDIAP_PATH_MAX 32768u
#define MEDIAP_MAX_W 1280u
#define MEDIAP_MAX_H 720u
#define MEDIAP_BANK_BYTES (MEDIAP_MAX_W*MEDIAP_MAX_H*4u)
#define MEDIAP_SECTION_BYTES (2u*MEDIAP_BANK_BYTES)
#define MEDIAP_MAX_PAYLOAD (MEDIAP_PATH_MAX*2u+64u)
enum { MEDIAP_OPEN=1,MEDIAP_CONTROL,MEDIAP_ACK,MEDIAP_CLOSE,MEDIAP_STATE=101,MEDIAP_FRAME };
enum { MEDIAP_LOADING=1,MEDIAP_READY,MEDIAP_FAILED };
enum { MEDIAP_AUDIO=1u,MEDIAP_THUMB=2u,MEDIAP_MUTED=4u };
enum { MEDIA_PLAY=1,MEDIA_PAUSE,MEDIA_SEEK,MEDIA_VOLUME,MEDIA_MUTE };
typedef struct mediap_head {uint32_t type,size,doc,ticket;} mediap_head;
/* OPEN: this header followed by one exact NUL-terminated UTF-16 absolute path.
 * One source per disposable process; replacement kills the old job. */
typedef struct mediap_open {uint32_t width,height,flags,reserved;} mediap_open;
typedef struct mediap_control {uint32_t action,reserved;double value;} mediap_control;
typedef struct mediap_state {
 uint32_t state,backend,error,fallback_error; /* backend: 1 MF, 2 libmpv */
 uint32_t has_video,has_audio,width,height,paused,muted,ended,audio_unavailable;
 double duration,position,volume;
} mediap_state;
/* Exactly one frame is outstanding. Parent copies its bank before ACK(ticket).
 * The worker never rewrites that bank while the parent can be reading it. */
typedef struct mediap_frame {uint32_t bank,width,height,reserved;} mediap_frame;
