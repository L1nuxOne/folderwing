/* Real view + explorer controls on an owned private desktop. No physical input. */
#define COBJMACROS
#define wWinMain filter_unused_entrypoint
#include "../app/fpdemo.c"
#undef wWinMain
static int passed,failed;static wchar_t fixture[32768],child[32768];
#define CHECK(label,c) do{if(c){passed++;printf("ok %s\n",label);}else{failed++;printf("FAIL %d %s\n",__LINE__,label);}}while(0)
static void pump_filter(int ms){uint64_t end=GetTickCount64()+ms;do{MSG m;while(PeekMessageW(&m,NULL,0,0,PM_REMOVE))if(m.message!=WM_QUIT){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(2);}while(GetTickCount64()<end);}
static int await_path(Pane *p,const wchar_t *path){uint64_t end=GetTickCount64()+10000;while(p->nav_ticket&&GetTickCount64()<end)pump_filter(10);return p->path&&same_path(p->path,path)&&!p->nav_ticket;}
static void input(Pane *p,const wchar_t *query){SetWindowTextW(p->filter_edit,query);}
static void reset_filter(Pane *p){p->filter[0]=0;p->filter_len=0;p->filter_types=0;p->filter_hide_folders=0;p_filter_changed(p);}
static void test_footer_name_patterns(void){
 CHECK("plain filename fragments ignore case",pf_text_matches(L"Invoice-2026.PDF",L"invoice"));
 CHECK("extension query matches suffix only",pf_text_matches(L"Invoice.PDF",L".pdf")&&!pf_text_matches(L"Invoice.pdf.tmp",L".pdf"));
 CHECK("glob anchors both ends",pf_text_matches(L"report-final.CSV",L"report*.csv")&&!pf_text_matches(L"myreport.csv",L"report*.csv"));
 CHECK("semicolon alternatives and whitespace",pf_text_matches(L"holiday.JPG",L" *.png ; *.jpg "));
 CHECK("single-character and repeated-star patterns",pf_text_matches(L"ab.txt",L"a?*.txt")&&pf_text_matches(L"abc.txt",L"**a**.txt")&&!pf_text_matches(L"a.txt",L"a?.txt"));
 CHECK("Unicode case folding",pf_text_matches(L"ÉTUDE.txt",L"étude"));
 CHECK("empty alternatives match all",pf_text_matches(L"file",L" ; ; "));
 CHECK("extensionless classifications use existing registry",pf_group(ti_of_path(L"Makefile",0))==PF_TEXT&&pf_group(ti_of_path(L"unknown.blob",0))==PF_OTHER);
}
static void test_footer_view_filters_refresh_and_selection(void){
 Pane *p=&g.panes[0];reset_filter(p);uint32_t all=p_count(p);CHECK("hidden entries excluded from initial count",all==10&&fpe_view_unfiltered_count(p->view)==10);
 uint32_t image=fpe_view_find(p->view,L"holiday.JPG"),pdf=fpe_view_find(p->view,L"report.pdf");p_set_sel(p,image,1);p_set_sel(p,pdf,1);
 p->filter_types=1u<<PF_IMAGES;p_filter_changed(p);CHECK("image category retains folders",p_count(p)==3&&fpe_view_find(p->view,L"photos")!=UINT32_MAX);
 CHECK("hidden selection is retired",p->nsel==1&&p_is_sel(p,fpe_view_find(p->view,L"holiday.JPG")));
 wchar_t **targets=NULL;uint32_t count=p_targets(p,&targets,NULL);CHECK("file actions gather only visible selection",count==1&&wcsstr(targets[0],L"holiday.JPG"));free_targets(targets,count);
 input(p,L"holiday");CHECK("name and category intersect including folder names",p_count(p)==1&&fpe_view_unfiltered_count(p->view)==all);
 p->filter_types|=1u<<PF_VIDEO;p_filter_changed(p);CHECK("multi-category widening restores matching video",p_count(p)==2);
 input(p,L"*.pdf");CHECK("nonmatching filter empties list",!p_count(p));p->filter_types=0;p_filter_changed(p);CHECK("widening restores PDF",p_count(p)==1);
 p->opts.descending=1;apply_opts(p);CHECK("sort preserves active filter",p_count(p)==1);
 fpe_dir *next=NULL;CHECK("refresh fixture readable",!fpe_dir_read(fixture,&next));if(next){p_refresh_in_place(p,next);fpe_dir_release(next);}CHECK("snapshot refresh preserves filters",p_count(p)==1&&!wcscmp(p->filter,L"*.pdf"));
 reset_filter(p);CHECK("clear restores all eligible entries",p_count(p)==all);
 p->filter_hide_folders=1;p_filter_changed(p);CHECK("folder toggle excludes directories",p_count(p)==all-2);
 p->opts.show_hidden=1;apply_opts(p);CHECK("hidden toggle rebuilds eligible total",fpe_view_unfiltered_count(p->view)==all+1&&p_count(p)==all-1);p->opts.show_hidden=0;apply_opts(p);reset_filter(p);
}
static void test_footer_controls_keyboard_and_layout(void){
 Pane *p=&g.panes[0];CHECK("footer contains native edit and buttons",IsWindow(p->filter_edit)&&IsWindow(p->filter_type_button)&&IsWindow(p->filter_clear));
 input(p,L".pdf");CHECK("native edit updates the real view",p_count(p)==1);
 p->filter_types=1u<<PF_DOCUMENTS;p_filter_changed(p);SendMessageW(p->filter_clear,BM_CLICK,0,0);CHECK("clear button retains category",!p->filter_len&&p->filter_types&&p_count(p)==4);
 SendMessageW(p->filter_edit,WM_KEYDOWN,VK_ESCAPE,0);CHECK("Escape clears both controls",!p_filter_active(p)&&p_count(p)==10);
 SendMessageW(p->hwnd,WM_CHAR,6,0);CHECK("Ctrl+F character targets the footer instead of preview Find",GetFocus()==p->filter_edit||g.focus_save==p->filter_edit);
 SendMessageW(p->hwnd,WM_CHAR,L'h',0);CHECK("type-to-filter mirrors into input",p->filter_len==1&&GetWindowTextLengthW(p->filter_edit)==1);
 SendMessageW(p->filter_edit,WM_KEYDOWN,VK_RETURN,0);CHECK("Enter targets the list without stealing foreground",GetFocus()==p->hwnd||g.focus_save==p->hwnd);
 int oldw=p->cw,oldh=p->ch;MoveWindow(p->hwnd,0,0,190,500,FALSE);p_finish_geometry(p);
 HWND controls[]={p->filter_type_button,p->filter_edit,p->filter_clear};int last=0,valid=1;
 for(int i=0;i<3;i++){RECT r;GetWindowRect(controls[i],&r);MapWindowPoints(NULL,p->hwnd,(POINT*)&r,2);if(r.left<last||r.right>p->cw||r.top<p_list_bottom(p)||r.bottom>p->ch-g.status_h||r.right<=r.left)valid=0;last=r.right;}
 CHECK("narrow footer controls stay within their reserved row",valid);MoveWindow(p->hwnd,0,0,oldw,oldh,FALSE);p_finish_geometry(p);reset_filter(p);
}
static void test_footer_tab_and_navigation_lifecycle(void){
 Pane *p=&g.panes[0];input(p,L"holiday");p->filter_types=1u<<PF_IMAGES;p_filter_changed(p);tab_new(p,fixture,1);CHECK("new tab opens without inherited filters",await_path(p,fixture)&&!p_filter_active(p));
 input(p,L".pdf");tab_select(p,0);CHECK("original tab restores query and category",await_path(p,fixture)&&!wcscmp(p->filter,L"holiday")&&p->filter_types==(1u<<PF_IMAGES)&&p_count(p)==1);
 tab_select(p,1);CHECK("second tab restores its independent query",await_path(p,fixture)&&!wcscmp(p->filter,L".pdf")&&p_count(p)==1);
 p_navigate(p,fixture,NULL,0);CHECK("reopening the same folder retains the filter",await_path(p,fixture)&&!wcscmp(p->filter,L".pdf")&&p_count(p)==1);
 p_navigate(p,child,NULL,1);CHECK("navigation clears filters after folder commits",await_path(p,child)&&!p_filter_active(p)&&!GetWindowTextLengthW(p->filter_edit));
 p_navigate(p,fixture,NULL,1);CHECK("return navigation shows complete listing",await_path(p,fixture)&&p_count(p)==10);
}
static void capture_footer(void){
 wchar_t path[32768];if(!GetEnvironmentVariableW(L"FOLDERWING_FILTER_CAPTURE",path,32768))return;
 Pane *p=&g.panes[0];input(p,L"holiday");p->filter_types=(1u<<PF_IMAGES)|(1u<<PF_VIDEO);p_filter_changed(p);
 BITMAPINFO bi={.bmiHeader={sizeof(BITMAPINFOHEADER),p->cw,-p->ch,1,32,BI_RGB}};void *pixels=NULL;
 HDC dc=CreateCompatibleDC(NULL);HBITMAP bitmap=CreateDIBSection(NULL,&bi,DIB_RGB_COLORS,&pixels,NULL,0);if(!dc||!bitmap){if(dc)DeleteDC(dc);if(bitmap)DeleteObject(bitmap);return;}
 HGDIOBJ old=SelectObject(dc,bitmap);SetBkMode(dc,TRANSPARENT);p_paint(p,dc);
 HWND controls[]={p->filter_type_button,p->filter_edit,p->filter_clear};for(int i=0;i<3;i++){RECT r;GetWindowRect(controls[i],&r);MapWindowPoints(NULL,p->hwnd,(POINT*)&r,2);int saved=SaveDC(dc);SetViewportOrgEx(dc,r.left,r.top,NULL);SendMessageW(controls[i],WM_PRINT,(WPARAM)dc,PRF_CLIENT|PRF_NONCLIENT|PRF_ERASEBKGND);RestoreDC(dc,saved);}
 GdiFlush();HANDLE file=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);if(file!=INVALID_HANDLE_VALUE){DWORD size=p->cw*p->ch*4,written;BITMAPFILEHEADER h={.bfType=0x4d42,.bfSize=sizeof h+sizeof(BITMAPINFOHEADER)+size,.bfOffBits=sizeof h+sizeof(BITMAPINFOHEADER)};WriteFile(file,&h,sizeof h,&written,NULL);WriteFile(file,&bi.bmiHeader,sizeof(BITMAPINFOHEADER),&written,NULL);WriteFile(file,pixels,size,&written,NULL);CloseHandle(file);}SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
}
int wmain(void){
 setvbuf(stdout,NULL,_IONBF,0);wchar_t temp[32768];GetTempPathW(32768,temp);swprintf(fixture,32768,L"%lsFolderwingFooter-%lu-%llu",temp,GetCurrentProcessId(),(unsigned long long)GetTickCount64());if(!CreateDirectoryW(fixture,NULL))return 2;
 const wchar_t *files[]={L"holiday.JPG",L"holiday.mp4",L"report.pdf",L"report.csv",L"report.json",L"report.md",L"notes.txt",L"log.PDF.txt",L"hidden.pdf"};
 for(unsigned i=0;i<9;i++){wchar_t path[32768];swprintf(path,32768,L"%ls\\%ls",fixture,files[i]);HANDLE f=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_NEW,i==8?FILE_ATTRIBUTE_HIDDEN:0,NULL);if(f==INVALID_HANDLE_VALUE)return 3;CloseHandle(f);}swprintf(child,32768,L"%ls\\next",fixture);CreateDirectoryW(child,NULL);wchar_t photos[32768];swprintf(photos,32768,L"%ls\\photos",fixture);CreateDirectoryW(photos,NULL);
 HDESK original=GetThreadDesktop(GetCurrentThreadId());wchar_t deskname[100];swprintf(deskname,100,L"FolderwingFooter-%lu",GetCurrentProcessId());HDESK desk=CreateDesktopW(deskname,NULL,NULL,0,GENERIC_ALL,NULL);if(!desk||!SetThreadDesktop(desk))return 4;
 CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);fn_host host={host_now,host_arm,host_status};fn_init(&host);fn_register(app_functions,sizeof app_functions/sizeof *app_functions);functions_register();relay_register();palette_register();
 const char *config="[functions]\nsvc.shell=off\nsvc.icons=off\nsvc.thumbnails=off\nwsl.helper=off\npreview.pdf=off\npreview.image=off\npreview.text=off\npreview.media=off\npreview.archive=off\n";fn_settings(config,strlen(config));fn_configure_all();g.engine=fpe_engine_create(NULL);g.dual=1;g.ratio=.5;session_defaults(&g_ses);
 for(int i=0;i<2;i++){g.panes[i].idx=i;g.panes[i].ntabs=1;g.panes[i].drop_row=-1;g.panes[i].opts.dirs_first=1;}
 HINSTANCE instance=GetModuleHandleW(NULL);register_class(L"OpenPilot",main_proc,0,instance);register_class(L"OpenPilotPane",pane_proc,0,instance);register_class(L"OpenPilotTree",tree_proc,0,instance);register_class(L"OpenPilotDebug",dbg_proc,0,instance);
 HWND h=CreateWindowExW(0,L"OpenPilot",L"Owned footer filter acceptance",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,0,0,1250,820,NULL,NULL,instance,NULL);if(!h)return 5;
 for(int i=0;i<2;i++){fpe_dir *d=read_start(fixture);if(d){p_show_dir(&g.panes[i],d,NULL,0);fpe_dir_release(d);}}
 ShowWindow(h,SW_SHOWNOACTIVATE);UpdateWindow(h);test_footer_name_patterns();test_footer_view_filters_refresh_and_selection();test_footer_controls_keyboard_and_layout();test_footer_tab_and_navigation_lifecycle();capture_footer();
 DestroyWindow(h);pump_filter(50);settings_save_join();folderwatch_join();fpe_engine_destroy(g.engine);CoUninitialize();SetThreadDesktop(original);CloseDesktop(desk);
 for(unsigned i=0;i<9;i++){wchar_t path[32768];swprintf(path,32768,L"%ls\\%ls",fixture,files[i]);SetFileAttributesW(path,FILE_ATTRIBUTE_NORMAL);DeleteFileW(path);}RemoveDirectoryW(child);RemoveDirectoryW(photos);RemoveDirectoryW(fixture);
 printf("%d passed, %d failed; 4 footer selectors\n",passed,failed);return failed?1:0;
}
