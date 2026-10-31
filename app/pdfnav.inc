/* PDF section rail. Included after the reader's layout and paint primitives. */
static const wchar_t *bookmark_identity(void){return R.mark_identity?R.mark_identity:R.path;}
static int nav_base_count(void){return R.outline_count?(int)R.outline_count:21;}
static double nav_range(void){return fmax(0,R.total-view_h());}
static int nav_first_y(void){return head_h()+app_px(12);}
static int nav_last_y(void){int y=R.ch-app_px(12);return y>nav_first_y()?y:nav_first_y();}
static RECT bookmark_rect(void){return (RECT){R.cw-pad()-app_px(26),app_px(2),R.cw-pad(),head_h()-app_px(2)};}
static int nav_entry(int index,uint32_t *page,float *y,double *ratio,const wchar_t **label,int *personal){
 int base=nav_base_count();*personal=index>=base;*label=NULL;
 if(index<0||!R.opened||!R.npages)return 0;
 if(index<base){if(R.outline_count){pdfp_outline_entry *e=R.outline+index;*page=e->page;*y=e->y;*label=(const wchar_t*)e->title;}
 else{double at=nav_range()*index/20.;*page=page_at_y(at);*y=(float)fmax(0,(at-R.ys[*page])/R.scale);}}
 else{index-=base;if((uint32_t)index>=R.mark_count)return 0;docmark *m=R.marks+index;*page=m->page;*y=m->y;*label=m->title;}
 if(*page>=R.npages)return 0;double at=R.ys[*page]+*y*R.scale;*ratio=nav_range()>0?fmax(0,fmin(1,at/nav_range())):0;return 1;
}
static int nav_y(double ratio){return nav_first_y()+(int)((nav_last_y()-nav_first_y())*ratio+.5);}
static int nav_nearest(int mouse_y,int *distance){
 int best=-1,nearest=INT_MAX;for(int i=0;i<nav_base_count()+(int)R.mark_count;i++){uint32_t page;float y;double ratio;const wchar_t *label;int personal;
 if(nav_entry(i,&page,&y,&ratio,&label,&personal)){int d=abs(nav_y(ratio)-mouse_y);if(d<nearest||(d==nearest&&personal)){nearest=d;best=i;}}}
 if(distance)*distance=nearest;return best;
}
static int bookmark_current(void){
 if(!R.opened||!R.npages||R.scale<=0)return -1;uint32_t page=page_at_y(R.top);float y=(float)fmax(0,(R.top-R.ys[page])/R.scale);
 for(uint32_t i=0;i<R.mark_count;i++)if(R.marks[i].page==page&&fabsf(R.marks[i].y-y)<12)return (int)i;return -1;
}
static void bookmarks_reload(void){
 R.mark_count=0;R.mark_error=R.mark_identity_failed?ERROR_NOT_ENOUGH_MEMORY:R.mark_file&&R.path?docmarks_load(R.mark_file,bookmark_identity(),R.marks,DOCMARK_PER_FILE,&R.mark_count):ERROR_PATH_NOT_FOUND;
 if(R.mark_error)R.mark_count=0;
}
static void bookmark_toggle(void){
 if(!R.opened||!R.npages||!R.path)return;
 if(R.mark_fallback){wchar_t dir[MAX_PATH+32];DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",dir,MAX_PATH);if(n&&n<MAX_PATH){wcscat(dir,L"\\Folderwing");CreateDirectoryW(dir,NULL);}}
 bookmarks_reload();if(R.mark_error){InvalidateRect(R.hwnd,NULL,FALSE);return;}
 int current=bookmark_current();docmark mark={0};if(current>=0)mark=R.marks[current];else{
  mark.page=page_at_y(R.top);mark.y=(float)fmax(0,(R.top-R.ys[mark.page])/R.scale);swprintf(mark.title,DOCMARK_TITLE,L"Page %u",mark.page+1);
  double best=-1;for(uint32_t i=0;i<R.outline_count;i++){pdfp_outline_entry *e=R.outline+i;if(e->page>=R.npages)continue;double at=R.ys[e->page]+e->y*R.scale;if(at<=R.top+1&&at>=best){best=at;wcsncpy(mark.title,(const wchar_t*)e->title,DOCMARK_TITLE-1);}}
 }
 DWORD error=docmarks_change(R.mark_file,bookmark_identity(),&mark,current>=0);if(!error)bookmarks_reload();else R.mark_error=error;InvalidateRect(R.hwnd,NULL,FALSE);
}
static void nav_jump(int index){uint32_t page;float y;double ratio;const wchar_t *label;int personal;if(nav_entry(index,&page,&y,&ratio,&label,&personal)){R.top=R.ys[page]+y*R.scale;view_changed();}}
static void nav_scrub(int y){int height=nav_last_y()-nav_first_y();if(height>0){R.top=fmax(0,fmin(1,(double)(y-nav_first_y())/height))*nav_range();view_changed();}}
static void nav_menu(int personal_only,int mouse_y){
 if(!R.opened||!R.npages)return;bookmarks_reload();HMENU menu=CreatePopupMenu();if(!menu)return;
 int base=nav_base_count(),shown=0;
 for(int i=personal_only?base:0;i<base+(int)R.mark_count;i++){uint32_t page;float y;double ratio;const wchar_t *label;int personal;
 if(!nav_entry(i,&page,&y,&ratio,&label,&personal))continue;
 if(!personal_only&&abs(nav_y(ratio)-mouse_y)>app_px(8))continue;
 wchar_t row[220];if(label)swprintf(row,220,L"%ls%ls  ·  Page %u",personal?L"★ ":L"",label,page+1);else swprintf(row,220,L"%u%%  ·  Page %u",i*5,page+1);
 AppendMenuW(menu,MF_STRING,100+i,row);shown++;
 }
 if(!shown)AppendMenuW(menu,MF_STRING|MF_GRAYED,1,personal_only?L"No personal bookmarks yet":L"No sections here");
 AppendMenuW(menu,MF_SEPARATOR,0,NULL);AppendMenuW(menu,MF_STRING,2,bookmark_current()>=0?L"Remove bookmark here\tCtrl+D":L"Bookmark this position\tCtrl+D");
 POINT point;if(personal_only){RECT r=bookmark_rect();point=(POINT){r.right,r.bottom};}else point=(POINT){R.cw-app_px(28),mouse_y};ClientToScreen(R.hwnd,&point);
 UINT command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY|TPM_RIGHTALIGN,point.x,point.y,0,R.hwnd,NULL);DestroyMenu(menu);
 if(command==2)bookmark_toggle();else if(command>=100&&command<100+(UINT)(base+R.mark_count))nav_jump((int)command-100);
}
static void nav_paint(HDC dc){
 if(!R.opened||!R.npages)return;
 int edge=R.cw-app_px(28);fill(dc,edge,head_h(),app_px(28),view_h(),C_TREE_BG);
 for(int i=0;i<nav_base_count()+(int)R.mark_count;i++){uint32_t page;float y;double ratio;const wchar_t *label;int personal;
 if(!nav_entry(i,&page,&y,&ratio,&label,&personal))continue;
 int at=nav_y(ratio),depth=R.outline_count&&i<(int)R.outline_count?(int)R.outline[i].depth:0;
 int length=personal?app_px(14):app_px(depth?6:10),distance=R.rail_hover?abs(at-R.rail_mouse_y):INT_MAX;
 if(distance<app_px(42))length+=(app_px(42)-distance)*app_px(10)/app_px(42);
 fill(dc,R.cw-app_px(5)-length,at,length,personal?app_px(3):app_px(1),personal?C_FOLDER:C_DIM);
 }
 int current=nav_y(nav_range()>0?R.top/nav_range():0);fill(dc,R.cw-app_px(4),current-app_px(3),app_px(3),app_px(7),C_ACCENT);
 if(R.rail_hover){int index=nav_nearest(R.rail_mouse_y,NULL);uint32_t page;float y;double ratio;const wchar_t *label;int personal;
 if(nav_entry(index,&page,&y,&ratio,&label,&personal)){
  int width=R.cw-app_px(44);if(width>app_px(320))width=app_px(320);int height=app_px(58),x=edge-app_px(8)-width,top=R.rail_mouse_y-height/2;if(top<head_h()+app_px(4))top=head_h()+app_px(4);if(top+height>R.ch)top=R.ch-height;
  if(width>app_px(50)){fill(dc,x,top,width,height,C_POPUP);wchar_t name[192],where[160];if(label)swprintf(name,192,L"%ls",label);else swprintf(name,192,L"%u%%",index*5);
   int nearby_count=0;for(int i=0;i<nav_base_count()+(int)R.mark_count;i++){uint32_t p;float yy;double rr;const wchar_t *ll;int pp;if(nav_entry(i,&p,&yy,&rr,&ll,&pp)&&abs(nav_y(rr)-nav_y(ratio))<=app_px(5))nearby_count++;}
   swprintf(where,160,L"%lsPage %u of %u%ls",personal?L"Personal bookmark · ":L"",page+1,R.npages,nearby_count>1?L" · Right-click for nearby entries":L"");
   text(dc,name,x+app_px(10),top+app_px(5),width-app_px(20),app_px(22),C_TEXT,DT_LEFT);text(dc,where,x+app_px(10),top+app_px(29),width-app_px(20),app_px(22),C_DIM,DT_LEFT);
  }
 }}
}
static void bookmark_paint(HDC dc){
 RECT r=bookmark_rect();if(R.bookmark_hover)fill(dc,r.left,r.top,r.right-r.left,r.bottom-r.top,C_SEL);
 int current=bookmark_current(),x=(r.left+r.right)/2,w=app_px(6),top=app_px(7),bottom=head_h()-app_px(6);
 POINT points[]={{x-w,top},{x+w,top},{x+w,bottom},{x,bottom-app_px(4)},{x-w,bottom}};
 COLORREF color=!R.opened?C_DIM:R.mark_error?C_ERROR:current>=0?C_FOLDER:C_TEXT;
 HPEN pen=CreatePen(PS_SOLID,app_px(1),color);HBRUSH brush=CreateSolidBrush(current>=0?color:C_HEADER);HGDIOBJ oldpen=SelectObject(dc,pen),oldbrush=SelectObject(dc,brush);Polygon(dc,points,5);SelectObject(dc,oldbrush);SelectObject(dc,oldpen);DeleteObject(pen);DeleteObject(brush);
 if(R.bookmark_hover){int width=R.cw-app_px(24);if(width>app_px(350))width=app_px(350);int x0=R.cw-width-app_px(12);wchar_t label[192];
 if(!R.opened)wcscpy(label,L"Open a PDF to bookmark a reading position");
 else if(R.mark_error){const wchar_t *reason=R.mark_error==ERROR_SHARING_VIOLATION?L"Bookmarks are in use. Try again.":R.mark_error==ERROR_INVALID_DATA?L"Bookmark storage needs review; saved data kept.":R.mark_error==ERROR_TOO_MANY_NAMES?L"Bookmark limit reached.":L"Bookmark storage is unavailable.";wcscpy(label,reason);}else swprintf(label,192,L"%ls · Right-click: saved marks",current>=0?L"Remove bookmark (Ctrl+D)":L"Bookmark this position (Ctrl+D)");
 fill(dc,x0,head_h()+app_px(4),width,app_px(32),C_POPUP);text(dc,label,x0+app_px(8),head_h()+app_px(4),width-app_px(16),app_px(32),C_TEXT,DT_LEFT);
 }
}
