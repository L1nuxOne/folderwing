/* Real reader/helper on hidden owned windows. No desktop input is injected. */
#include "../app/pdfview.c"
#include <stdio.h>
static int passed,failed;static wchar_t fixtures[32768],memory_file[32768];static HWND owner;
#define CHECK(label,condition) do{if(condition){passed++;printf("ok %s\n",label);}else{failed++;printf("FAIL %d %s\n",__LINE__,label);}}while(0)
HFONT app_font(void){return GetStockObject(DEFAULT_GUI_FONT);}int app_px(int n){return n;}
void app_focus_window(HWND h){(void)h;}void app_focus_cycle(int n){(void)n;}void app_preview_close(void){pdfview_clear();}
void app_preview_ctx(fn_ctx *c,HWND h,int from){memset(c,0,sizeof *c);c->from=from;(void)h;}void app_ctx_free(fn_ctx *c){(void)c;}
int fn_key(uint8_t key,uint8_t mods,fn_ctx *c){(void)key;(void)mods;(void)c;return 0;}
void app_status(Pane *p,int error,const wchar_t *format,...){(void)p;(void)error;(void)format;}
uint64_t app_clipboard_begin(HWND h){(void)h;return 1;}void app_clipboard_cancel(HWND h){(void)h;}
void app_clipboard_submit(uint64_t request,UINT format,HGLOBAL data,UINT extra_format,HGLOBAL extra,const wchar_t *success){(void)request;(void)format;(void)extra_format;(void)success;if(data)GlobalFree(data);if(extra)GlobalFree(extra);}
static void pump_nav(int ms){uint64_t end=GetTickCount64()+ms;do{MSG m;while(PeekMessageW(&m,NULL,0,0,PM_REMOVE))if(m.message!=WM_QUIT){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(2);}while(GetTickCount64()<end);}
static int open_nav(const wchar_t *name,int outline){wchar_t path[32768];swprintf(path,32768,L"%ls\\%ls",fixtures,name);pdfview_open(path,0,0);uint64_t end=GetTickCount64()+15000;while((!R.opened||(outline&&!R.outline_count))&&!R.err&&!R.gone&&GetTickCount64()<end)pump_nav(10);pump_nav(120);printf("reader opened=%d pages=%u outline=%u error=%d gone=%d\n",R.opened,R.npages,R.outline_count,R.err,R.gone);return R.opened&&(!outline||R.outline_count);}
static void test_docmarks_persistence_and_conflicts(void){
 wchar_t store[32768];swprintf(store,32768,L"%ls\\storage.fwb",fixtures);docmark one={.page=2,.y=33},two={.page=5,.y=72},out[8];uint32_t n=0;wcscpy(one.title,L"Équations 日本語");wcscpy(two.title,L"Second mark");
 CHECK("missing bookmark store is empty",!docmarks_load(store,L"C:\\owned.pdf",out,8,&n)&&!n);
 CHECK("first personal mark saved",!docmarks_change(store,L"C:\\Owned.pdf",&one,0)&&one.id);
 CHECK("separate callers append without replacing older marks",!docmarks_change(store,L"c:\\owned.PDF",&two,0)&&!docmarks_load(store,L"C:\\owned.pdf",out,8,&n)&&n==2);
 CHECK("Unicode title and point offset round trip",!wcscmp(out[0].title,one.title)&&out[0].page==2&&fabsf(out[0].y-33)<.001);
 CHECK("deletion preserves the other caller's mark",!docmarks_change(store,L"C:\\owned.pdf",&one,1)&&!docmarks_load(store,L"C:\\owned.pdf",out,8,&n)&&n==1&&out[0].id==two.id);
 wchar_t lockpath[32768];swprintf(lockpath,32768,L"%ls.lock",store);HANDLE lock=CreateFileW(lockpath,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);one.id=0;
 CHECK("busy store fails without overwriting",lock!=INVALID_HANDLE_VALUE&&docmarks_change(store,L"C:\\owned.pdf",&one,0)==ERROR_SHARING_VIOLATION);if(lock!=INVALID_HANDLE_VALUE)CloseHandle(lock);
 CHECK("busy failure preserved saved data",!docmarks_load(store,L"C:\\owned.pdf",out,8,&n)&&n==1);
 one.id=0;CHECK("Linux uppercase path mark saved",!docmarks_change(store,L"\\\\wsl.localhost\\Ubuntu\\tmp\\A.pdf",&one,0));one.id=0;CHECK("Linux lowercase path is distinct",!docmarks_change(store,L"\\\\wsl.localhost\\Ubuntu\\tmp\\a.pdf",&one,0)&&!docmarks_load(store,L"\\\\wsl.localhost\\Ubuntu\\tmp\\A.pdf",out,8,&n)&&n==1);
 wchar_t corrupt[32768];swprintf(corrupt,32768,L"%ls\\corrupt.fwb",fixtures);HANDLE f=CreateFileW(corrupt,GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);DWORD wrote;WriteFile(f,"preserve invalid data",21,&wrote,NULL);CloseHandle(f);
 CHECK("corrupt store rejected on read and mutation",docmarks_load(corrupt,L"C:\\owned.pdf",out,8,&n)==ERROR_INVALID_DATA&&docmarks_change(corrupt,L"C:\\owned.pdf",&one,0)==ERROR_INVALID_DATA);
 char bytes[32]={0};f=CreateFileW(corrupt,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);DWORD got=0;ReadFile(f,bytes,31,&got,NULL);CloseHandle(f);CHECK("corrupt bookmark data remains unchanged",got==21&&!memcmp(bytes,"preserve invalid data",21));
}
static void test_pdf_outline_and_fallback_navigation(void){
 CHECK("real PDF helper opens structured document",open_nav(L"outline.pdf",1)&&R.npages==12);CHECK("only internal outline destinations are exposed",R.outline_count==3);
 if(R.outline_count==3){CHECK("hierarchy and Unicode title survive IPC",R.outline[1].depth==1&&!wcscmp((wchar_t*)R.outline[1].title,L"Équations 日本語"));nav_jump(1);CHECK("outline click jumps to its point destination",fabs(R.top-(R.ys[4]+72*R.scale))<2);}
 CHECK("plain PDF opens without an outline",open_nav(L"plain.pdf",0)&&!R.outline_count&&nav_base_count()==21);
 nav_jump(7);CHECK("fallback tick represents 35 percent of scroll range",fabs(R.top-nav_range()*.35)<2);
 nav_scrub(nav_last_y());CHECK("rail drag reaches document bottom",fabs(R.top-nav_range())<1);nav_scrub(nav_first_y());CHECK("rail drag reaches document start",R.top==0);
 CHECK("cyclic outline traversal is bounded",open_nav(L"cycle.pdf",1)&&R.outline_count==3);
}
static void test_pdf_personal_bookmark_button_and_restore(void){
 CHECK("reader prepared for personal marks",open_nav(L"outline.pdf",1));nav_jump(1);RECT button=bookmark_rect();
 SendMessageW(R.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM((button.left+button.right)/2,(button.top+button.bottom)/2));
 CHECK("bookmark icon saves current reading position",R.mark_count==1&&bookmark_current()==0&&!R.mark_error);
 CHECK("personal mark uses its containing section title",!wcscmp(R.marks[0].title,L"Équations 日本語"));
 uint64_t id=R.marks[0].id;nav_jump(0);nav_jump(nav_base_count());CHECK("personal rail mark restores its saved offset",bookmark_current()==0&&R.marks[0].id==id);
 pdfview_clear();CHECK("reader clear removes old rail targets",!R.outline_count&&!R.mark_count&&!R.rail_hover);
 CHECK("bookmarks survive reopen with recent-position memory disabled",open_nav(L"outline.pdf",1)&&R.mark_count==1&&R.marks[0].id==id);nav_jump(nav_base_count());bookmark_toggle();CHECK("toggle removes saved personal mark",!R.mark_count&&!R.mark_error);
 CHECK("document change starts with no old rail targets",open_nav(L"plain.pdf",0)&&!R.outline_count&&!R.mark_count);
 msg *stale=calloc(1,sizeof(msg)+sizeof(pdfp_outline));stale->head=(pdfp_head){PDFP_OUTLINED,sizeof(pdfp_outline),R.doc-1,0};SendMessageW(R.hwnd,WM_PDF_MSG,R.launch_gen,(LPARAM)stale);CHECK("stale source replies cannot replace the current outline",!R.outline_count);
 msg *invalid=calloc(1,sizeof(msg)+sizeof(pdfp_outline)+sizeof(pdfp_outline_entry));invalid->head=(pdfp_head){PDFP_OUTLINED,sizeof(pdfp_outline)+sizeof(pdfp_outline_entry),R.doc,0};((pdfp_outline*)invalid->data)->count=1;((pdfp_outline_entry*)(invalid->data+sizeof(pdfp_outline)))->page=R.npages;SendMessageW(R.hwnd,WM_PDF_MSG,R.launch_gen,(LPARAM)invalid);CHECK("invalid outline destinations are rejected",!R.outline_count);
 pdfview_set_bookmark_identity(L"C:\\owned.zip\x1f" L"0041002E007000640066");nav_scrub(nav_y(.5));bookmark_toggle();CHECK("archive identity saves a personal mark",R.mark_count==1&&!R.mark_error);
 CHECK("different staging document opens",open_nav(L"outline.pdf",1));pdfview_set_bookmark_identity(L"c:\\OWNED.ZIP\x1f" L"0041002E007000640066");CHECK("stable archive identity survives changing staging paths",R.mark_count==1);
 pdfview_set_bookmark_identity(L"C:\\owned.zip\x1f" L"0061002E007000640066");CHECK("case-distinct archive entry has independent bookmarks",R.mark_count==0);

}
static void test_pdf_rail_paint_and_input(void){
 CHECK("rail paint fixture opens",open_nav(L"outline.pdf",1));nav_jump(1);bookmark_toggle();
 int distance=0,index=nav_nearest(nav_y(nav_range()>0?R.top/nav_range():0),&distance);CHECK("personal mark wins when ticks share a position",index>=nav_base_count()&&distance<=1);
 uint64_t until=GetTickCount64()+5000;int rendered=0;do{for(int i=0;i<PDFP_SLOTS;i++)if(R.slots[i].state==S_READY&&R.slots[i].page==page_at_y(R.top))rendered=1;if(!rendered)pump_nav(10);}while(!rendered&&GetTickCount64()<until);CHECK("jumped page receives decoded pixels",rendered);
 R.rail_hover=1;R.rail_mouse_y=nav_y(nav_range()>0?R.top/nav_range():0);int width=R.cw,height=R.ch;BITMAPINFO bi={.bmiHeader={sizeof(BITMAPINFOHEADER),width,-height,1,32,BI_RGB}};void *pixels=NULL;HBITMAP bitmap=CreateDIBSection(NULL,&bi,DIB_RGB_COLORS,&pixels,NULL,0);HDC dc=CreateCompatibleDC(NULL);HGDIOBJ old=SelectObject(dc,bitmap);SetBkMode(dc,TRANSPARENT);paint(dc);GdiFlush();
 unsigned ink=0;for(int y=45;y<220;y+=2)for(int x=20;x<400;x+=2){COLORREF color=GetPixel(dc,x,y);if(GetRValue(color)<160&&GetGValue(color)<160&&GetBValue(color)<160)ink++;}CHECK("navigation still paints real page text",ink>5);
 CHECK("rail background is painted independently of page tiles",GetPixel(dc,width-27,head_h()+2)==C_TREE_BG);
 CHECK("reading-position indicator uses its distinct accent",GetPixel(dc,width-3,nav_y(nav_range()>0?R.top/nav_range():0))==C_ACCENT);
 wchar_t screenshot[32768];if(GetEnvironmentVariableW(L"FOLDERWING_PDF_NAV_SCREENSHOT",screenshot,32768)){HANDLE f=CreateFileW(screenshot,GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);if(f!=INVALID_HANDLE_VALUE){DWORD bytes=width*height*4,wrote;BITMAPFILEHEADER header={.bfType=0x4d42,.bfSize=sizeof header+sizeof(BITMAPINFOHEADER)+bytes,.bfOffBits=sizeof header+sizeof(BITMAPINFOHEADER)};WriteFile(f,&header,sizeof header,&wrote,NULL);WriteFile(f,&bi.bmiHeader,sizeof(BITMAPINFOHEADER),&wrote,NULL);WriteFile(f,pixels,bytes,&wrote,NULL);CloseHandle(f);}}
 SelectObject(dc,old);DeleteDC(dc);DeleteObject(bitmap);
 SendMessageW(R.hwnd,WM_MOUSELEAVE,0,0);CHECK("leaving the rail clears hover state",!R.rail_hover&&!R.bookmark_hover);
 SendMessageW(R.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(width-10,nav_first_y()));SendMessageW(R.hwnd,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(width-10,nav_last_y()));SendMessageW(R.hwnd,WM_LBUTTONUP,0,MAKELPARAM(width-10,nav_last_y()));CHECK("owned mouse messages scrub without selecting text",!R.sel&&!R.rail_drag&&fabs(R.top-nav_range())<1);
}
int wmain(int argc,wchar_t **argv){
 setvbuf(stdout,NULL,_IONBF,0);if(argc!=2)return 2;wcsncpy(fixtures,argv[1],32767);swprintf(memory_file,32768,L"%ls\\positions.txt",fixtures);
 owner=CreateWindowExW(0,L"STATIC",L"Owned PDF rail test",WS_OVERLAPPEDWINDOW,0,0,800,800,NULL,NULL,GetModuleHandleW(NULL),NULL);if(!owner)return 3;
 CHECK("PDF reader starts",!pdfview_start(owner));MoveWindow(R.hwnd,0,0,640,800,FALSE);pdfview_set_memory(memory_file,0);
 test_docmarks_persistence_and_conflicts();test_pdf_outline_and_fallback_navigation();test_pdf_personal_bookmark_button_and_restore();test_pdf_rail_paint_and_input();pdfview_stop();DestroyWindow(owner);
 printf("%d passed, %d failed; 4 PDF navigation selectors\n",passed,failed);return failed?1:0;
}
