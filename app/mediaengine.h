#pragma once
#include <wchar.h>
#include "mediaproto.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*media_frame_callback)(const void *pixels,uint32_t width,uint32_t height,uint32_t stride,void *user);
int32_t media_engine_open(const wchar_t *path,int mpv,uint32_t width,uint32_t height,uint32_t flags,media_frame_callback frame,void *user);
void media_engine_poll(mediap_state *state);
void media_engine_control(uint32_t action,double value);
void media_engine_close(void);
#ifdef __cplusplus
}
#endif
