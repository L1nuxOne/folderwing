#include "actiondock.h"
#include "theme.h"
#include <windowsx.h>
#include <commctrl.h>
#include <stdlib.h>
#include <string.h>
const char *const ad_names[AD_MORE]={"find","content","compare","size","duplicates","git","workspaces","terminal","operations","direct"};
const wchar_t *const ad_labels[AD_COUNT]={L"Find files",L"Search contents",L"Compare",L"Size",L"Duplicates",L"Git",L"Workspaces",L"Terminal",L"Operations",L"Direct agent",L"More tools and dock options"};
static struct {HWND hwnd,buttons[AD_COUNT],tooltip;ad_host host;ad_geometry geo;UINT dpi;int labels,context,releasing;HFONT font;WNDPROC button_proc;wchar_t tips[AD_COUNT][320];} D;
/* A dock menu can destroy its invoking button. Keep the common-controls DLL
 * cached until process exit: its subclass callback can still be on the stack. */
static HMODULE common_controls;
static int px(int n,UINT dpi){return MulDiv(n,(int)(dpi?dpi:96),96);}
static int minimum(int a,int b){return a<b?a:b;}
static int maximum(int a,int b){return a>b?a:b;}
void actiondock_geometry(int w,int h,int tree,int tw,UINT dpi,int position,int labels,uint32_t mask,ad_geometry *o){memset(o,0,sizeof *o);if(w<0)w=0;if(h<0)h=0;o->body=(RECT){0,0,w,h};o->effective=position==AD_TREE_BOTTOM&&!tree?AD_LEFT:position;int pad=px(4,dpi),cell=px(36,dpi),height=px(36,dpi);unsigned count=1;for(int i=0;i<AD_MORE;i++)count+=!!(mask&(1u<<i));
 if(o->effective==AD_LEFT){int width=minimum(w,px(labels?146:44,dpi));o->dock=(RECT){0,0,width,h};o->body.left=width;}
 else if(o->effective==AD_TOP){int bar=minimum(h,height+pad*2);o->dock=(RECT){0,0,w,bar};o->body.top=bar;}
 int body_width=o->body.right-o->body.left,max_tree=maximum(px(140,dpi),body_width-px(320,dpi));tw=maximum(px(140,dpi),minimum(tw,max_tree));tw=minimum(tw,maximum(0,body_width-px(32,dpi)));o->tree_width=tw;o->tree=(RECT){o->body.left,o->body.top,o->body.left+(tree?tw:0),h};
 if(o->effective==AD_TREE_BOTTOM){int bar=minimum(h,px(count==1?44:80,dpi));o->dock=(RECT){o->body.left,h-bar,o->body.left+tw,h};o->tree.bottom=h-bar;}
 if(o->effective==AD_HIDDEN)return;int dw=o->dock.right-o->dock.left,dh=o->dock.bottom-o->dock.top;if(dw<=0||dh<=0)return;int columns=1,rows=1,cw=dw-pad*2,ch=height;
 if(o->effective==AD_LEFT){rows=maximum(1,(dh-pad*2)/height);cw=maximum(1,dw-pad*2);}
 else{int smallest=px(labels?106:24,dpi),wanted=o->effective==AD_TREE_BOTTOM?(int)(count+1)/2:(int)count;columns=maximum(1,minimum(wanted,(dw-pad*2)/maximum(1,smallest)));rows=o->effective==AD_TREE_BOTTOM?minimum(2,maximum(1,(dh-pad*2)/height)):1;cw=maximum(1,minimum(px(labels?124:36,dpi),(dw-pad*2)/columns));}
 ch=maximum(1,minimum(ch,(dh-pad*2)/maximum(1,rows)));int slots=minimum(AD_COUNT,rows*columns),used=0;for(int id=0;id<AD_COUNT;id++){if(id!=AD_MORE&&!(mask&(1u<<id)))continue;if(id!=AD_MORE&&used>=slots-1)continue;if(used>=slots)break;int col=o->effective==AD_LEFT?0:used%columns,row=o->effective==AD_LEFT?used:used/columns;int x=minimum(pad,dw-1)+col*cw,y=minimum(pad,dh-1)+row*ch;o->buttons[id]=(RECT){x,y,minimum(dw,x+cw-px(2,dpi)),minimum(dh,y+ch-px(2,dpi))};if(o->buttons[id].right<=x)o->buttons[id].right=minimum(dw,x+1);if(o->buttons[id].bottom<=y)o->buttons[id].bottom=minimum(dh,y+1);o->visible|=1u<<id;used++;}
 (void)cell;}
