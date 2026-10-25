/* Direct2D SVG on software WARP, entirely inside the restricted image helper.
 * No browser, scripts or remote assets. XmlLite validates and reports unsupported
 * content instead of silently describing every SVG as faithfully rendered. */
#include <d3d11.h>
#include <d2d1_3.h>
#include <d2d1svg.h>
#include <xmllite.h>
#include <shlwapi.h>
#include <math.h>
#include <string.h>
#include "svgview.h"
template<class T> static void release(T *&p) { if(p) { p->Release(); p=nullptr; } }
static ID2D1DeviceContext5 *context;
static ID2D1SvgDocument *document;
static uint32_t width, height;
void svg_close(void) { release(document); release(context); width=height=0; }
static HRESULT validate(IStream *stream, uint32_t *partial)
{
    IXmlReader *xml=nullptr;
    HRESULT hr=CreateXmlReader(__uuidof(IXmlReader), (void**)&xml, nullptr);
    if(SUCCEEDED(hr)) hr=xml->SetProperty(XmlReaderProperty_DtdProcessing,DtdProcessing_Prohibit);
    if(SUCCEEDED(hr)) hr=xml->SetProperty(XmlReaderProperty_MaxElementDepth,128);
    if(SUCCEEDED(hr)) hr=xml->SetInput(stream);
    XmlNodeType type; unsigned nodes=0; bool root=false;
    while(SUCCEEDED(hr) && (hr=xml->Read(&type))==S_OK) {
        if(++nodes>100000) { hr=HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE); break; }
        if(type!=XmlNodeType_Element) continue;
        const wchar_t *name=nullptr; xml->GetLocalName(&name,nullptr);
        if(!root) { root=true; if(!name || wcscmp(name,L"svg")) { hr=E_INVALIDARG; break; } }
        static const wchar_t *tags[]={L"svg",L"circle",L"clipPath",L"defs",L"desc",L"ellipse",L"g",L"image",L"line",L"linearGradient",L"path",L"polygon",L"polyline",L"radialGradient",L"rect",L"stop",L"title",L"use"};
        bool known=false;
        for(auto tag:tags) if(name && !wcscmp(name,tag)) known=true;
        if(!known) *partial=1;
        if(xml->MoveToFirstAttribute()==S_OK) {
            do {
                const wchar_t *attribute=nullptr,*value=nullptr;
                xml->GetLocalName(&attribute,nullptr); xml->GetValue(&value,nullptr);
                if(attribute && !wcscmp(attribute,L"href") && value && value[0]!=L'#' && wcsncmp(value,L"data:image/",11)) *partial=1;
                const wchar_t *uri=nullptr;xml->GetNamespaceUri(&uri,nullptr);
                bool declaration=uri && !wcscmp(uri,L"http://www.w3.org/2000/xmlns/");
                static const wchar_t *attributes[]={L"id",L"style",L"transform",L"cx",L"cy",L"r",L"clipPathUnits",L"rx",L"ry",L"x",L"y",L"width",L"height",L"preserveAspectRatio",L"href",L"x1",L"y1",L"x2",L"y2",L"gradientUnits",L"gradientTransform",L"spreadMethod",L"d",L"points",L"fx",L"fy",L"offset",L"viewBox",L"clip-path",L"clip-rule",L"color",L"display",L"fill",L"fill-opacity",L"fill-rule",L"opacity",L"overflow",L"stop-color",L"stop-opacity",L"stroke",L"stroke-dasharray",L"stroke-dashoffset",L"stroke-linecap",L"stroke-linejoin",L"stroke-miterlimit",L"stroke-opacity",L"stroke-width",L"visibility",L"version",L"space"};
                bool supported=declaration;
                for(auto key:attributes) if(attribute && !wcscmp(attribute,key)) supported=true;
                if(!supported) *partial=1;
                if(attribute && !wcscmp(attribute,L"style") && value &&
                   (wcsstr(value,L"font-")||wcsstr(value,L"filter:")||wcsstr(value,L"mask:")||wcsstr(value,L"var("))) *partial=1;
            } while(xml->MoveToNextAttribute()==S_OK);
            xml->MoveToElement();
        }
    }
    if(hr==S_FALSE) hr=root?S_OK:E_INVALIDARG;
    release(xml);
    LARGE_INTEGER zero={}; stream->Seek(zero,STREAM_SEEK_SET,nullptr);
    return hr;
}
HRESULT svg_open(const uint8_t *data, uint32_t bytes, imgp_info *info)
{
    svg_close();
    IStream *stream=SHCreateMemStream(data,bytes);
    if(!stream) return E_OUTOFMEMORY;
    HRESULT hr=validate(stream,&info->partial);
    ID3D11Device *d3d=nullptr; IDXGIDevice *dxgi=nullptr;
    ID2D1Factory1 *factory=nullptr;
    ID2D1Device *device=nullptr; ID2D1DeviceContext *base=nullptr;
    if(SUCCEEDED(hr)) hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&d3d,nullptr,nullptr);
    if(SUCCEEDED(hr)) hr=d3d->QueryInterface(__uuidof(IDXGIDevice),(void**)&dxgi);
    if(SUCCEEDED(hr)) hr=D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,__uuidof(ID2D1Factory1),nullptr,(void**)&factory);
    if(SUCCEEDED(hr)) hr=factory->CreateDevice(dxgi,&device);
    if(SUCCEEDED(hr)) hr=device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,&base);
    if(SUCCEEDED(hr)) hr=base->QueryInterface(__uuidof(ID2D1DeviceContext5),(void**)&context);
    if(SUCCEEDED(hr)) hr=context->CreateSvgDocument(stream,D2D1_SIZE_F{300,150},&document);
    ID2D1SvgElement *root=nullptr;
    if(SUCCEEDED(hr)) {
        document->GetRoot(&root);
        D2D1_SVG_VIEWBOX box={};
        bool vb=SUCCEEDED(root->GetAttributeValue(L"viewBox",D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX,&box,sizeof box)) && box.width>0 && box.height>0;
        D2D1_SVG_LENGTH w={},h={};
        bool hasw=root->IsAttributeSpecified(L"width") && SUCCEEDED(root->GetAttributeValue(L"width",&w)) && w.units==D2D1_SVG_LENGTH_UNITS_NUMBER;
        bool hash=root->IsAttributeSpecified(L"height") && SUCCEEDED(root->GetAttributeValue(L"height",&h)) && h.units==D2D1_SVG_LENGTH_UNITS_NUMBER;
        double fw=hasw?w.value:vb?box.width:300, fh=hash?h.value:vb?box.height:150;
        if(vb && hasw && !hash) fh=fw*box.height/box.width;
        if(vb && hash && !hasw) fw=fh*box.width/box.height;
        if(!isfinite(fw)||!isfinite(fh)||fw<=0||fh<=0||fw>IMGP_MAX_DIM||fh>IMGP_MAX_DIM||ceil(fw)*ceil(fh)>IMGP_MAX_PIXELS) hr=WINCODEC_ERR_IMAGESIZEOUTOFRANGE;
        else {
            info->w=width=(uint32_t)ceil(fw); info->h=height=(uint32_t)ceil(fh);
            info->frames=info->orientation=1; wcscpy(info->format,L"SVG");
            document->SetViewportSize(D2D1_SIZE_F{(float)width,(float)height});
        }
    }
    release(root); release(base); release(device);release(factory); release(dxgi); release(d3d); release(stream);
    if(FAILED(hr)) svg_close();
    return hr;
}
static void point(float x,float y,const imgp_render *r,float *ox,float *oy)
{
    float a=x,b=y,ow=width;
    switch(r->rotation) {
    case 1: a=height-y;b=x;ow=height;break;
    case 2: a=width-x;b=height-y;break;
    case 3: a=y;b=width-x;ow=height;break;
    }
    if(r->flip) a=ow-a;
    *ox=(a-r->x)*r->w/r->cw; *oy=(b-r->y)*r->h/r->ch;
}
HRESULT svg_render(const imgp_render *r,uint8_t *pixels)
{
    if(!document || !context) return E_ABORT;
    uint32_t w=(r->rotation&1)?height:width,h=(r->rotation&1)?width:height;
    if(r->frame || r->x>w || r->y>h || r->cw>w-r->x || r->ch>h-r->y) return E_INVALIDARG;
    ID2D1Bitmap1 *target=nullptr,*readback=nullptr;
    D2D1_BITMAP_PROPERTIES1 props={}; props.pixelFormat={DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED};props.dpiX=props.dpiY=96;props.bitmapOptions=D2D1_BITMAP_OPTIONS_TARGET;
    D2D1_SIZE_U size={r->w,r->h};
    HRESULT hr=context->CreateBitmap(size,nullptr,0,&props,&target);
    if(SUCCEEDED(hr)) {
        context->SetTarget(target); context->SetDpi(96,96);
        float x0,y0,x1,y1,x2,y2;point(0,0,r,&x0,&y0);point(1,0,r,&x1,&y1);point(0,1,r,&x2,&y2);
        D2D1_MATRIX_3X2_F matrix={};matrix._11=x1-x0;matrix._12=y1-y0;matrix._21=x2-x0;matrix._22=y2-y0;matrix._31=x0;matrix._32=y0;
        context->SetTransform(&matrix);context->BeginDraw();
        D2D1_COLOR_F clear={};context->Clear(&clear);context->DrawSvgDocument(document);hr=context->EndDraw();
        context->SetTarget(nullptr);
    }
    props.bitmapOptions=(D2D1_BITMAP_OPTIONS)(D2D1_BITMAP_OPTIONS_CPU_READ|D2D1_BITMAP_OPTIONS_CANNOT_DRAW);
    if(SUCCEEDED(hr)) hr=context->CreateBitmap(size,nullptr,0,&props,&readback);
    if(SUCCEEDED(hr)) hr=readback->CopyFromBitmap(nullptr,target,nullptr);
    D2D1_MAPPED_RECT mapped={};
    if(SUCCEEDED(hr)) hr=readback->Map(D2D1_MAP_OPTIONS_READ,&mapped);
    if(SUCCEEDED(hr)) {
        for(uint32_t y=0;y<r->h;++y) memcpy(pixels+(size_t)y*r->w*4,mapped.bits+(size_t)y*mapped.pitch,(size_t)r->w*4);
        readback->Unmap();
        if(r->straight) for(size_t i=0;i<(size_t)r->w*r->h;++i) {
            uint8_t *p=pixels+i*4;unsigned a=p[3];
            for(int c=0;c<3;++c) p[c]=a?(uint8_t)(((unsigned)p[c]*255+a/2)/a):0;
        }
    }
    release(readback);release(target);return hr;
}
