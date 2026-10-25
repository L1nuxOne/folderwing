/* Private, local, fixed-size durability protocol. No paths or mutation verbs. */
#ifndef FP_DURABLEWIRE_H
#define FP_DURABLEWIRE_H
#include <stdint.h>
#define FPD_MAGIC 0x4450464fu
#define FPD_VERSION 1u
#define FPD_LIMIT 256u
#define FPD_TIMEOUT 30000u
#define FPD_PIPE_PREFIX L"\\\\.\\pipe\\OpenPilot-durability-"
typedef struct fpd_message {
    uint32_t magic,version,sequence,error;
    uint64_t handle;
    uint32_t volume,high,low,reserved;
} fpd_message;
_Static_assert(sizeof(fpd_message)==40,"durability frame layout");
#endif