static void fill(HDC dc,RECT r,COLORREF c){HBRUSH b=CreateSolidBrush(c);FillRect(dc,&r,b);DeleteObject(b);}
static void line(HDC dc,int x,int y,int a,int b){MoveToEx(dc,x,y,NULL);LineTo(dc,a,b);}
static void icon(HDC dc,int id,int x,int y,int size,COLORREF c){int saved=SaveDC(dc);SetMapMode(dc,MM_ANISOTROPIC);SetWindowExtEx(dc,24,24,NULL);SetViewportExtEx(dc,size,size,NULL);OffsetViewportOrgEx(dc,x,y,NULL);HPEN pen=CreatePen(PS_SOLID,1,c);HGDIOBJ old=SelectObject(dc,pen),brush=SelectObject(dc,GetStockObject(NULL_BRUSH));
 switch(id){case AD_FIND:Rectangle(dc,2,5,15,18);Ellipse(dc,9,9,19,19);line(dc,17,17,22,22);line(dc,3,3,8,3);break;
 case AD_CONTENT:Rectangle(dc,3,2,17,22);line(dc,6,6,14,6);line(dc,6,10,12,10);Ellipse(dc,12,12,20,20);line(dc,19,19,23,23);break;
 case AD_COMPARE:Rectangle(dc,1,3,9,21);Rectangle(dc,15,3,23,21);line(dc,7,9,17,9);line(dc,14,6,17,9);line(dc,17,9,14,12);line(dc,17,16,7,16);line(dc,10,13,7,16);line(dc,7,16,10,19);break;
 case AD_SIZE:Rectangle(dc,3,14,7,22);Rectangle(dc,10,9,14,22);Rectangle(dc,17,3,21,22);break;
 case AD_DUPLICATES:Rectangle(dc,3,2,15,17);Rectangle(dc,9,8,21,23);line(dc,12,13,18,13);line(dc,12,17,18,17);break;
 case AD_GIT:line(dc,7,6,7,18);line(dc,7,13,18,6);Ellipse(dc,4,2,10,8);Ellipse(dc,4,17,10,23);Ellipse(dc,15,2,21,8);break;
 case AD_WORKSPACES:for(int row=0;row<2;row++)for(int col=0;col<2;col++)Rectangle(dc,2+col*12,2+row*12,10+col*12,10+row*12);break;
 case AD_TERMINAL:Rectangle(dc,1,3,23,21);line(dc,5,8,9,12);line(dc,9,12,5,16);line(dc,12,16,19,16);break;
 case AD_DIRECT:RoundRect(dc,2,3,22,18,4,4);line(dc,6,18,6,23);line(dc,6,23,12,18);line(dc,6,8,18,8);line(dc,6,12,15,12);break;
 case AD_OPERATIONS:for(int i=0;i<3;i++){Rectangle(dc,2,3+i*7,6,7+i*7);line(dc,10,5+i*7,22-i*3,5+i*7);}break;
 default:for(int i=0;i<3;i++)Ellipse(dc,3+i*8,10,6+i*8,13);break;}
 SelectObject(dc,brush);SelectObject(dc,old);DeleteObject(pen);RestoreDC(dc,saved);}
