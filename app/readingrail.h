/* Shared native rail drawing/input; document coordinates belong to each reader. */
#pragma once
#include <windowsx.h>
#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include "theme.h"
#define RR_CAPACITY 533
typedef struct reading_target {double ratio;uint32_t value;int personal;wchar_t title[160],detail[80];} reading_target;
typedef struct reading_rail {RECT body,button;int hover,bookmark_hover,mouse_y,drag,hit;double ratio;DWORD error;} reading_rail;
enum {RR_NONE,RR_REDRAW,RR_JUMP,RR_SCRUB,RR_TOGGLE,RR_MENU};
static inline int rr_first(reading_rail *r){return r->body.top+app_px(12);}
static inline int rr_last(reading_rail *r){int last=r->body.bottom-app_px(12);return last>rr_first(r)?last:rr_first(r);}
static inline int rr_y(reading_rail *r,double ratio){return rr_first(r)+(int)((rr_last(r)-rr_first(r))*fmax(0,fmin(1,ratio))+.5);}
static inline int rr_nearest(reading_rail *r,const reading_target *targets,int count,int mouse,int *distance){int best=-1,d=INT_MAX;for(int i=0;i<count;i++){int delta=abs(rr_y(r,targets[i].ratio)-mouse);if(delta<d||(delta==d&&targets[i].personal)){best=i;d=delta;}}if(distance)*distance=d;return best;}
static inline void rr_fill(HDC dc,RECT rect,COLORREF color){SetDCBrushColor(dc,color);FillRect(dc,&rect,GetStockObject(DC_BRUSH));}
static inline void rr_text(HDC dc,const wchar_t *value,RECT rect,COLORREF color){SetTextColor(dc,color);DrawTextW(dc,value,-1,&rect,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX|DT_END_ELLIPSIS);}
static inline void rr_geometry(reading_rail *r,int width,int height,int top){r->body=(RECT){width-app_px(28),top,width,height};r->button=(RECT){width-app_px(28),app_px(3),width-app_px(5),app_px(27)};}
static inline void rr_paint(reading_rail *r,HDC dc,const reading_target *targets,int count,double ratio,int marked,int enabled){
 int saved=SaveDC(dc);SetBkMode(dc,TRANSPARENT);SelectObject(dc,app_font());rr_fill(dc,r->body,C_TREE_BG);
 for(int i=0;i<count;i++){int y=rr_y(r,targets[i].ratio),length=app_px(targets[i].personal?14:10),distance=r->hover?abs(y-r->mouse_y):INT_MAX;if(distance<app_px(42))length+=(app_px(42)-distance)*app_px(10)/app_px(42);RECT tick={r->body.right-app_px(4)-length,y,r->body.right-app_px(4),y+app_px(targets[i].personal?3:1)};rr_fill(dc,tick,targets[i].personal?C_FOLDER:C_DIM);}
 int pos=rr_y(r,ratio);RECT marker={r->body.right-app_px(3),pos-app_px(3),r->body.right,pos+app_px(4)};if(enabled)rr_fill(dc,marker,C_ACCENT);
 if(r->bookmark_hover)rr_fill(dc,r->button,C_SEL);int x=(r->button.left+r->button.right)/2,w=app_px(5),top=r->button.top+app_px(3),bottom=r->button.bottom-app_px(3);POINT p[]={{x-w,top},{x+w,top},{x+w,bottom},{x,bottom-app_px(4)},{x-w,bottom}};COLORREF color=!enabled?C_DIM:r->error?C_ERROR:marked?C_FOLDER:C_TEXT;HPEN pen=CreatePen(PS_SOLID,app_px(1),color);HBRUSH brush=CreateSolidBrush(marked?color:C_HEADER);HGDIOBJ oldpen=SelectObject(dc,pen),oldbrush=SelectObject(dc,brush);Polygon(dc,p,5);SelectObject(dc,oldbrush);SelectObject(dc,oldpen);DeleteObject(pen);DeleteObject(brush);
 if(r->hover||r->bookmark_hover){int index=rr_nearest(r,targets,count,r->mouse_y,NULL);const wchar_t *title=NULL,*detail=NULL;int height=app_px(56),width=r->body.right-app_px(44);if(width>app_px(340))width=app_px(340);
 if(r->bookmark_hover){title=!enabled?L"Open a preview to bookmark a position":r->error==ERROR_SHARING_VIOLATION?L"Bookmarks are in use. Try again.":r->error?L"Bookmark storage is unavailable; saved data kept.":marked?L"Remove bookmark (Ctrl+D)":L"Bookmark this position (Ctrl+D)";detail=L"Right-click for personal bookmarks";}
 else if(index>=0){title=targets[index].title;detail=targets[index].detail;}
 if(title&&width>app_px(50)){int y=r->bookmark_hover?r->body.top+app_px(4):r->mouse_y-height/2;if(y<r->body.top)y=r->body.top;if(y+height>r->body.bottom)y=r->body.bottom-height;RECT box={r->body.left-app_px(8)-width,y,r->body.left-app_px(8),y+height};rr_fill(dc,box,C_POPUP);RECT line={box.left+app_px(8),y+app_px(3),box.right-app_px(8),y+app_px(26)};rr_text(dc,title,line,C_TEXT);line.top=y+app_px(28);line.bottom=y+height-app_px(3);rr_text(dc,detail,line,C_DIM);}}
 RestoreDC(dc,saved);
}
static inline int rr_input(reading_rail *r,HWND hwnd,UINT message,WPARAM w,LPARAM l,const reading_target *targets,int count,int enabled){
 POINT p={GET_X_LPARAM(l),GET_Y_LPARAM(l)};int body=PtInRect(&r->body,p),button=PtInRect(&r->button,p);
 if(message==WM_MOUSEMOVE){if(body||button||r->hover||r->bookmark_hover){r->hover=body;r->bookmark_hover=button;r->mouse_y=p.y;TRACKMOUSEEVENT track={sizeof track,TME_LEAVE,hwnd,0};TrackMouseEvent(&track);InvalidateRect(hwnd,NULL,FALSE);}if(r->drag){int height=rr_last(r)-rr_first(r);r->ratio=height>0?fmax(0,fmin(1,(double)(p.y-rr_first(r))/height)):0;return RR_SCRUB;}if(body||button)return RR_REDRAW;}
 if(message==WM_MOUSELEAVE){r->hover=r->bookmark_hover=0;InvalidateRect(hwnd,NULL,FALSE);}
 if(message==WM_LBUTTONDOWN&&enabled){if(button)return RR_TOGGLE;if(body){int distance;r->hit=rr_nearest(r,targets,count,p.y,&distance);r->drag=1;SetCapture(hwnd);if(distance<=app_px(6))return RR_JUMP;int height=rr_last(r)-rr_first(r);r->ratio=height>0?fmax(0,fmin(1,(double)(p.y-rr_first(r))/height)):0;return RR_SCRUB;}}
 if(message==WM_LBUTTONUP&&r->drag){r->drag=0;if(GetCapture()==hwnd)ReleaseCapture();return RR_REDRAW;}
 if(message==WM_CAPTURECHANGED)r->drag=0;
 if(message==WM_CONTEXTMENU){if(l==-1)return RR_NONE;ScreenToClient(hwnd,&p);body=PtInRect(&r->body,p);button=PtInRect(&r->button,p);if(body||button){r->mouse_y=p.y;r->bookmark_hover=button;return RR_MENU;}}
 return RR_NONE;
}
/* Returns an entry index, -1 to toggle here, -2 when dismissed. */
static inline int rr_menu(reading_rail *r,HWND hwnd,const reading_target *targets,int count,int personal_only,int marked){HMENU menu=CreatePopupMenu();if(!menu)return -2;int n=0;for(int i=0;i<count;i++){if(personal_only?!targets[i].personal:abs(rr_y(r,targets[i].ratio)-r->mouse_y)>app_px(8))continue;wchar_t label[256];swprintf(label,256,L"%ls%ls · %ls",targets[i].personal?L"★ ":L"",targets[i].title,targets[i].detail);AppendMenuW(menu,MF_STRING,100+i,label);n++;}if(!n)AppendMenuW(menu,MF_GRAYED,1,L"No saved positions here yet");AppendMenuW(menu,MF_SEPARATOR,0,NULL);AppendMenuW(menu,MF_STRING,2,marked?L"Remove bookmark here\tCtrl+D":L"Bookmark this position\tCtrl+D");POINT point={r->body.left,r->bookmark_hover?r->button.bottom:r->mouse_y};ClientToScreen(hwnd,&point);UINT command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY|TPM_RIGHTALIGN,point.x,point.y,0,hwnd,NULL);DestroyMenu(menu);return command==2?-1:command>=100&&command<100+(UINT)count?(int)command-100:-2;}
