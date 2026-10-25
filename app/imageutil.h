#pragma once
#include <stdint.h>
#include <wchar.h>
int image_extension(const wchar_t *name);
double image_fit(uint32_t w, uint32_t h, int vw, int vh, int width);
/* EXIF 1..8 -> WIC rotation/flip flags (values from WICBitmapTransformOptions). */
uint32_t image_orientation(uint32_t exif);
void image_premultiply(uint8_t *pixels, size_t count);
