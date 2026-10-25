#include "imageutil.h"
#include <wctype.h>
int image_extension(const wchar_t *name)
{
    static const wchar_t *const exts[] = {L"svg", L"jpg", L"jpeg", L"jpe", L"png",  L"gif",  L"bmp",  L"dib",
                                          L"tif", L"tiff", L"ico", L"webp", L"heic", L"heif", L"avif",
                                          L"jxr", L"wdp",  L"dng", L"cr2",  L"cr3",  L"nef",  L"arw",
                                          L"orf", L"rw2",  L"raf", L"pef",  L"srw",  L"dds"};
    const wchar_t *e = name ? wcsrchr(name, L'.') : NULL;
    if (!e || !e[1])
        return 0;
    for (size_t i = 0; i < sizeof exts / sizeof *exts; ++i) {
        const wchar_t *a = e + 1, *b = exts[i];
        while (*a && *b && towlower(*a) == *b)
            ++a, ++b;
        if (!*a && !*b)
            return 1;
    }
    return 0;
}
double image_fit(uint32_t w, uint32_t h, int vw, int vh, int width)
{
    if (!w || !h || vw <= 0 || vh <= 0)
        return 1;
    double s = (double)vw / w, t = (double)vh / h;
    if (!width && t < s)
        s = t;
    return width ? s : s < 1 ? s : 1; /* initial whole-image fit never enlarges small images */
}
uint32_t image_orientation(uint32_t exif)
{
    static const uint32_t flags[] = {0, 0, 8, 2, 16, 1 | 8, 1, 1 | 16, 3};
    return exif <= 8 ? flags[exif] : 0;
}
void image_premultiply(uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; ++i, p += 4) {
        unsigned a = p[3];
        p[0] = (uint8_t)((p[0] * a + 127) / 255);
        p[1] = (uint8_t)((p[1] * a + 127) / 255);
        p[2] = (uint8_t)((p[2] * a + 127) / 255);
    }
}
