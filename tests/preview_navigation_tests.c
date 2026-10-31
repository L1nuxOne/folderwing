/* Real native text snapshots and shared storage; owned hidden windows only. */
#include "../app/textview.c"
#include <stdio.h>
static int passed,failed;static wchar_t fixtures[32768],memory_file[32768];static HWND owner;
#define CHECK(label,condition) do{if(condition){passed++;printf("ok %s\n",label);}else{failed++;printf("FAIL %d %s\n",__LINE__,label);}}while(0)
HFONT app_font(void){return GetStockObject(DEFAULT_GUI_FONT);}int app_px(int n){return n;}
HWND app_window(void){return owner;}int app_menu_context_current(const fn_ctx *c){(void)c;return 1;}
uint32_t fn_menu(HMENU m,uint32_t menu,const fn_ctx *c){(void)m;(void)menu;(void)c;return 0;}int fn_menu_run(UINT id,fn_ctx *c){(void)id;(void)c;return 0;}UINT menu_track(HMENU m,HWND owner,POINT p){(void)m;(void)owner;(void)p;return 0;}
void app_focus_window(HWND h){(void)h;}void app_focus_cycle(int n){(void)n;}void app_preview_close(void){textview_clear();}
void app_preview_ctx(fn_ctx *c,HWND h,int from){memset(c,0,sizeof *c);c->from=from;(void)h;}void app_ctx_free(fn_ctx *c){(void)c;}
int fn_key(uint8_t key,uint8_t mods,fn_ctx *c){(void)key;(void)mods;(void)c;return 0;}
void app_status(Pane *p,int error,const wchar_t *format,...){(void)p;(void)error;(void)format;}
uint64_t app_clipboard_begin(HWND h){(void)h;return 1;}void app_clipboard_cancel(HWND h){(void)h;}
void app_clipboard_submit(uint64_t request,UINT format,HGLOBAL data,UINT extra_format,HGLOBAL extra,const wchar_t *success){(void)request;(void)format;(void)extra_format;(void)success;if(data)GlobalFree(data);if(extra)GlobalFree(extra);}
static void pump_nav(int ms){uint64_t end=GetTickCount64()+ms;do{MSG m;while(PeekMessageW(&m,NULL,0,0,PM_REMOVE))if(m.message!=WM_QUIT){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(2);}while(GetTickCount64()<end);}
static int open_nav(const wchar_t *name,const wchar_t *identity){wchar_t path[32768];swprintf(path,32768,L"%ls\\%ls",fixtures,name);if(identity)textview_open_archive_key(path,0,0,identity);else textview_open(path,0,0);uint64_t end=GetTickCount64()+15000;while(!R.loaded&&!R.err&&!R.gone&&GetTickCount64()<end)pump_nav(10);pump_nav(100);printf("loaded=%d rows=%u chars=%u error=%lx\n",R.loaded,R.info.rows,R.info.chars,R.err);return R.loaded&&!R.err;}
static void test_source_rail_positions_and_reflow(void){
 CHECK("source reader opens",open_nav(L"large.txt",NULL));textnav_build();CHECK("source fallback uses 5 percent ticks",R.nav_targets_count==21);CHECK("native vertical scrollbar replaced",!(GetWindowLongW(R.hwnd,GWL_STYLE)&WS_VSCROLL));
 R.top=textnav_range()*3/4;view_changed();uint32_t offset=row_at(R.top).start;CHECK("fixture exceeds legacy PDF page bounds",offset>100000);textnav_toggle();CHECK("source bookmark records character offset",R.nav_count==1&&R.nav_marks[0].page==offset&&!R.rail.error);
 textnav_build();R.top=0;textnav_jump(21);CHECK("source bookmark restores its line",row_at(R.top).start==offset);
 MoveWindow(R.hwnd,0,0,350,500,FALSE);R.restore_at=offset;R.want_layout=1;request_work();uint64_t end=GetTickCount64()+15000;while((R.want_layout||R.banks[0].busy||R.banks[1].busy)&&GetTickCount64()<end)pump_nav(10);textnav_build();R.top=0;textnav_jump(21);CHECK("reflow retains the bookmarked character",row_at(R.top).start<=offset&&row_at(R.top).end>=offset);
 textnav_toggle();CHECK("toggle removes mark after reflow",R.nav_count==0);
 SendMessageW(R.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(R.cw-8,rr_first(&R.rail)));SendMessageW(R.hwnd,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(R.cw-8,rr_last(&R.rail)));SendMessageW(R.hwnd,WM_LBUTTONUP,0,MAKELPARAM(R.cw-8,rr_last(&R.rail)));CHECK("rail scrubs without selecting text",R.top==textnav_range()&&!R.rail.drag&&!R.selecting);
 HDC dc=GetDC(R.hwnd);HDC mem=CreateCompatibleDC(dc);HBITMAP bmp=CreateCompatibleBitmap(dc,R.cw,R.ch);HGDIOBJ old=SelectObject(mem,bmp);paint(mem);CHECK("rail background is painted",GetPixel(mem,R.cw-26,head_h()+2)==C_TREE_BG);SelectObject(mem,old);DeleteObject(bmp);DeleteDC(mem);ReleaseDC(R.hwnd,dc);
}
static void test_source_archive_identity_and_lifecycle(void){
 const wchar_t *key=L"C:\\owned.zip\x1f" L"0041002E007400780074";CHECK("archive source opens",open_nav(L"archive-a.txt",key));R.top=textnav_range()/2;view_changed();textnav_toggle();uint32_t offset=R.nav_marks[0].page;CHECK("archive source saves stable identity",R.nav_count==1&&!R.rail.error);
 CHECK("new extraction path opens",open_nav(L"archive-b.txt",key));textnav_build();CHECK("new staging path reuses mark",R.nav_count==1&&R.nav_marks[0].page==offset);
 textview_configure(3,1,1,TEXT_1252);uint64_t end=GetTickCount64()+15000;while(!R.loaded&&!R.err&&GetTickCount64()<end)pump_nav(10);textnav_build();CHECK("encoding reload preserves archive identity",R.nav_count==1&&!wcscmp(R.nav_identity,key));
 CHECK("case distinct member opens",open_nav(L"archive-a.txt",L"C:\\owned.zip\x1f" L"0061002E007400780074"));textnav_build();CHECK("archive member case remains distinct",R.nav_count==0);
 textnav_build();SendMessageW(R.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(R.cw-8,rr_first(&R.rail)));CHECK("rail owns capture during drag",GetCapture()==R.hwnd);textview_clear();CHECK("clearing releases capture and old context",GetCapture()!=R.hwnd&&!R.nav_identity&&!R.nav_key&&!R.nav_count&&!R.rail.drag);
}
static void test_managed_native_storage_compatibility(void){
 wchar_t path[32768];swprintf(path,32768,L"%ls\\interop.fwb",fixtures);wchar_t *key=docmarks_identity(L"C:\\interop.md",L"markdown");docmark marks[4];uint32_t count=0;CHECK("native reads managed checksummed store",!docmarks_load(path,key,marks,4,&count)&&count==1&&marks[0].page==700000&&!wcscmp(marks[0].title,L"Équations 日本語"));docmark mark={.page=1234567,.y=.75f};wcscpy(mark.title,L"Native mark");CHECK("native appends without replacing managed mark",!docmarks_change(path,key,&mark,0));free(key);
}
int wmain(int argc,wchar_t **argv){setvbuf(stdout,NULL,_IONBF,0);if(argc!=2)return 2;wcsncpy(fixtures,argv[1],32767);swprintf(memory_file,32768,L"%ls\\session.json",fixtures);owner=CreateWindowExW(0,L"STATIC",L"Owned source rail test",WS_OVERLAPPEDWINDOW,0,0,800,600,NULL,NULL,GetModuleHandleW(NULL),NULL);if(!owner)return 3;CHECK("reader starts",!textview_start(owner));MoveWindow(R.hwnd,0,0,650,500,FALSE);textview_set_bookmark_memory(memory_file);test_source_rail_positions_and_reflow();test_source_archive_identity_and_lifecycle();test_managed_native_storage_compatibility();textview_stop();DestroyWindow(owner);printf("%d passed, %d failed; 3 source/navigation selectors\n",passed,failed);return failed?1:0;}
