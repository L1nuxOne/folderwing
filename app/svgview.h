#pragma once
#include <windows.h>
#include "imageproto.h"
#ifdef __cplusplus
extern "C" {
#endif
HRESULT svg_open(const uint8_t *data, uint32_t bytes, imgp_info *info);
HRESULT svg_render(const imgp_render *r, uint8_t *pixels);
void svg_close(void);
#ifdef __cplusplus
}
#endif
