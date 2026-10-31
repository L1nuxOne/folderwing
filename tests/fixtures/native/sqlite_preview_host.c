/* Real Folderwing routing with owned off-screen windows, a synthetic ZIP and no input. */
#define COBJMACROS
#define wWinMain sqlite_unused_entrypoint
#include "../../../app/fpdemo.c"
#undef wWinMain
#define set_status archive_ui_status
#include "../../../app/archiveview.c"
#undef set_status
static int failed;
#define CHECK(s,c) do { if(c) printf("ok %s\n",s); else {printf("FAIL %s\n",s);failed++;} } while(0)
static void pump(int ms){uint64_t until=GetTickCount64()+ms;do{MSG m;while(PeekMessageW(&m,NULL,0,0,PM_REMOVE)){if(m.message!=WM_QUIT){TranslateMessage(&m);DispatchMessageW(&m);}}Sleep(2);}while(GetTickCount64()<until);}
static int idle(void){uint64_t until=GetTickCount64()+15000;while(A.busy&&GetTickCount64()<until)pump(10);return !A.busy;}
typedef struct {const wchar_t *needle;int found;} lookup;
static BOOL CALLBACK label(HWND h,LPARAM l){lookup *p=(lookup*)l;wchar_t text[1000];GetWindowTextW(h,text,1000);if(wcsstr(text,p->needle)){p->found=1;return FALSE;}return TRUE;}
static int has_label(HWND root,const wchar_t *text){lookup p={text,0};EnumChildWindows(root,label,(LPARAM)&p);return p.found;}
static int wait_label(HWND root,const wchar_t *text){uint64_t until=GetTickCount64()+15000;while(!has_label(root,text)&&GetTickCount64()<until)pump(10);return has_label(root,text);}
static int choose(const wchar_t *name){int row=-1;for(uint32_t i=0;i<A.count;i++)if(!wcscmp(A.rows[i].name,name)){row=(int)i;break;}if(row<0)return 0;select_row(row);A.due=GetTickCount64();archiveview_tick();if(!idle())return 0;pump(30);return g.ar_kind==3;}
int wmain(int argc,wchar_t **argv)
{
 if(argc!=2)return 2;setvbuf(stdout,NULL,_IONBF,0);SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
 fn_host host={host_now,host_arm,host_status};fn_init(&host);fn_register(app_functions,sizeof app_functions/sizeof *app_functions);
 static const fn_def ops={.id="operations.show",.title=L"Operations",.life=FN_COLD,.where=FN_ANYWHERE,.opts=operations_opts,.run=run_operations_show};fn_register(&ops,1);functions_register();relay_register();palette_register();
 const char *config="[functions]\nsvc.shell = off\nsvc.icons = off\n[panel.side]\nslide = off\n";fn_settings(config,strlen(config));fn_configure_all();g.engine=fpe_engine_create(NULL);g.dual=1;g.ratio=0.4;g.show_tree=0;session_defaults(&g_ses);
 for(int i=0;i<2;i++){Pane *p=&g.panes[i];p->idx=i;p->ntabs=1;p->drop_row=-1;p->opts.dirs_first=1;}
 HINSTANCE inst=GetModuleHandleW(NULL);register_class(L"OpenPilot",main_proc,0,inst);register_class(L"OpenPilotPane",pane_proc,CS_DBLCLKS,inst);register_class(L"OpenPilotTree",tree_proc,CS_DBLCLKS,inst);register_class(L"OpenPilotDebug",dbg_proc,0,inst);
 HWND window=CreateWindowExW(0,L"OpenPilot",L"SQLite preview acceptance",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,-32000,-32000,1300,760,NULL,NULL,inst,NULL);if(!window)return 3;
 for(int i=0;i<2;i++){fpe_dir *dir=read_start(argv[1]);if(dir){p_show_dir(&g.panes[i],dir,NULL,0);fpe_dir_release(dir);}}
 ShowWindow(window,SW_SHOWNOACTIVATE);UpdateWindow(window);pump(30);
 Pane *p=&g.panes[0];uint32_t row=fpe_view_find(p->view,L"sample.sqlite3");CHECK("SQLite database is in the folder",row!=UINT32_MAX);
 if(row!=UINT32_MAX){p_clear_sel(p);p->cursor=p->anchor=row;p_set_sel(p,row,1);preview_note(p);KillTimer(g.hwnd,TIMER_PREVIEW);preview_show();CHECK("ordinary database uses the preview pane",g.pv_shown==3);CHECK("ordinary database helper shows rows",wait_label(textview_window(),L"Page 1 · 2 rows"));fn_ctx c;app_preview_ctx(&c,textview_window(),FN_FROM_MENU);CHECK("text-only commands do not appear in a database preview",!fn_applies(fn_find("text.find"),&c));app_ctx_free(&c);}
 preview_hide();row=fpe_view_find(p->view,L"sqlite-preview.zip");CHECK("ZIP fixture is in the folder",row!=UINT32_MAX);
 if(row!=UINT32_MAX){p_clear_sel(p);p->cursor=p->anchor=row;p_set_sel(p,row,1);preview_note(p);KillTimer(g.hwnd,TIMER_PREVIEW);preview_show();CHECK("archive replaces the opposite pane",g.pv_shown==4&&idle());
 CHECK("SQLite archive entry opens in the third pane",choose(L"sample.sqlite3"));CHECK("archive helper shows database rows",wait_label(textview_window(),L"Page 1 · 2 rows"));
 CHECK("WAL entry uses database preview",choose(L"wal.sqlite"));CHECK("WAL entry explains missing companion files",wait_label(textview_window(),L"archive entry uses SQLite WAL mode"));}
 preview_hide();DestroyWindow(window);pump(100);settings_save_join();folderwatch_join();fpe_engine_destroy(g.engine);archive_sources_finish();CoUninitialize();printf("%d SQLite host failures\n",failed);return failed?1:0;
}