static void query(int id,ad_state *s){memset(s,0,sizeof *s);s->enabled=1;if(D.host.query)D.host.query(id,s);if(!s->tooltip[0])wcsncpy(s->tooltip,ad_labels[id],319);}
static void draw(const DRAWITEMSTRUCT *item){int id=(int)item->CtlID-100;if(id<0||id>=AD_COUNT)return;ad_state state;query(id,&state);RECT r=item->rcItem;int hover=GetPropW(item->hwndItem,L"Dock.Hover")!=NULL;fill(item->hDC,r,state.selected?C_SEL:item->itemState&ODS_SELECTED?C_SEL_IDLE:hover?C_EDIT:C_TREE_BG);COLORREF c=state.enabled?(state.selected?C_ACCENT:C_TEXT):C_BORDER;int width=r.right-r.left,height=r.bottom-r.top,size=minimum(px(22,D.dpi),minimum(width,height)-px(6,D.dpi));if(size<1)size=1;int x=D.labels?px(7,D.dpi):(width-size)/2,y=(height-size)/2;icon(item->hDC,id,x,y,size,c);if(D.labels){RECT text={x+size+px(7,D.dpi),0,width-px(4,D.dpi),height};HGDIOBJ old=SelectObject(item->hDC,D.font);SetTextColor(item->hDC,c);SetBkMode(item->hDC,TRANSPARENT);DrawTextW(item->hDC,id==AD_MORE?L"More":ad_labels[id],-1,&text,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);SelectObject(item->hDC,old);}if(item->itemState&ODS_FOCUS){RECT focus=r;InflateRect(&focus,-2,-2);DrawFocusRect(item->hDC,&focus);}}
static void begin(void){if(!D.context){D.context=1;if(D.host.begin)D.host.begin();}}
static void end(void){if(D.context){D.context=0;if(D.host.end)D.host.end();}}
static void invoke(int id){begin();ad_state state;query(id,&state);if(state.enabled&&D.host.invoke){RECT r;GetWindowRect(D.buttons[id],&r);D.host.invoke(id,(POINT){r.left,r.bottom});}end();}
static void move_focus(HWND h){if(D.host.focus)D.host.focus(h);else if(GetForegroundWindow()==GetAncestor(D.hwnd,GA_ROOT))SetFocus(h);}
static void next(HWND from,int direction){int id=(int)GetDlgCtrlID(from)-100;for(int i=1;i<=AD_COUNT;i++){int candidate=(id+direction*i+AD_COUNT*2)%AD_COUNT;HWND h=D.buttons[candidate];if(IsWindowVisible(h)&&IsWindowEnabled(h)){move_focus(h);return;}}}
static LRESULT CALLBACK button_proc(HWND h,UINT m,WPARAM w,LPARAM l){int id=(int)GetDlgCtrlID(h)-100;WNDPROC old=D.button_proc;switch(m){case WM_SETFOCUS:if(D.host.enter)D.host.enter((HWND)w);break;case WM_LBUTTONDOWN:end();begin();break;case WM_LBUTTONUP:{D.releasing=1;LRESULT result=CallWindowProcW(old,h,m,w,l);D.releasing=0;end();return result;}case WM_CANCELMODE:end();break;case WM_CAPTURECHANGED:if(!D.releasing)end();break;
 case WM_MOUSEMOVE:if(!GetPropW(h,L"Dock.Hover")){SetPropW(h,L"Dock.Hover",(HANDLE)1);TRACKMOUSEEVENT track={sizeof track,TME_LEAVE,h,0};TrackMouseEvent(&track);InvalidateRect(h,NULL,FALSE);}break;case WM_MOUSELEAVE:RemovePropW(h,L"Dock.Hover");InvalidateRect(h,NULL,FALSE);break;
 case WM_CONTEXTMENU:begin();if(D.host.invoke){POINT p={GET_X_LPARAM(l),GET_Y_LPARAM(l)};if(p.x==-1&&p.y==-1){RECT r;GetWindowRect(h,&r);p=(POINT){r.left,r.bottom};}D.host.invoke(AD_MORE,p);}end();return 0;
 case WM_KEYDOWN:if(w==VK_RETURN||w==VK_SPACE){if(!(l&(1ll<<30)))invoke(id);return 0;}if(w==VK_RIGHT||w==VK_DOWN){next(h,1);return 0;}if(w==VK_LEFT||w==VK_UP){next(h,-1);return 0;}if(w==VK_HOME){move_focus(actiondock_focus_target());return 0;}if(w==VK_TAB||w==VK_ESCAPE){if(D.host.leave)D.host.leave(w==VK_ESCAPE?0:w==VK_TAB&&(GetKeyState(VK_SHIFT)&0x8000)?-1:1);return 0;}if(w==VK_APPS||(w==VK_F10&&(GetKeyState(VK_SHIFT)&0x8000))){SendMessageW(h,WM_CONTEXTMENU,(WPARAM)h,-1);return 0;}if(D.host.key&&D.host.key((UINT)w,0,l))return 0;break;
 case WM_SYSKEYDOWN:if(D.host.key&&D.host.key((UINT)w,1,l))return 0;break;
 case WM_KEYUP:case WM_CHAR:if(w==VK_SPACE||w==VK_RETURN||w==VK_TAB)return 0;break;case WM_GETDLGCODE:return DLGC_WANTARROWS|DLGC_WANTTAB;}
 return CallWindowProcW(old,h,m,w,l);}
