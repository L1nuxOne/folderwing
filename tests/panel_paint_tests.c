/* Native side-panel paint contracts; includes the production window handlers.
 * Real GDI, controlled BeginPaint/EndPaint target and allocation faults.
 * Primary claims / impact binding (app/sidepanel.c):
 * test_header: native pixel contract, damage coordinates and hit geometry.
 * test_slot: native pixel/protocol contract, frame banks and ACK ordering.
 * test_resources: native resource contract, transient and failure cleanup. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
static HDC destination;
static RECT dirty;
static HRGN update_region;
static int width=420,height=320,dpi=96,saved,painting,fault,ended,ack_before_end;
static int copies,copy_x,copy_y,copy_w,copy_h,source_is_dib;
static uint64_t requested_pixels;
static int queued_bytes(void);
static HDC test_begin(HWND h,LPPAINTSTRUCT ps){memset(ps,0,sizeof *ps);ps->hdc=destination;ps->rcPaint=dirty;saved=SaveDC(destination);IntersectClipRect(destination,dirty.left,dirty.top,dirty.right,dirty.bottom);if(update_region)ExtSelectClipRgn(destination,update_region,RGN_AND);painting=1;ended=0;return destination;}
static BOOL test_end(HWND h,const PAINTSTRUCT *ps){ack_before_end=queued_bytes();ended=1;painting=0;RestoreDC(destination,saved);return TRUE;}
static BOOL test_rect(HWND h,LPRECT r){*r=(RECT){0,0,width,height};return TRUE;}
/* Real BeginPaint supplies a display DC. Preserve legacy DDB allocation
 * semantics even though the observation target is a CPU pixel surface. */
