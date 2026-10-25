/* WIC + libwebp in a restricted, disposable process. Decode on one thread;
 * a bounded input queue replaces obsolete opens while that thread is busy. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>
#include <propvarutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "src/webp/decode.h"
#include "src/webp/demux.h"
#include "imageproto.h"
#include "imageutil.h"
#include "svgview.h"
#define DROP(p)                                                                                              \
    do {                                                                                                     \
        if (p) {                                                                                             \
            IUnknown_Release((IUnknown *)(p));                                                               \
            (p) = NULL;                                                                                      \
        }                                                                                                    \
    } while (0)
typedef struct msg {
    struct msg *next;
    imgp_head h;
    uint8_t data[];
} msg;
static struct {
    HANDLE in, out, wake;
    uint8_t *pixels;
    CRITICAL_SECTION lock;
    msg *first, *last;
    volatile LONG latest, eof;
    unsigned count;
} Q;
static struct {
    IWICImagingFactory *factory;
    IWICBitmapDecoder *decoder;
    IWICBitmapFrameDecode *frame;
    IWICStream *stream;
    IWICBitmapSource *source;
    IWICColorContext *icc;
    uint8_t *data;
    uint32_t pixel_w, pixel_h;
    uint32_t id, data_bytes, cache_mb, frame_index, delay_ms, disposal, bg;
    WICRect previous;
    uint8_t *canvas, *restore, *fragment;
    WebPAnimDecoder *webp;
    int web_stamp, svg;
    imgp_info info;
} D;
static int read_all(void *p, DWORD n)
{
    while (n) {
        DWORD g;
        if (!ReadFile(Q.in, p, n, &g, NULL) || !g)
            return -1;
        p = (uint8_t *)p + g;
        n -= g;
    }
    return 0;
}
static int write_all(const void *p, DWORD n)
{
    while (n) {
        DWORD g;
        if (!WriteFile(Q.out, p, n, &g, NULL) || !g)
            return -1;
        p = (const uint8_t *)p + g;
        n -= g;
    }
    return 0;
}
static void reply(uint32_t type, uint32_t doc, uint32_t ticket, const void *p, uint32_t n)
{
    imgp_head h = {type, n, doc, ticket};
    if (write_all(&h, sizeof h) || write_all(p, n))
        InterlockedExchange(&Q.eof, 1);
}
static DWORD WINAPI reader(void *arg)
{
    for (;;) {
        imgp_head h;
        if (read_all(&h, sizeof h) || h.size > sizeof(imgp_open) + IMGP_PATH_MAX * 2u)
            break;
        if ((h.type == IMGP_OPEN && (h.size < sizeof(imgp_open) + 2 || (h.size - sizeof(imgp_open)) % 2)) ||
            (h.type == IMGP_RENDER && h.size != sizeof(imgp_render)) ||
            (h.type == IMGP_PAGE && h.size != sizeof(imgp_page)) ||
            ((h.type == IMGP_CLOSE || h.type == IMGP_QUIT) && h.size) || h.type < IMGP_OPEN ||
            h.type > IMGP_PAGE)
            break;
        msg *m = malloc(sizeof *m + h.size);
        if (!m)
            break;
        m->next = NULL;
        m->h = h;
        if (read_all(m->data, h.size)) {
            free(m);
            break;
        }
        if (h.type == IMGP_OPEN &&
            ((wchar_t *)(m->data + sizeof(imgp_open)))[(h.size - sizeof(imgp_open)) / 2 - 1]) {
            free(m);
            break;
        }
        EnterCriticalSection(&Q.lock);
        if (h.type == IMGP_OPEN || h.type == IMGP_CLOSE) {
            InterlockedExchange(&Q.latest, (LONG)h.doc);
            /* Keep render replies, which release their shared-bank ownership. */
            msg **at = &Q.first;
            Q.last = NULL;
            while (*at) {
                msg *k = *at;
                if (k->h.type == IMGP_OPEN || k->h.type == IMGP_CLOSE) {
                    *at = k->next;
                    free(k);
                    --Q.count;
                } else {
                    Q.last = k;
                    at = &k->next;
                }
            }
        }
        if (Q.count >= 8) {
            LeaveCriticalSection(&Q.lock);
            free(m);
            break;
        }
        if (Q.last)
            Q.last->next = m;
        else
            Q.first = m;
        Q.last = m;
        ++Q.count;
        LeaveCriticalSection(&Q.lock);
        SetEvent(Q.wake);
    }
    InterlockedExchange(&Q.eof, 1);
    SetEvent(Q.wake);
    return 0;
}
static msg *pop(void)
{
    EnterCriticalSection(&Q.lock);
    msg *m = Q.first;
    if (m) {
        Q.first = m->next;
        if (!Q.first)
            Q.last = NULL;
        --Q.count;
    }
    LeaveCriticalSection(&Q.lock);
    return m;
}
static void close_image(void)
{
    svg_close(); D.svg=0;
    if(D.webp) WebPAnimDecoderDelete(D.webp);
    D.webp=NULL;
    free(D.canvas);free(D.restore);free(D.fragment);
    D.canvas=D.restore=D.fragment=NULL;
    D.frame_index=UINT32_MAX;D.delay_ms=D.disposal=D.bg=0;
    D.web_stamp=0;
    DROP(D.source);
    DROP(D.icc);
    DROP(D.frame);
    DROP(D.decoder);
    DROP(D.stream);
    free(D.data);
    D.data = NULL;
    memset(&D.info, 0, sizeof D.info);
}
static uint32_t number(IWICMetadataQueryReader *r, const wchar_t *key)
{
    PROPVARIANT v;
    PropVariantInit(&v);
    uint32_t n = 0;
    if (SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName(r, key, &v))) {
        if (v.vt == VT_BOOL)
            n=v.boolVal!=VARIANT_FALSE;
        else if (v.vt == VT_UI2)
            n = v.uiVal;
        else if (v.vt == VT_UI4)
            n = v.ulVal;
        else if (v.vt == VT_UI1)
            n = v.bVal;
    }
    PropVariantClear(&v);
    return n;
}
static void string_value(IWICMetadataQueryReader *r, const wchar_t *key, wchar_t *out, size_t cap)
{
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName(r, key, &v))) {
        if (v.vt == VT_LPWSTR && v.pwszVal) {
            wcsncpy(out, v.pwszVal, cap - 1);
            out[cap - 1] = 0;
        } else if (v.vt == VT_LPSTR && v.pszVal)
            MultiByteToWideChar(CP_UTF8, 0, v.pszVal, -1, out, (int)cap);
    }
    PropVariantClear(&v);
    out[cap - 1] = 0;
    for (size_t i = 0; out[i]; ++i)
        if (out[i] < 32)
            out[i] = L' ';
}
/* Minimal bounded TIFF IFD reader for the EXIF chunk in WebP. */
static unsigned u16(const uint8_t *p, int le) { return le ? p[0] | p[1] << 8 : p[0] << 8 | p[1]; }
static uint32_t u32(const uint8_t *p, int le)
{
    return le ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24
              : (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint32_t web_orientation(const uint8_t *p, size_t n)
{
    if (n >= 6 && !memcmp(p, "Exif\0\0", 6)) {
        p += 6;
        n -= 6;
    }
    if (n < 8)
        return 1;
    int le = p[0] == 'I' && p[1] == 'I';
    if (!le && !(p[0] == 'M' && p[1] == 'M'))
        return 1;
    if (u16(p + 2, le) != 42)
        return 1;
    uint32_t off = u32(p + 4, le);
    if (off > n - 2)
        return 1;
    unsigned count = u16(p + off, le);
    off += 2;
    for (unsigned i = 0; i < count && (uint64_t)off + 12 <= n; ++i, off += 12)
        if (u16(p + off, le) == 274 && u16(p + off + 2, le) == 3 && u32(p + off + 4, le) == 1) {
            unsigned o = u16(p + off + 8, le);
            return o >= 1 && o <= 8 ? o : 1;
        }
    return 1;
}
static uint32_t delay(uint32_t ms) { return !ms ? 100 : ms < 20 ? 20 : ms; }
static int current_doc(void) { return D.id==(uint32_t)InterlockedCompareExchange(&Q.latest,0,0) && !Q.eof; }
static HRESULT animation_budget(uint32_t w,uint32_t h) {
    return (uint64_t)w*h*16 + D.data_bytes <= (uint64_t)D.cache_mb*1024*1024 ? S_OK : WINCODEC_ERR_IMAGESIZEOUTOFRANGE;
}
static HRESULT source_pixels(const uint8_t *pixels) {
    IWICBitmap *bm=NULL;
    HRESULT hr=IWICImagingFactory_CreateBitmapFromMemory(D.factory,D.pixel_w,D.pixel_h,&GUID_WICPixelFormat32bppBGRA,D.pixel_w*4,D.pixel_w*D.pixel_h*4,(BYTE*)pixels,&bm);
    if(SUCCEEDED(hr)) { DROP(D.source); D.source=(IWICBitmapSource*)bm; }
    return hr;
}
/* NETSCAPE loop count is repetitions after the initial pass; WebP is total passes. */
static uint32_t gif_loops(void) {
    const uint8_t *p=D.data;size_t n=D.data_bytes,off=13;
    if(n<13) return 1;
    if(p[10]&128) off+=3u*(2u<<(p[10]&7));
    while(off<n) {
        unsigned tag=p[off++];
        if(tag==0x3b) break;
        if(tag==0x2c) {
            if(n-off<9) break;
            unsigned flags=p[off+8];off+=9;
            if(flags&128) off+=3u*(2u<<(flags&7));
            if(off>=n) break;
            ++off;
        } else if(tag==0x21) {
            if(off>=n) break;
            unsigned label=p[off++];
            if(label==0xff && n-off>=17 && p[off]==11 &&
               (!memcmp(p+off+1,"NETSCAPE2.0",11)||!memcmp(p+off+1,"ANIMEXTS1.0",11)) && p[off+12]==3 && p[off+13]==1) {
                unsigned loops=p[off+14]|p[off+15]<<8;
                return loops?loops+1:0;
            }
        } else break;
        while(off<n) { unsigned bytes=p[off++];if(!bytes)break;if(bytes>n-off)return 1;off+=bytes; }
    }
    return 1;
}
static HRESULT gif_frame(uint32_t wanted) {
    if(wanted>=D.info.frames) return E_INVALIDARG;
    size_t bytes=(size_t)D.info.w*D.info.h*4;
    if(D.frame_index==UINT32_MAX || wanted<D.frame_index) {
        for(size_t i=0;i<bytes;i+=4) memcpy(D.canvas+i,&D.bg,4);
        D.frame_index=UINT32_MAX;D.disposal=0;
    }
    uint32_t next=D.frame_index==UINT32_MAX?0:D.frame_index+1;
    for(uint32_t index=next;index<=wanted;++index) {
        if(!current_doc()) return E_ABORT;
        if(D.disposal==2) for(int y=0;y<D.previous.Height;++y) for(int x=0;x<D.previous.Width;++x)
            memcpy(D.canvas+((size_t)(D.previous.Y+y)*D.info.w+D.previous.X+x)*4,&D.bg,4);
        else if(D.disposal==3) memcpy(D.canvas,D.restore,bytes);
        IWICBitmapFrameDecode *frame=NULL;IWICMetadataQueryReader *meta=NULL;IWICFormatConverter *conv=NULL;
        HRESULT hr=IWICBitmapDecoder_GetFrame(D.decoder,index,&frame);
        UINT w=0,h=0,x=0,y=0,disposal=0,ms=100;
        if(SUCCEEDED(hr)) hr=IWICBitmapFrameDecode_GetSize(frame,&w,&h);
        if(SUCCEEDED(hr)) hr=IWICBitmapFrameDecode_GetMetadataQueryReader(frame,&meta);
        if(SUCCEEDED(hr)) {
            x=number(meta,L"/imgdesc/Left");y=number(meta,L"/imgdesc/Top");
            disposal=number(meta,L"/grctlext/Disposal");ms=delay(number(meta,L"/grctlext/Delay")*10);
        }
        if(SUCCEEDED(hr) && (!w||!h||x>D.info.w||y>D.info.h||w>D.info.w-x||h>D.info.h-y)) hr=WINCODEC_ERR_BADIMAGE;
        if(SUCCEEDED(hr)) hr=IWICImagingFactory_CreateFormatConverter(D.factory,&conv);
        if(SUCCEEDED(hr)) hr=IWICFormatConverter_Initialize(conv,(IWICBitmapSource*)frame,&GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,NULL,0,WICBitmapPaletteTypeCustom);
        if(SUCCEEDED(hr)) hr=IWICFormatConverter_CopyPixels(conv,NULL,w*4,w*h*4,D.fragment);
        if(SUCCEEDED(hr)) {
            if(disposal==3) memcpy(D.restore,D.canvas,bytes);
            for(UINT row=0;row<h;++row) for(UINT col=0;col<w;++col) {
                const uint8_t *p=D.fragment+((size_t)row*w+col)*4;
                if(p[3]) memcpy(D.canvas+((size_t)(y+row)*D.info.w+x+col)*4,p,4);
            }
            D.disposal=disposal;D.previous=(WICRect){(INT)x,(INT)y,(INT)w,(INT)h};D.delay_ms=ms;D.frame_index=index;
        }
        DROP(conv);DROP(meta);DROP(frame);
        if(FAILED(hr)) return hr;
    }
    return source_pixels(D.canvas);
}
static HRESULT gif_open(void) {
    IWICMetadataQueryReader *meta=NULL;
    HRESULT hr=IWICBitmapDecoder_GetMetadataQueryReader(D.decoder,&meta);
    if(FAILED(hr)) return hr;
    uint32_t w=number(meta,L"/logscrdesc/Width"),h=number(meta,L"/logscrdesc/Height");
    unsigned bg=number(meta,L"/logscrdesc/BackgroundColorIndex");DROP(meta);
    if(!w||!h||w>IMGP_MAX_DIM||h>IMGP_MAX_DIM||(uint64_t)w*h>IMGP_MAX_PIXELS) return WINCODEC_ERR_IMAGESIZEOUTOFRANGE;
    hr=animation_budget(w,h);if(FAILED(hr))return hr;
    D.pixel_w=w;D.pixel_h=h;D.info.orientation=1;
    D.info.w=w;D.info.h=h;D.info.animated=D.info.frames>1;D.info.loops=gif_loops();
    IWICPalette *palette=NULL;
    if(SUCCEEDED(IWICImagingFactory_CreatePalette(D.factory,&palette)) && SUCCEEDED(IWICBitmapDecoder_CopyPalette(D.decoder,palette))) {
        WICColor colors[256];UINT got=0;
        if(SUCCEEDED(IWICPalette_GetColors(palette,256,colors,&got)) && bg<got) D.bg=colors[bg];
    }
    DROP(palette);
    if(SUCCEEDED(IWICBitmapFrameDecode_GetMetadataQueryReader(D.frame,&meta))) {
        if(number(meta,L"/grctlext/TransparencyFlag")) D.bg=0;
        DROP(meta);
    }
    size_t bytes=(size_t)w*h*4;
    D.canvas=malloc(bytes);D.restore=malloc(bytes);D.fragment=malloc(bytes);
    if(!D.canvas||!D.restore||!D.fragment) return E_OUTOFMEMORY;
    return gif_frame(0);
}
static HRESULT select_frame(uint32_t frame) {
    if(!D.info.animated) return frame==(D.info.pages?D.info.page:0)?S_OK:E_INVALIDARG;
    if(frame>=D.info.frames) return E_INVALIDARG;
    if(frame==D.frame_index) return S_OK;
    if(!D.webp) return gif_frame(frame);
    if(D.frame_index==UINT32_MAX || frame<D.frame_index) {
        WebPAnimDecoderReset(D.webp);D.frame_index=UINT32_MAX;D.web_stamp=0;
    }
    uint8_t *pixels=NULL;
    while(D.frame_index==UINT32_MAX || D.frame_index<frame) {
        if(!current_doc()) return E_ABORT;
        int stamp;
        if(!WebPAnimDecoderGetNext(D.webp,&pixels,&stamp)) return WINCODEC_ERR_BADIMAGE;
        D.delay_ms=delay(stamp>D.web_stamp?(uint32_t)(stamp-D.web_stamp):0);D.web_stamp=stamp;
        D.frame_index=D.frame_index==UINT32_MAX?0:D.frame_index+1;
    }
    return source_pixels(pixels);
}
static HRESULT open_webp(size_t n)
{
    WebPData wd = {D.data, n};
    WebPDemuxer *dm = WebPDemux(&wd);
    if (!dm)
        return WINCODEC_ERR_BADIMAGE;
    unsigned w = WebPDemuxGetI(dm, WEBP_FF_CANVAS_WIDTH), h = WebPDemuxGetI(dm, WEBP_FF_CANVAS_HEIGHT);
    HRESULT hr = E_FAIL;
    WebPChunkIterator chunk;
    WebPAnimDecoder *anim = NULL;
    if (!w || !h || w > IMGP_MAX_DIM || h > IMGP_MAX_DIM || (uint64_t)w * h > IMGP_MAX_PIXELS) {
        hr = WINCODEC_ERR_IMAGESIZEOUTOFRANGE;
        goto out;
    }
    D.pixel_w=D.info.w = w;
    D.pixel_h=D.info.h = h;
    D.info.frames = WebPDemuxGetI(dm, WEBP_FF_FRAME_COUNT);
    if(!D.info.frames || D.info.frames>10000) { hr=WINCODEC_ERR_IMAGESIZEOUTOFRANGE;goto out; }
    D.info.animated=D.info.frames>1;
    if(D.info.animated && FAILED(hr=animation_budget(w,h))) goto out;
    D.info.loops=WebPDemuxGetI(dm, WEBP_FF_LOOP_COUNT);
    D.info.orientation = 1;
    wcscpy(D.info.format, L"WebP");
    if (WebPDemuxGetChunk(dm, "EXIF", 1, &chunk)) {
        D.info.orientation = web_orientation(chunk.chunk.bytes, chunk.chunk.size);
        WebPDemuxReleaseChunkIterator(&chunk);
    }
    if (WebPDemuxGetChunk(dm, "ICCP", 1, &chunk)) {
        if (SUCCEEDED(IWICImagingFactory_CreateColorContext(D.factory, &D.icc)) &&
            chunk.chunk.size <= UINT32_MAX &&
            SUCCEEDED(
                IWICColorContext_InitializeFromMemory(D.icc, chunk.chunk.bytes, (UINT)chunk.chunk.size)))
            D.info.color = 1;
        else {
            DROP(D.icc);
            D.info.color = 2;
        }
        WebPDemuxReleaseChunkIterator(&chunk);
    }
    /* Decode the initial composited canvas and retain only the streaming animation decoder. */
    IWICBitmap *bm = NULL;
    IWICBitmapLock *lock = NULL;
    WICRect rect = {0, 0, (INT)w, (INT)h};
    UINT stride = 0, capacity = 0;
    BYTE *dst = NULL;
    hr = IWICImagingFactory_CreateBitmap(D.factory, w, h, &GUID_WICPixelFormat32bppBGRA,
                                         WICBitmapCacheOnDemand, &bm);
    if (SUCCEEDED(hr))
        hr = IWICBitmap_Lock(bm, &rect, WICBitmapLockWrite, &lock);
    if (SUCCEEDED(hr))
        hr = IWICBitmapLock_GetStride(lock, &stride);
    if (SUCCEEDED(hr))
        hr = IWICBitmapLock_GetDataPointer(lock, &capacity, &dst);
    if (SUCCEEDED(hr)) {
        if (D.info.frames <= 1) {
            if (!WebPDecodeBGRAInto(D.data, n, dst, capacity, (int)stride))
                hr = WINCODEC_ERR_BADIMAGE;
        } else {
            WebPAnimDecoderOptions opt;
            WebPAnimDecoderOptionsInit(&opt);
            opt.color_mode = MODE_BGRA;
            anim = WebPAnimDecoderNew(&wd, &opt);
            uint8_t *pixels;
            int stamp;
            if (!anim || !WebPAnimDecoderGetNext(anim, &pixels, &stamp))
                hr = WINCODEC_ERR_BADIMAGE;
            else {
                for (unsigned y = 0; y < h; ++y)
                    memcpy(dst + (size_t)y * stride, pixels + (size_t)y * w * 4, (size_t)w * 4);
                D.webp=anim;anim=NULL;D.web_stamp=stamp;D.delay_ms=delay((uint32_t)stamp);D.frame_index=0;
            }
        }
    }
    DROP(lock);
    if (SUCCEEDED(hr))
        D.source = (IWICBitmapSource *)bm;
    else
        DROP(bm);

out:
    if (anim)
        WebPAnimDecoderDelete(anim);
    WebPDemuxDelete(dm);
    return hr;
}
/* Keep only the selected WIC page. Dimensions, orientation, ICC and metadata
 * belong to that page; build them privately before replacing the old source. */
static HRESULT wic_page(uint32_t page)
{
    IWICBitmapFrameDecode *frame=NULL;
    IWICBitmapSource *source=NULL;
    IWICColorContext *icc=NULL;
    imgp_info info=D.info;
    HRESULT hr=IWICBitmapDecoder_GetFrame(D.decoder,page,&frame);
    if (SUCCEEDED(hr))
        hr = IWICBitmapFrameDecode_GetSize(frame, &info.w, &info.h);
    if (SUCCEEDED(hr) && (!info.w || !info.h || info.w > IMGP_MAX_DIM || info.h > IMGP_MAX_DIM ||
                          (uint64_t)info.w * info.h > IMGP_MAX_PIXELS))
        hr = WINCODEC_ERR_IMAGESIZEOUTOFRANGE;
    if (SUCCEEDED(hr)) {
        source = (IWICBitmapSource *)frame;
        IUnknown_AddRef((IUnknown *)source);
        info.color=0;info.camera[0]=info.date[0]=0;
        info.orientation = 1;
        IWICMetadataQueryReader *r = NULL;
        if (SUCCEEDED(IWICBitmapFrameDecode_GetMetadataQueryReader(frame, &r))) {
            uint32_t o = number(r, L"/app1/ifd/{ushort=274}");
            if (!o)
                o = number(r, L"/ifd/{ushort=274}");
            if (o >= 1 && o <= 8)
                info.orientation = o;
            string_value(r, L"/app1/ifd/{ushort=272}", info.camera, 96);
            if (!info.camera[0])
                string_value(r, L"/ifd/{ushort=272}", info.camera, 96);
            string_value(r, L"/app1/ifd/exif/{ushort=36867}", info.date, 64);
            if (!info.date[0])
                string_value(r, L"/ifd/exif/{ushort=36867}", info.date, 64);
            if (!info.date[0])
                string_value(r, L"/app1/ifd/{ushort=306}", info.date, 64);
            if (!info.date[0])
                string_value(r, L"/ifd/{ushort=306}", info.date, 64);
            DROP(r);
        }
        UINT count = 0;
        IWICBitmapFrameDecode_GetColorContexts(frame, 0, NULL, &count);
        if (count) {
            hr = IWICImagingFactory_CreateColorContext(D.factory, &icc);
            if (SUCCEEDED(hr))
                hr = IWICBitmapFrameDecode_GetColorContexts(frame, 1, &icc, &count);
            if (FAILED(hr)) {
                DROP(icc);
                info.color = 2;
                hr = S_OK;
            } else
                info.color = 1;
        }
    }
    if(SUCCEEDED(hr)) {
        if(info.orientation>=5) {uint32_t t=info.w;info.w=info.h;info.h=t;}
        info.page=page;info.err=0;
        DROP(D.source);DROP(D.frame);DROP(D.icc);
        D.source=source;D.frame=frame;D.icc=icc;D.info=info;
    } else {DROP(source);DROP(frame);DROP(icc);}
    return hr;
}
static HRESULT open_image(const wchar_t *path, uint32_t cache_mb, uint32_t doc)
{
    close_image();
    D.id = doc;D.cache_mb=cache_mb;D.data_bytes=0;
    HRESULT hr = E_FAIL;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32(GetLastError());
    LARGE_INTEGER size;
    if (!GetFileSizeEx(f, &size)) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto read_out;
    }
    D.info.bytes = (uint64_t)size.QuadPart;
    if (size.QuadPart <= 0 || size.QuadPart > (uint64_t)cache_mb * 1024 * 1024) {
        hr = WINCODEC_ERR_IMAGESIZEOUTOFRANGE;
        goto read_out;
    }
    D.data = malloc((size_t)size.QuadPart);
    if (!D.data) {
        hr = E_OUTOFMEMORY;
        goto read_out;
    }
    DWORD off = 0, n = (DWORD)size.QuadPart;
    while (off < n) {
        if ((uint32_t)InterlockedCompareExchange(&Q.latest, 0, 0) != doc || Q.eof) {
            hr = E_ABORT;
            goto read_out;
        }
        DWORD got = 0, want = n - off > 65536 ? 65536 : n - off;
        if (!ReadFile(f, D.data + off, want, &got, NULL) || !got) {
            hr = HRESULT_FROM_WIN32(ERROR_READ_FAULT);
            goto read_out;
        }
        off += got;
    }
    hr = S_OK;
read_out:
    CloseHandle(f);
    if (FAILED(hr))
        return hr;
    D.data_bytes=n;
    const wchar_t *ext=wcsrchr(path,L'.');
    if(ext && !_wcsicmp(ext,L".svg")) { D.svg=1;hr=svg_open(D.data,n,&D.info); }
    else if (n >= 12 && !memcmp(D.data, "RIFF", 4) && !memcmp(D.data + 8, "WEBP", 4))
        hr = open_webp(n);
    else {
        hr = IWICImagingFactory_CreateStream(D.factory, &D.stream);
        if (SUCCEEDED(hr))
            hr = IWICStream_InitializeFromMemory(D.stream, D.data, n);
        if (SUCCEEDED(hr))
            hr = IWICImagingFactory_CreateDecoderFromStream(D.factory, (IStream *)D.stream, NULL,
                                                            WICDecodeMetadataCacheOnDemand, &D.decoder);
        if (SUCCEEDED(hr))
            hr = IWICBitmapDecoder_GetFrameCount(D.decoder, &D.info.frames);
        if (SUCCEEDED(hr) && (!D.info.frames || D.info.frames>10000)) hr=WINCODEC_ERR_IMAGESIZEOUTOFRANGE;
        if(SUCCEEDED(hr)) {
            GUID fmt;
            hr=IWICBitmapDecoder_GetContainerFormat(D.decoder,&fmt);
            if(SUCCEEDED(hr)) {
                const wchar_t *label=IsEqualGUID(&fmt,&GUID_ContainerFormatJpeg)?L"JPEG"
                    :IsEqualGUID(&fmt,&GUID_ContainerFormatPng)?L"PNG"
                    :IsEqualGUID(&fmt,&GUID_ContainerFormatGif)?L"GIF"
                    :IsEqualGUID(&fmt,&GUID_ContainerFormatTiff)?L"TIFF"
                    :IsEqualGUID(&fmt,&GUID_ContainerFormatBmp)?L"BMP"
                    :IsEqualGUID(&fmt,&GUID_ContainerFormatIco)?L"ICO":L"WIC image";
                wcscpy(D.info.format,label);
                D.info.pages=IsEqualGUID(&fmt,&GUID_ContainerFormatTiff)?D.info.frames:0;
                hr=wic_page(0);
            }
        }
    }
    if(SUCCEEDED(hr) && !wcscmp(D.info.format,L"GIF")) hr=gif_open();
    if (SUCCEEDED(hr) && !D.decoder && D.info.orientation >= 5) {
        uint32_t t = D.info.w;
        D.info.w = D.info.h;
        D.info.h = t;
    }
    return hr;
}
static HRESULT transform(IWICBitmapSource **s, uint32_t flags)
{
    if (!flags)
        return S_OK;
    IWICBitmapFlipRotator *r = NULL;
    HRESULT hr = IWICImagingFactory_CreateBitmapFlipRotator(D.factory, &r);
    if (SUCCEEDED(hr))
        hr = IWICBitmapFlipRotator_Initialize(r, *s, (WICBitmapTransformOptions)flags);
    if (SUCCEEDED(hr)) {
        DROP(*s);
        *s = (IWICBitmapSource *)r;
    } else
        DROP(r);
    return hr;
}
static HRESULT render(const imgp_render *r, uint32_t doc)
{
    if ((!D.source && !D.svg) || doc != D.id)
        return E_ABORT;
    if (r->bank >= IMGP_BANKS || r->rotation > 3 || r->flip > 1 || r->straight > 1 || !r->w || !r->h ||
        r->w > IMGP_EDGE || r->h > IMGP_EDGE || !r->cw || !r->ch)
        return E_INVALIDARG;
    if(D.svg) return svg_render(r,Q.pixels+(size_t)r->bank*IMGP_BANK_BYTES);
    HRESULT selected=select_frame(r->frame);
    if(FAILED(selected)) return selected;
    IWICBitmapSource *s = D.source;
    IUnknown_AddRef((IUnknown *)s);
    HRESULT hr = transform(&s, image_orientation(D.info.orientation));
    if (SUCCEEDED(hr))
        hr = transform(&s, r->rotation);
    if (SUCCEEDED(hr) && r->flip)
        hr = transform(&s, 8);
    UINT w = 0, h = 0;
    if (SUCCEEDED(hr))
        hr = IWICBitmapSource_GetSize(s, &w, &h);
    if (SUCCEEDED(hr) && (r->x > w || r->y > h || r->cw > w - r->x || r->ch > h - r->y))
        hr = E_INVALIDARG;
    IWICBitmapClipper *clip = NULL;
    IWICBitmapScaler *scale = NULL;
    IWICColorTransform *color = NULL;
    IWICColorContext *srgb = NULL;
    IWICFormatConverter *conv = NULL;
    if (SUCCEEDED(hr))
        hr = IWICImagingFactory_CreateBitmapClipper(D.factory, &clip);
    WICRect rect = {(INT)r->x, (INT)r->y, (INT)r->cw, (INT)r->ch};
    if (SUCCEEDED(hr))
        hr = IWICBitmapClipper_Initialize(clip, s, &rect);
    if (SUCCEEDED(hr)) {
        DROP(s);
        s = (IWICBitmapSource *)clip;
        clip = NULL;
    }
    if (SUCCEEDED(hr) && (r->w != r->cw || r->h != r->ch)) {
        hr = IWICImagingFactory_CreateBitmapScaler(D.factory, &scale);
        if (SUCCEEDED(hr))
            hr = IWICBitmapScaler_Initialize(scale, s, r->w, r->h,
                                             r->w > r->cw || r->h > r->ch
                                                 ? WICBitmapInterpolationModeNearestNeighbor
                                                 : WICBitmapInterpolationModeFant);
        if (SUCCEEDED(hr)) {
            DROP(s);
            s = (IWICBitmapSource *)scale;
            scale = NULL;
        }
    }
    if (SUCCEEDED(hr) && D.icc) {
        HRESULT ch = IWICImagingFactory_CreateColorContext(D.factory, &srgb);
        if (SUCCEEDED(ch))
            ch = IWICColorContext_InitializeFromExifColorSpace(srgb, 1);
        if (SUCCEEDED(ch))
            ch = IWICImagingFactory_CreateColorTransformer(D.factory, &color);
        if (SUCCEEDED(ch))
            ch = IWICColorTransform_Initialize(color, s, D.icc, srgb, &GUID_WICPixelFormat32bppBGRA);
        if (SUCCEEDED(ch)) {
            DROP(s);
            s = (IWICBitmapSource *)color;
            color = NULL;
        } else
            hr = ch; /* do not silently claim a managed rendering */
    }
    if (SUCCEEDED(hr))
        hr = IWICImagingFactory_CreateFormatConverter(D.factory, &conv);
    if (SUCCEEDED(hr))
        hr = IWICFormatConverter_Initialize(conv, s, &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                            NULL, 0, WICBitmapPaletteTypeCustom);
    uint8_t *dst = Q.pixels + (size_t)r->bank * IMGP_BANK_BYTES;
    if (SUCCEEDED(hr))
        hr = IWICFormatConverter_CopyPixels(conv, NULL, r->w * 4, r->w * r->h * 4, dst);
    if (SUCCEEDED(hr) && !r->straight)
        image_premultiply(dst, (size_t)r->w * r->h);
    DROP(conv);
    DROP(color);
    DROP(srgb);
    DROP(scale);
    DROP(clip);
    DROP(s);
    return hr;
}
int wmain(int argc, wchar_t **argv)
{
    if (argc != 2)
        return 2;
    HANDLE section = (HANDLE)(uintptr_t)_wcstoui64(argv[1], NULL, 10);
    Q.pixels = MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, (size_t)IMGP_BANKS * IMGP_BANK_BYTES);
    if (!Q.pixels)
        return 3;
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED)))
        return 4;
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                                (void **)&D.factory)))
        return 5;
    Q.in = GetStdHandle(STD_INPUT_HANDLE);
    Q.out = GetStdHandle(STD_OUTPUT_HANDLE);
    InitializeCriticalSection(&Q.lock);
    Q.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE t = CreateThread(NULL, 0, reader, NULL, 0, NULL);
    if (!t)
        return 6;
    while (!Q.eof) {
        msg *m = pop();
        if (!m) {
            WaitForSingleObject(Q.wake, INFINITE);
            continue;
        }
        int quit = m->h.type == IMGP_QUIT;
        if (m->h.type == IMGP_OPEN) {
            imgp_open p;
            memcpy(&p, m->data, sizeof p);
            if (p.cache_mb < 16 || p.cache_mb > 128)
                p.cache_mb = 64;
            HRESULT hr = m->h.doc == (uint32_t)Q.latest
                             ? open_image((wchar_t *)(m->data + sizeof p), p.cache_mb, m->h.doc)
                             : E_ABORT;
            D.info.err = (int32_t)hr;
            reply(IMGP_OPENED, m->h.doc, m->h.ticket, &D.info, sizeof D.info);
        } else if(m->h.type==IMGP_PAGE) {
            imgp_page p;memcpy(&p,m->data,sizeof p);
            HRESULT hr=m->h.doc!=D.id || m->h.doc!=(uint32_t)Q.latest ? E_ABORT
                :!D.decoder || !D.info.pages || p.page>=D.info.pages ? E_INVALIDARG :wic_page(p.page);
            imgp_info info=D.info;info.err=(int32_t)hr;info.page=p.page;
            reply(IMGP_PAGED,m->h.doc,m->h.ticket,&info,sizeof info);
        } else if (m->h.type == IMGP_RENDER) {
            imgp_render r;
            memcpy(&r, m->data, sizeof r);
            HRESULT hr = m->h.doc == (uint32_t)Q.latest ? render(&r, m->h.doc) : E_ABORT;
            imgp_rendered p = {r.bank, r.w, r.h, D.delay_ms, (int32_t)hr};
            reply(IMGP_RENDERED, m->h.doc, m->h.ticket, &p, sizeof p);
        } else if (m->h.type == IMGP_CLOSE)
            close_image();
        free(m);
        if (quit)
            break;
    }
    /* EOF or parent shutdown: kill the blocked reader by exiting the process. */
    close_image();
    DROP(D.factory);
    CoUninitialize();
    return 0;
}
