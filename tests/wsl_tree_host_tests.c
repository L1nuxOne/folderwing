/* Actual main tree/registry parser with an owned hive and real native navigation. */
#include <windows.h>
static HKEY registry_fixture;static int registry_present=1;static unsigned registry_reads,unc_probes;
static LSTATUS WINAPI tree_fixture_open(HKEY,LPCWSTR,DWORD,REGSAM,PHKEY);
static DWORD WINAPI tree_fixture_attributes(LPCWSTR);
#define RegOpenKeyExW tree_fixture_open
#define GetFileAttributesW tree_fixture_attributes
#define COBJMACROS
#define wWinMain wsl_tree_unused_entrypoint
#include "../app/fpdemo.c"
#undef wWinMain
#undef RegOpenKeyExW
#undef GetFileAttributesW
static LSTATUS WINAPI tree_fixture_open(HKEY key,LPCWSTR sub,DWORD options,REGSAM access,PHKEY out){if(key==HKEY_CURRENT_USER&&sub&&!_wcsicmp(sub,L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss")){registry_reads++;return registry_present?RegOpenKeyExW(registry_fixture,L"",0,access,out):ERROR_FILE_NOT_FOUND;}return RegOpenKeyExW(key,sub,options,access,out);}
static DWORD WINAPI tree_fixture_attributes(LPCWSTR path){if(path&&!_wcsnicmp(path,L"\\\\wsl",5))unc_probes++;return GetFileAttributesW(path);}
static int passed,failed;static wchar_t left[32768],native[32768],distro_name[128];
#define CHECK(label,c) do{if(c){passed++;printf("ok %s\n",label);}else{failed++;printf("FAIL %d %s\n",__LINE__,label);}}while(0)
static void pump_tree(int ms){uint64_t end=GetTickCount64()+ms;do{MSG m;while(PeekMessageW(&m,NULL,0,0,PM_REMOVE))if(m.message!=WM_QUIT){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(2);}while(GetTickCount64()<end);}
static int tree_settled(void){uint64_t end=GetTickCount64()+15000;do{pump_tree(5);if(!g.panes[0].nav_ticket&&!g.panes[0].refresh_ticket&&!T.npend&&!T.reveal)return 1;}while(GetTickCount64()<end);return 0;}
static void tree_settings(int on){char s[256];snprintf(s,sizeof s,"[functions]\nwsl.helper=%s\nsvc.shell=off\nsvc.icons=off\npreview.pdf=off\npreview.image=off\npreview.text=off\npreview.media=off\npreview.archive=off\n",on?"on":"off");fn_settings(s,strlen(s));fn_configure_all();}
static tnode *linux_root(void){for(uint32_t i=0;i<T.nroots;i++)if(T.roots[i]->kind==NODE_LINUX)return T.roots[i];return NULL;}
static tnode *native_root(void){tnode *n=linux_root();for(uint32_t i=0;n&&i<n->nkids;i++)if(!wcscmp(n->kids[i]->label,distro_name))return n->kids[i];return NULL;}
static void distro_record(const wchar_t *key_name,const wchar_t *name,DWORD version,DWORD type){HKEY key;RegCreateKeyExW(registry_fixture,key_name,0,NULL,0,KEY_WRITE,NULL,&key,NULL);RegSetValueExW(key,L"DistributionName",0,REG_SZ,(const BYTE*)name,(DWORD)((wcslen(name)+1)*2));RegSetValueExW(key,L"Version",0,type,(const BYTE*)&version,sizeof version);RegCloseKey(key);}
static unsigned places_count;static int places_valid;
static void tree_place_observe(void *user,const wchar_t *path,const wchar_t *label,int kind){(void)user;(void)label;if(!path||!*path)places_valid=0;if(path&&!_wcsnicmp(path,L"\\\\wsl.localhost\\",16)&&kind==APP_PLACE_ROOT)places_count++;}
static void test_wsl_tree_discovery_is_cold_and_default_expanded(void){
 fpe_wsl_stats before,after;fpe_wsl_get_stats(&before);tree_settings(0);unsigned reads=registry_reads;tree_init_roots();CHECK("disabled WSL leaves ordinary drives and does not enumerate distros",!linux_root()&&T.nroots&&registry_reads==reads);
 registry_present=0;tree_settings(1);CHECK("absent WSL registration leaves no empty Linux group",!linux_root());registry_present=1;unc_probes=0;tree_settings(1);tnode *group=linux_root(),*child=native_root();fpe_wsl_get_stats(&after);
 CHECK("Linux appears beside drives with installed distros already visible",group&&group->expanded&&group->loaded&&group->nkids==2&&tree_row_of(group)>=0&&child&&tree_row_of(child)==tree_row_of(group)+2);
 CHECK("WSL1 and WSL2 entries are sorted and invalid/duplicate registrations omitted",group&&!wcscmp(group->kids[0]->label,L"AA-Fixture")&&!wcscmp(group->kids[1]->label,distro_name));
 CHECK("discovery does not probe UNC paths, start helpers or list distro folders",!unc_probes&&after.helpers==before.helpers&&!T.npend&&child&&!child->loaded&&!child->expanded&&child->depth==1);
 if(!group||!child)return;unsigned rows=T.nrows;tree_toggle(group);tree_toggle(group);CHECK("group collapse/expand reuses catalog without filesystem loads",T.nrows==rows&&!T.npend&&!unc_probes&&group->expanded);
 group->expanded=0;tree_rebuild();tree_settings(1);CHECK("unchanged settings preserve the user-collapsed catalog",linux_root()==group&&!group->expanded);tree_toggle(group);
 CHECK("Linux and distro icons stay local even with Windows icons selected",tree_icon(group)==-1&&tree_icon(child)==-1&&!unc_probes&&group->ti==TI_LINUX);
}
static void test_wsl_tree_group_scope_and_policy_retirement(void){
 tnode *group=linux_root(),*child=native_root();if(!group||!child){CHECK("group fixture available",0);return;}fn_ctx c;tree_ctx(&c,group,FN_FROM_MENU);CHECK("virtual heading has no file, tab or mutation target",!c.n&&!c.dir&&!c.dest&&!c.paths&&!(c.state&FN_S_FOLDER)&&!fn_applies(fn_find("file.delete"),&c));ctx_free(&c);
 tree_ctx(&c,child,FN_FROM_MENU);CHECK("distro retains real root protection and exact navigation identity",c.n==1&&c.dirs==1&&(c.state&FN_S_ROOT)&&same_path(c.dir,child->path));ctx_free(&c);
 uint64_t ticket=g.panes[0].nav_ticket;tree_open(group);tree_request(group);CHECK("heading activation cannot issue navigation or enumeration",g.panes[0].nav_ticket==ticket&&!T.npend);
 tree_ensure_visible(group);POINT point={g.pad+40,g.pad+(tree_row_of(group)-T.top)*g.row_h+g.row_h/2};const wchar_t *paths[]={left};CHECK("virtual heading refuses drag-over and committed drops",tree_drop_over(NULL,point,0,DROPEFFECT_COPY,paths,1)==DROPEFFECT_NONE&&tree_drop(NULL,point,DROPEFFECT_COPY,0,DROPEFFECT_COPY,paths,1)==DROPEFFECT_NONE);T.top=0;
 places_count=0;places_valid=1;app_places(tree_place_observe,NULL);CHECK("places list exports distro roots and no virtual empty path",places_valid&&places_count==2);
 T.pend=realloc(T.pend,(T.npend+1)*sizeof *T.pend);T.pend_cap=T.npend+1;T.pend[T.npend++]=(tpend){.ticket=0x123456789ull,.path=wdup(child->path)};T.current=T.cursor=T.down=T.drop=child;tree_settings(0);
 CHECK("disabling WSL removes subtree and retires all node references and pending intake",!linux_root()&&!T.npend&&!T.current&&!T.cursor&&!T.down&&!T.drop);tree_settings(1);load_msg late={.ticket=0x123456789ull};CHECK("late prior-tree completion cannot attach after re-enable",linux_root()&&linux_root()->expanded&&!tree_handle_loaded(&late)&&!native_root()->loaded);
}
static void test_wsl_tree_real_native_navigation_and_reveal(void){
 tnode *child=native_root();CHECK("registered native child available",child!=NULL);if(!child)return;tree_open(child);CHECK("clicking distro navigates to its real filesystem root",tree_settled()&&g.panes[0].view&&tree_find(g.panes[0].path)==child&&T.current==child);
 tree_toggle(child);CHECK("distro expansion loads directories on demand",tree_settled()&&child->loaded&&child->expanded&&child->nkids>0);
 p_navigate(&g.panes[0],native,NULL,1);CHECK("native descendant loads through normal pane navigation and reveals ancestry",tree_settled()&&same_path(g.panes[0].path,native)&&T.current&&same_path(T.current->path,native)&&linux_root()->expanded);
 fpe_wsl_stats stats;uint64_t end=GetTickCount64()+10000;do{fpe_wsl_get_stats(&stats);if(stats.helpers)break;pump_tree(5);}while(GetTickCount64()<end);p_refresh(&g.panes[0]);CHECK("ready Linux helper supplies the existing native provider",tree_settled()&&g.panes[0].view&&fpe_dir_is_linux(fpe_view_dir(g.panes[0].view)));
 wchar_t alias[32768];swprintf(alias,32768,L"\\\\wsl$\\%ls",native+16);tnode *exact=tree_find(native);CHECK("legacy UNC alias resolves the same tree node",tree_find(alias)==exact);tree_toggle(linux_root());tree_reveal(alias);CHECK("revealing a legacy path reopens the Linux ancestor",tree_settled()&&linux_root()->expanded&&T.current==exact);
 wchar_t lower[32768],upper[32768];swprintf(lower,32768,L"%ls\\child",native);swprintf(upper,32768,L"%ls\\Child",native);tnode *parent=tree_find(native);if(parent&&!parent->expanded)tree_toggle(parent);CHECK("owned child directories load",tree_settled()&&tree_find(lower)&&tree_find(upper)&&tree_find(lower)!=tree_find(upper));tree_reveal(upper);CHECK("Linux path matching preserves case-sensitive directory identity",tree_settled()&&T.current==tree_find(upper));
 p_navigate(&g.panes[0],left,NULL,1);CHECK("ordinary Windows navigation still restores its tree selection",tree_settled()&&same_path(g.panes[0].path,left));
}
int wmain(int argc,wchar_t **argv){setvbuf(stdout,NULL,_IONBF,0);if(argc!=4)return 2;wcscpy(left,argv[1]);DWORD n=GetEnvironmentVariableW(L"OPENPILOT_TREE_WSL",native,32768);wchar_t linux[32768];if(!n||!term_wsl_split(native,distro_name,128,linux,32768))return 2;if(RegOpenKeyExW(HKEY_CURRENT_USER,argv[3],0,KEY_ALL_ACCESS,&registry_fixture))return 2;
 distro_record(L"001",distro_name,2,REG_DWORD);distro_record(L"002",L"AA-Fixture",1,REG_DWORD);distro_record(L"003",L"..",2,REG_DWORD);distro_record(L"004",L"bad\\name",2,REG_DWORD);distro_record(L"005",distro_name,2,REG_DWORD);distro_record(L"006",L"UnknownVersion",3,REG_DWORD);distro_record(L"007",L"BadType",2,REG_BINARY);
 CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);fn_host host={host_now,host_arm,host_status};fn_init(&host);fn_register(app_functions,sizeof app_functions/sizeof *app_functions);functions_register();relay_register();palette_register();tree_settings(0);g.engine=fpe_engine_create(NULL);g.dual=g.show_tree=1;g.ratio=.5;session_defaults(&g_ses);for(int i=0;i<2;i++){g.panes[i].idx=i;g.panes[i].ntabs=1;g.panes[i].drop_row=-1;g.panes[i].opts.dirs_first=1;}HINSTANCE inst=GetModuleHandleW(NULL);register_class(L"OpenPilot",main_proc,0,inst);register_class(L"OpenPilotPane",pane_proc,CS_DBLCLKS,inst);register_class(L"OpenPilotTree",tree_proc,CS_DBLCLKS,inst);register_class(L"OpenPilotDebug",dbg_proc,0,inst);HWND h=CreateWindowExW(0,L"OpenPilot",L"Owned Linux tree acceptance",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,60,60,1250,820,NULL,NULL,inst,NULL);if(!h)return 3;
 for(int i=0;i<2;i++){fpe_dir *d=read_start(i?argv[2]:left);if(d){p_show_dir(&g.panes[i],d,NULL,0);fpe_dir_release(d);}}ShowWindow(h,SW_SHOWNOACTIVATE);UpdateWindow(h);pump_tree(30);test_wsl_tree_discovery_is_cold_and_default_expanded();test_wsl_tree_group_scope_and_policy_retirement();test_wsl_tree_real_native_navigation_and_reveal();DestroyWindow(h);pump_tree(100);settings_save_join();folderwatch_join();fpe_engine_destroy(g.engine);for(uint32_t i=0;i<T.nroots;i++)tree_free_node(T.roots[i]);free(T.roots);free(T.rows);for(uint32_t i=0;i<T.npend;i++)free(T.pend[i].path);free(T.pend);free(T.reveal);RegCloseKey(registry_fixture);CoUninitialize();printf("%d passed, %d failed; 3 WSL tree host selectors\n",passed,failed);return failed?1:0;}