static HBITMAP test_compatible_bitmap(HDC dc,int w,int h){if(painting&&fault==2){fault=0;return NULL;}HDC display=GetDC(NULL);HBITMAP bmp=CreateCompatibleBitmap(display,w,h);ReleaseDC(NULL,display);return bmp;}
static HDC test_dc(HDC dc){if(painting&&fault==1){fault=0;return NULL;}return CreateCompatibleDC(dc);}
static HBITMAP test_dib(HDC dc,const BITMAPINFO *info,UINT use,void **bits,HANDLE map,DWORD off){if(painting&&fault==2){fault=0;return NULL;}return CreateDIBSection(dc,info,use,bits,map,off);}
static HGDIOBJ test_select(HDC dc,HGDIOBJ obj){if(painting&&dc!=destination&&fault==3&&GetObjectType(obj)==OBJ_BITMAP){fault=0;return HGDI_ERROR;}return SelectObject(dc,obj);}
static BOOL test_origin(HDC dc,int x,int y,LPPOINT previous){if(painting&&fault==4){fault=0;return FALSE;}return SetWindowOrgEx(dc,x,y,previous);}
static int test_clip(HDC dc,int l,int t,int r,int b){if(painting&&dc!=destination&&fault==5){fault=0;return ERROR;}return IntersectClipRect(dc,l,t,r,b);}
static BOOL test_blt(HDC dc,int x,int y,int w,int h,HDC from,int sx,int sy,DWORD rop){
 if(painting){copies++;copy_x=x;copy_y=y;copy_w=w;copy_h=h;requested_pixels+=(uint64_t)w*h;DIBSECTION ds={0};source_is_dib=GetObjectW(GetCurrentObject(from,OBJ_BITMAP),sizeof ds,&ds)==sizeof ds;if(fault==6){fault=0;return FALSE;}}
 return BitBlt(dc,x,y,w,h,from,sx,sy,rop);
}
#define BeginPaint test_begin
#define EndPaint test_end
#define GetClientRect test_rect
#define CreateCompatibleBitmap test_compatible_bitmap
#define CreateCompatibleDC test_dc
#define CreateDIBSection test_dib
#define SelectObject test_select
#define SetWindowOrgEx test_origin
#define IntersectClipRect test_clip
#define BitBlt test_blt
#include "../app/sidepanel.c"
#undef BeginPaint
#undef EndPaint
#undef GetClientRect
#undef CreateCompatibleBitmap
#undef CreateCompatibleDC
#undef CreateDIBSection
#undef SelectObject
#undef SetWindowOrgEx
#undef IntersectClipRect
#undef BitBlt
static writer queue;
static int queued_bytes(void){return (int)queue.len;}
static HFONT font;
int app_px(int v){return MulDiv(v,dpi,96);}
HFONT app_font(void){return font?font:GetStockObject(DEFAULT_GUI_FONT);}
static int passed,failed;
#define CHECK(name,x) do{if(x){passed++;printf("  ok    %s\n",name);}else{failed++;printf("  FAIL  %s line %d\n",name,__LINE__);}}while(0)
typedef struct surface {HDC dc;HBITMAP bitmap;HGDIOBJ old;uint32_t *pixels;int w,h;} surface;
static surface make_surface(int w,int h){surface s={0};s.w=w;s.h=h;s.dc=CreateCompatibleDC(NULL);BITMAPINFO info={.bmiHeader={sizeof(BITMAPINFOHEADER),w,-h,1,32,BI_RGB}};s.bitmap=CreateDIBSection(s.dc,&info,DIB_RGB_COLORS,(void**)&s.pixels,NULL,0);s.old=SelectObject(s.dc,s.bitmap);return s;}
static void drop_surface(surface *s){SelectObject(s->dc,s->old);DeleteObject(s->bitmap);DeleteDC(s->dc);memset(s,0,sizeof *s);}
static uint32_t sentinel(int x,int y){return ((x*19+y*7)&255)|(((x*3+y*13)&255)<<8)|(((x*11+y*5)&255)<<16);}
static void reset_surface(surface *s){GdiFlush();for(int y=0;y<s->h;y++)for(int x=0;x<s->w;x++)s->pixels[y*s->w+x]=sentinel(x,y);}
static int pixels_match(surface *reference,surface *actual,RECT damage){GdiFlush();for(int y=0;y<height;y++)for(int x=0;x<width;x++){int inside=x>=damage.left&&x<damage.right&&y>=damage.top&&y<damage.bottom&&(!update_region||PtInRegion(update_region,x,y));uint32_t expected=inside?reference->pixels[y*width+x]:sentinel(x,y);uint32_t got=actual->pixels[y*width+x];if((got&0xffffff)!=(expected&0xffffff)){printf("    pixel mismatch (%d,%d) expected=%06x got=%06x\n",x,y,expected&0xffffff,got&0xffffff);return 0;}}return 1;}
static void setup_state(int mode){memset(&P,0,sizeof P);P.hwnd=(HWND)(uintptr_t)0x5050;P.slot=(HWND)(uintptr_t)0x6060;P.open=1;P.w=width;P.h=height;wcscpy(P.apps[0].path,L"settings.exe");wcscpy(P.apps[0].title,L"Settings β");P.apps[0].state=A_RUNNING;P.cur=&P.apps[0];if(mode){wcscpy(P.apps[1].path,L"terminal.exe");wcscpy(P.apps[1].title,L"Terminal");P.apps[1].state=mode==1?A_HUNG:A_GONE;P.apps[1].exit_code=mode==2?0xc0000005u:0;P.cur=&P.apps[1];}}
static void render_header(surface *target,RECT area){destination=target->dc;dirty=area;copies=0;requested_pixels=0;queue.len=0;panel_proc(P.hwnd,WM_PAINT,0,0);GdiFlush();}
static void test_header(surface *reference,surface *actual){
 RECT regions[]={{0,0,420,30},{41,7,180,25},{0,85,5,190},{0,18,420,60},{-12,-5,25,9},{410,310,440,340}};
 for(int mode=0;mode<3;mode++){
  setup_state(mode);reset_surface(reference);paint_container(reference->dc,width,height);GdiFlush();RECT close=P.r_close,restart=P.r_restart,tabs[APPS];for(int i=0;i<APPS;i++)tabs[i]=P.apps[i].r_tab;
  for(unsigned i=0;i<sizeof regions/sizeof regions[0];i++){
   reset_surface(actual);render_header(actual,regions[i]);CHECK("container damage matches independent full render and preserves all outside pixels",pixels_match(reference,actual,regions[i]));
   int same=EqualRect(&close,&P.r_close)&&EqualRect(&restart,&P.r_restart);for(int j=0;j<APPS;j++)same=same&&EqualRect(&tabs[j],&P.apps[j].r_tab);CHECK("partial container repaint preserves close/restart/tab hit geometry",same);
  }
 }
 setup_state(0);reset_surface(reference);paint_container(reference->dc,width,height);
 HRGN piece=CreateRectRgn(100,12,180,28);update_region=CreateRectRgn(10,4,60,20);
 CombineRgn(update_region,update_region,piece,RGN_OR);DeleteObject(piece);
 reset_surface(actual);render_header(actual,(RECT){10,4,180,28});
 CHECK("disjoint update region preserves the gap inside its bounding rectangle",pixels_match(reference,actual,(RECT){10,4,180,28}));
 DeleteObject(update_region);update_region=NULL;
 setup_state(0);reset_surface(actual);render_header(actual,(RECT){0,0,width,30});
 CHECK("container source backing matches the requested arm",!source_is_dib);
 CHECK("header damage copies only its rectangle",copies==1&&copy_x==0&&copy_y==0&&copy_w==width&&copy_h==30&&requested_pixels==(uint64_t)width*30);
 
 {
  reset_surface(reference);paint_container(reference->dc,width,height);
  for(int failure=1;failure<=6;failure++){
   reset_surface(actual);fault=failure;render_header(actual,(RECT){41,7,180,25});CHECK("resource/presentation failure falls back to correct direct pixels",fault==0&&ended&&pixels_match(reference,actual,(RECT){41,7,180,25}));
  }
 }
}
static void reference_slot(surface *out,surface *frame,int present){GdiFlush();for(int y=0;y<height;y++)for(int x=0;x<width;x++)out->pixels[y*width+x]=present&&x<frame->w&&y<frame->h?frame->pixels[y*frame->w+x]:0x1e1e1eu;}
static void render_slot(surface *actual,RECT area){destination=actual->dc;dirty=area;copies=0;requested_pixels=0;queue.len=0;P.cur->out=&queue;P.cur->helloed=1;P.cur->ack_due=1;slot_proc(P.slot,WM_PAINT,0,0);GdiFlush();}
static void test_slot(surface *reference,surface *actual){
 surface frame=make_surface(300,200);CHECK("shared frame CPU surface is created",frame.dc&&frame.bitmap&&frame.pixels);
 for(int y=0;y<frame.h;y++)for(int x=0;x<frame.w;x++)frame.pixels[y*frame.w+x]=(x*3+y*5+0x73519u)&0xffffff;
 GdiFlush();SelectObject(frame.dc,frame.old); /* producer released selection before host reads */
 RECT regions[]={{11,13,51,46},{280,180,340,230},{330,12,390,48},{17,240,140,280},{0,0,420,320},{-9,-5,24,15}};
 for(int mode=0;mode<3;mode++){
  setup_state(0);P.cur->shown=0;P.cur->have_frame=mode==0;P.cur->bmp[0]=mode==0?frame.bitmap:NULL;P.cur->bw=frame.w;P.cur->bh=frame.h;P.cur->old_bmp[0]=mode==1?frame.bitmap:NULL;P.cur->old_w=frame.w;P.cur->old_h=frame.h;
  reset_surface(reference);reference_slot(reference,&frame,mode!=2);GdiFlush();
  for(unsigned i=0;i<sizeof regions/sizeof regions[0];i++){
   reset_surface(actual);render_slot(actual,regions[i]);CHECK("current/resize-fallback/empty frame damage matches independent pixel oracle",pixels_match(reference,actual,regions[i]));
   CHECK("frame acknowledgement is queued once after EndPaint",ended&&ack_before_end==0&&!P.cur->ack_due&&queue.len==sizeof(panelp_head)&&((panelp_head*)queue.buf)->type==PANELP_FRAME_ACK);
  }
 }
 setup_state(0);P.cur->have_frame=1;P.cur->bmp[0]=frame.bitmap;P.cur->bw=frame.w;P.cur->bh=frame.h;reset_surface(actual);render_slot(actual,(RECT){11,13,51,46});
 CHECK("helper-frame copy uses nonzero dirty source and destination coordinates",copies==1&&copy_x==11&&copy_y==13&&copy_w==40&&copy_h==33);
 
 DWORD before=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
 const int faults[]={1,3,6};
 reference_slot(reference,&frame,0);
 for(unsigned i=0;i<sizeof faults/sizeof faults[0];i++){
  reset_surface(actual);fault=faults[i];render_slot(actual,(RECT){11,13,51,46});
  CHECK("helper copy failure fills only damage and acknowledges after EndPaint",fault==0&&ended&&ack_before_end==0&&queue.len==sizeof(panelp_head)&&pixels_match(reference,actual,(RECT){11,13,51,46}));
 }
 CHECK("helper copy failure paths retain no GDI objects",before==GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS));
 P.cur->bmp[0]=NULL;P.cur->old_bmp[0]=NULL;P.cur->out=NULL;drop_surface(&frame);
}
static void test_resources(surface *reference,surface *actual){
 setup_state(0);render_header(actual,(RECT){0,0,width,30});DWORD before=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
 for(int i=0;i<150;i++)render_header(actual,(RECT){41,7,180,25});
 CHECK("150 dirty paints retain no temporary GDI objects",before==GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS));
 {for(int f=1;f<=6;f++){fault=f;render_header(actual,(RECT){41,7,180,25});}CHECK("all fault paths release temporary GDI objects",before==GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS));}
 reset_surface(actual);render_header(actual,(RECT){10,10,10,15});CHECK("empty damage leaves all destination pixels untouched",pixels_match(reference,actual,(RECT){10,10,10,15}));
}
int wmain(void){
 const int layouts[][3]={{420,320,96},{420,320,144},{840,640,192},{180,110,96}};
 for(unsigned layout=0;layout<sizeof layouts/sizeof layouts[0];layout++){
 width=layouts[layout][0];height=layouts[layout][1];dpi=layouts[layout][2];
 font=CreateFontW(-app_px(12),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Segoe UI");
 printf("layout %dx%d dpi=%d\n",width,height,dpi);
 surface reference=make_surface(width,height),actual=make_surface(width,height);CHECK("independent reference and destination surfaces exist",font&&reference.pixels&&actual.pixels);
 if(font&&reference.pixels&&actual.pixels){test_header(&reference,&actual);test_slot(&reference,&actual);test_resources(&reference,&actual);}
 drop_surface(&reference);drop_surface(&actual);DeleteObject(font);font=NULL;
 }
 free(queue.buf);
 printf("%d passed, %d failed\n",passed,failed);return failed?1:0;
}