/* Disabled BUTTONs pass mouse movement to their parent, so relay that movement
 * to their registered tool as well. Enabled buttons use TTF_SUBCLASS normally. */
static void disabled_tip(HWND h,WPARAM w,LPARAM l){
 if(!D.tooltip)return;POINT p={GET_X_LPARAM(l),GET_Y_LPARAM(l)};
 HWND child=ChildWindowFromPointEx(h,p,CWP_SKIPINVISIBLE|CWP_SKIPTRANSPARENT);
 if(child==h||!actiondock_contains(child)||IsWindowEnabled(child))return;
 MapWindowPoints(h,child,&p,1);MSG event={.hwnd=child,.message=WM_MOUSEMOVE,.wParam=w,.lParam=MAKELPARAM(p.x,p.y),.time=(DWORD)GetMessageTime()};GetCursorPos(&event.pt);
 SendMessageW(D.tooltip,TTM_RELAYEVENT,0,(LPARAM)&event);
}
static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l){switch(m){case WM_MOUSEMOVE:disabled_tip(h,w,l);break;case WM_DRAWITEM:draw((const DRAWITEMSTRUCT*)l);return TRUE;case WM_COMMAND:if(HIWORD(w)==BN_CLICKED){int id=LOWORD(w)-100;if(id>=0&&id<AD_COUNT)invoke(id);return 0;}break;case WM_CONTEXTMENU:actiondock_popup();return 0;case WM_ERASEBKGND:return 1;case WM_PRINTCLIENT:{RECT r;GetClientRect(h,&r);fill((HDC)w,r,C_TREE_BG);return 0;}case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);fill(dc,ps.rcPaint,C_TREE_BG);EndPaint(h,&ps);return 0;}case WM_SETFOCUS:{if(D.host.enter)D.host.enter((HWND)w);HWND target=actiondock_focus_target();if(target)move_focus(target);return 0;}}return DefWindowProcW(h,m,w,l);}
HWND actiondock_create(HWND parent,const ad_host *host){if(D.hwnd)return D.hwnd;D.host=*host;D.dpi=96;WNDCLASSW cls={.lpfnWndProc=proc,.hInstance=GetModuleHandleW(NULL),.hCursor=LoadCursorW(NULL,IDC_ARROW),.lpszClassName=L"OpenPilotActionDock"};RegisterClassW(&cls);D.hwnd=CreateWindowExW(0,cls.lpszClassName,L"Action dock",WS_CHILD|WS_CLIPSIBLINGS|WS_CLIPCHILDREN,0,0,0,0,parent,NULL,cls.hInstance,NULL);if(!D.hwnd)return NULL;
 if(!common_controls)common_controls=LoadLibraryExW(L"comctl32.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);if(common_controls){typedef BOOL(WINAPI *init_fn)(const INITCOMMONCONTROLSEX*);init_fn init=(init_fn)(void*)GetProcAddress(common_controls,"InitCommonControlsEx");INITCOMMONCONTROLSEX c={sizeof c,ICC_WIN95_CLASSES};if(init)init(&c);D.tooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,NULL,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,0,0,0,0,D.hwnd,NULL,cls.hInstance,NULL);if(D.tooltip){SendMessageW(D.tooltip,TTM_SETMAXTIPWIDTH,0,px(350,D.dpi));SendMessageW(D.tooltip,TTM_SETDELAYTIME,TTDT_INITIAL,400);}}
 for(int i=0;i<AD_COUNT;i++){D.buttons[i]=CreateWindowExW(0,L"BUTTON",ad_labels[i],WS_CHILD|WS_TABSTOP|BS_OWNERDRAW,0,0,0,0,D.hwnd,(HMENU)(INT_PTR)(100+i),cls.hInstance,NULL);if(!D.buttons[i]){actiondock_destroy();return NULL;}WNDPROC old=(WNDPROC)SetWindowLongPtrW(D.buttons[i],GWLP_WNDPROC,(LONG_PTR)button_proc);if(!D.button_proc)D.button_proc=old;if(D.tooltip){/* V2 size also supports the unmanifested v5 common-controls DLL. */TOOLINFOW t={.cbSize=TTTOOLINFOW_V2_SIZE,.uFlags=TTF_IDISHWND|TTF_SUBCLASS,.hwnd=D.hwnd,.uId=(UINT_PTR)D.buttons[i],.lpszText=D.tips[i]};SendMessageW(D.tooltip,TTM_ADDTOOLW,0,(LPARAM)&t);}}
 return D.hwnd;}
void actiondock_destroy(void){end();HWND h=D.hwnd;D.hwnd=NULL;if(D.tooltip)DestroyWindow(D.tooltip);D.tooltip=NULL;if(h)DestroyWindow(h);memset(&D,0,sizeof D);}
HWND actiondock_window(void){return D.hwnd;}
HWND actiondock_button(int id){return id>=0&&id<AD_COUNT?D.buttons[id]:NULL;}
int actiondock_contains(HWND h){return h&&D.hwnd&&(h==D.hwnd||IsChild(D.hwnd,h));}
HWND actiondock_focus_target(void){for(int i=0;i<AD_COUNT;i++)if(D.buttons[i]&&IsWindowVisible(D.buttons[i])&&IsWindowEnabled(D.buttons[i]))return D.buttons[i];return D.hwnd;}
void actiondock_refresh(void){if(!D.hwnd)return;for(int i=0;i<AD_COUNT;i++){if(!(D.geo.visible&(1u<<i)))continue;ad_state s;query(i,&s);EnableWindow(D.buttons[i],s.enabled);wcsncpy(D.tips[i],s.tooltip,319);D.tips[i][319]=0;if(D.tooltip){TOOLINFOW t={.cbSize=TTTOOLINFOW_V2_SIZE,.uFlags=TTF_IDISHWND,.hwnd=D.hwnd,.uId=(UINT_PTR)D.buttons[i],.lpszText=D.tips[i]};SendMessageW(D.tooltip,TTM_UPDATETIPTEXTW,0,(LPARAM)&t);}InvalidateRect(D.buttons[i],NULL,FALSE);}}
void actiondock_place(const ad_geometry *g,UINT dpi,int labels,HFONT font){if(!D.hwnd)return;D.geo=*g;D.dpi=dpi?dpi:96;D.labels=labels;D.font=font;SetWindowPos(D.hwnd,NULL,g->dock.left,g->dock.top,g->dock.right-g->dock.left,g->dock.bottom-g->dock.top,SWP_NOZORDER|SWP_NOACTIVATE|SWP_SHOWWINDOW);for(int i=0;i<AD_COUNT;i++){RECT r=g->buttons[i];if(g->visible&(1u<<i))SetWindowPos(D.buttons[i],NULL,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE|SWP_SHOWWINDOW);else ShowWindow(D.buttons[i],SW_HIDE);}if(D.tooltip)SendMessageW(D.tooltip,TTM_SETMAXTIPWIDTH,0,px(350,D.dpi));actiondock_refresh();}
void actiondock_popup(void){if(!D.hwnd)return;RECT r;GetWindowRect(D.hwnd,&r);begin();if(D.host.invoke)D.host.invoke(AD_MORE,(POINT){r.left,r.top});end();}
