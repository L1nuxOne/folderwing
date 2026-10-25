#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "mediaproto.h"
#ifdef __cplusplus
extern "C" {
#endif
int media_extension(const wchar_t *name);
int media_audio_extension(const wchar_t *name);
int media_fallback_error(uint32_t error);
int media_open_valid(const mediap_head *h,const void *payload);
int media_frame_valid(const mediap_frame *f);
/* Only local drive/UNC filenames; extended drive paths allowed; no URL, raw device, ADS or relative path. */
int media_path_valid(const wchar_t *path);

#ifdef __cplusplus
}
#endif
