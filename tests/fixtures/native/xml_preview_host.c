/* Real Folderwing routing and preview mode switching in owned off-screen windows. */
#define COBJMACROS
#define wWinMain xml_unused_entrypoint
#include "../../../app/fpdemo.c"
#undef wWinMain
#define set_status archive_ui_status
#include "../../../app/archiveview.c"
#undef set_status
static int failed;
#define CHECK(s,c) do { if(c) printf("ok %s\n",s); else {printf("FAIL %s\n",s);failed++;} } while(0)
static void pump(int ms){uint64_t until=GetTickCount64()+ms;do{MSG m;while(PeekMessageW(&m,NULL,0,0,PM_REMOVE)){if(m.message!=WM_QUIT){TranslateMessage(&m);DispatchMessageW(&m);}}Sleep(2);}while(GetTickCount64()<until);}
static int idle(void){uint64_t until=GetTickCount64()+15000;while(A.busy&&GetTickCount64()<until)pump(10);return !A.busy;}
typedef struct {HWND parent, child;} child_query;
static BOOL CALLBACK find_child(HWND h,LPARAM l){child_query *q=(child_query*)l;DWORD pid=0;GetWindowThreadProcessId(h,&pid);if(GetParent(h)==q->parent && pid && pid!=GetCurrentProcessId()){q->child=h;return FALSE;}return TRUE;}
static HWND hosted(void){child_query q={textview_window(),NULL};EnumChildWindows(q.parent,find_child,(LPARAM)&q);return q.child;}
static HWND wait_hosted(void){uint64_t until=GetTickCount64()+20000;HWND h=NULL;while(GetTickCount64()<until){h=hosted();if(h && IsWindowVisible(h)){RECT child,view;GetWindowRect(h,&child);MapWindowPoints(NULL,textview_window(),(POINT*)&child,2);GetClientRect(textview_window(),&view);if(child.left==0 && child.top>=app_px(48) && child.right>=view.right-2)return h;}pump(10);}return NULL;}
static int wait_visible(HWND child,int want){uint64_t until=GetTickCount64()+4000;while(GetTickCount64()<until){if(!!IsWindowVisible(child)==!!want)return 1;pump(10);}return !!IsWindowVisible(child)==!!want;}
static int source_ready(void){uint64_t until=GetTickCount64()+15000;wchar_t title[512];do{GetWindowTextW(textview_window(),title,512);if(wcsstr(title,L" lines |"))return 1;pump(10);}while(GetTickCount64()<until);return 0;}
static void toggle(int rendered){RECT rc;GetClientRect(textview_window(),&rc);int x=rc.right-app_px(rendered?120:40);SendMessageW(textview_window(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,app_px(15)));pump(100);}
static int choose(const wchar_t *name){int row=-1;for(uint32_t i=0;i<A.count;i++)if(!wcscmp(A.rows[i].name,name)){row=(int)i;break;}if(row<0)return 0;select_row(row);A.due=GetTickCount64();archiveview_tick();if(!idle())return 0;pump(30);return g.ar_kind==3;}
int wmain(int argc,wchar_t **argv)
{
 if(argc!=2)return 2;setvbuf(stdout,NULL,_IONBF,0);SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
 fn_host host={host_now,host_arm,host_status};fn_init(&host);fn_register(app_functions,sizeof app_functions/sizeof *app_functions);
 static const fn_def ops={.id="operations.show",.title=L"Operations",.life=FN_COLD,.where=FN_ANYWHERE,.opts=operations_opts,.run=run_operations_show};fn_register(&ops,1);functions_register();relay_register();palette_register();
 const char *config="[functions]\nsvc.shell = off\nsvc.icons = off\n[panel.side]\nslide = off\n";fn_settings(config,strlen(config));fn_configure_all();g.engine=fpe_engine_create(NULL);g.dual=1;g.ratio=0.4;g.show_tree=0;session_defaults(&g_ses);
 for(int i=0;i<2;i++){Pane *p=&g.panes[i];p->idx=i;p->ntabs=1;p->drop_row=-1;p->opts.dirs_first=1;}
 HINSTANCE inst=GetModuleHandleW(NULL);register_class(L"OpenPilot",main_proc,0,inst);register_class(L"OpenPilotPane",pane_proc,CS_DBLCLKS,inst);register_class(L"OpenPilotTree",tree_proc,CS_DBLCLKS,inst);register_class(L"OpenPilotDebug",dbg_proc,0,inst);
 HWND window=CreateWindowExW(0,L"OpenPilot",L"XML preview acceptance",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,-32000,-32000,1300,760,NULL,NULL,inst,NULL);if(!window)return 3;
 for(int i=0;i<2;i++){fpe_dir *dir=read_start(argv[1]);if(dir){p_show_dir(&g.panes[i],dir,NULL,0);fpe_dir_release(dir);}}
 ShowWindow(window,SW_SHOWNOACTIVATE);UpdateWindow(window);pump(30);
 CHECK("XML-related extensions route to archive text preview",app_archive_preview_kind(L"x.xml")==3 && app_archive_preview_kind(L"x.xaml")==3 && app_archive_preview_kind(L"x.rss")==3 && app_archive_preview_kind(L"x.atom")==3);
 Pane *p=&g.panes[0];uint32_t row=fpe_view_find(p->view,L"sample.xml");CHECK("XML file is in the folder",row!=UINT32_MAX);
 if(row!=UINT32_MAX){p_clear_sel(p);p->cursor=p->anchor=row;p_set_sel(p,row,1);preview_note(p);KillTimer(g.hwnd,TIMER_PREVIEW);preview_show();CHECK("ordinary XML uses the preview pane",g.pv_shown==3);HWND child=wait_hosted();CHECK("ordinary XML displays a separate structured helper",child && IsWindowVisible(child));CHECK("ordinary XML source is ready",source_ready());if(child){toggle(0);CHECK("ordinary XML switches to Source",wait_visible(child,0));toggle(1);CHECK("ordinary XML returns to Structured",wait_visible(child,1));}}
 preview_hide();row=fpe_view_find(p->view,L"xml-preview.zip");CHECK("ZIP fixture is in the folder",row!=UINT32_MAX);
 if(row!=UINT32_MAX){p_clear_sel(p);p->cursor=p->anchor=row;p_set_sel(p,row,1);preview_note(p);KillTimer(g.hwnd,TIMER_PREVIEW);preview_show();CHECK("archive replaces the opposite pane",g.pv_shown==4&&idle());CHECK("XML archive entry opens in the third pane",choose(L"sample.xml"));HWND child=wait_hosted();CHECK("archive XML displays a separate structured helper",child && IsWindowVisible(child));CHECK("archive XML source is ready",source_ready());if(child){toggle(0);CHECK("archive XML switches to Source",wait_visible(child,0));toggle(1);CHECK("archive XML returns to Structured",wait_visible(child,1));pump(500);CHECK("archive XML remains in Structured",IsWindowVisible(child));}}
 preview_hide();DestroyWindow(window);pump(100);settings_save_join();folderwatch_join();fpe_engine_destroy(g.engine);archive_sources_finish();CoUninitialize();printf("%d XML host failures\n",failed);return failed?1:0;
}
