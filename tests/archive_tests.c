/* Scoped native acceptance: actual host, archive window, restricted helpers and preview routing.
 * Only new owned windows/messages; no desktop input or external clipboard writes. */
#define COBJMACROS
#define wWinMain archive_unused_entrypoint
#include "../app/fpdemo.c"
#undef wWinMain
#define set_status archive_ui_status
#include "../app/archiveview.c"
#undef set_status
int archive_test_image_ready(void);
static int passed,failed;
static wchar_t fixture[ARCP_NAME];
#define CHECK(label,condition) do{if(condition){passed++;printf("ok %s\n",label);}else{failed++;printf("FAIL line %d: %s\n",__LINE__,label);}}while(0)
static void pump(int ms){uint64_t until=GetTickCount64()+ms;do{MSG msg;while(PeekMessageW(&msg,NULL,0,0,PM_REMOVE)){if(msg.message!=WM_QUIT){TranslateMessage(&msg);DispatchMessageW(&msg);}}Sleep(2);}while(GetTickCount64()<until);}
static int idle(void){uint64_t until=GetTickCount64()+15000;while(A.busy&&GetTickCount64()<until)pump(5);return !A.busy;}
static int row_named(const wchar_t *name){for(uint32_t i=0;i<A.count;i++)if(!wcscmp(A.rows[i].name,name))return (int)i;return -1;}
static int open_archive(const wchar_t *name,int source){
 preview_hide();set_active(source);Pane *p=&g.panes[source];uint32_t row=fpe_view_find(p->view,name);CHECK("archive fixture exists in the real pane",row!=UINT32_MAX);if(row==UINT32_MAX)return 0;
 p_clear_sel(p);p->cursor=p->anchor=row;p_set_sel(p,row,1);preview_note(p);KillTimer(g.hwnd,TIMER_PREVIEW);preview_show();
 CHECK("selecting archive replaces the opposite pane",g.pv_shown==4&&IsWindowVisible(archiveview_window()));if(g.pv_shown!=4)return 0;
 CHECK("archive listing finishes",idle());if(A.busy)return 0;return 1;
}
static int choose(const wchar_t *name,int kind){int row=row_named(name);CHECK("entry is visible in archive folder",row>=0);if(row<0)return 0;select_row(row);A.due=GetTickCount64();archiveview_tick();CHECK("entry decode finishes",idle());pump(20);CHECK("third pane uses the selected handler",g.ar_kind==kind);
 if(g.ar_kind==3){wchar_t title[512];uint64_t until=GetTickCount64()+8000;do{GetWindowTextW(textview_window(),title,512);if(wcsstr(title,L" lines |"))break;pump(5);}while(GetTickCount64()<until);CHECK("text helper decoded and laid out the selected entry",wcsstr(title,L" lines |")!=NULL);}
 if(g.ar_kind==2){uint64_t until=GetTickCount64()+8000;while(!archive_test_image_ready()&&GetTickCount64()<until)pump(5);CHECK("image helper delivered decoded pixels to the real reader",archive_test_image_ready());}
 return g.ar_kind==kind;
}
static void screenshot(void){wchar_t path[ARCP_NAME];DWORD n=GetEnvironmentVariableW(L"OPENPILOT_ARCHIVE_SCREENSHOT",path,ARCP_NAME);if(!n||n>=ARCP_NAME)return;UpdateWindow(g.hwnd);RECT rc;GetClientRect(g.hwnd,&rc);BITMAPINFO info={.bmiHeader={sizeof(BITMAPINFOHEADER),rc.right,-rc.bottom,1,32,BI_RGB}};void *pixels=NULL;HDC source=GetDC(g.hwnd),dc=CreateCompatibleDC(source);HBITMAP bitmap=CreateDIBSection(source,&info,DIB_RGB_COLORS,&pixels,NULL,0);if(bitmap){HGDIOBJ old=SelectObject(dc,bitmap);BitBlt(dc,0,0,rc.right,rc.bottom,source,0,0,SRCCOPY);GdiFlush();DWORD bytes=rc.right*rc.bottom*4;BITMAPFILEHEADER head={.bfType=0x4d42,.bfSize=sizeof head+sizeof(BITMAPINFOHEADER)+bytes,.bfOffBits=sizeof head+sizeof(BITMAPINFOHEADER)};HANDLE out=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);if(out!=INVALID_HANDLE_VALUE){DWORD w;WriteFile(out,&head,sizeof head,&w,NULL);WriteFile(out,&info.bmiHeader,sizeof(BITMAPINFOHEADER),&w,NULL);WriteFile(out,pixels,bytes,&w,NULL);CloseHandle(out);}SelectObject(dc,old);DeleteObject(bitmap);}DeleteDC(dc);ReleaseDC(g.hwnd,source);}

static int file_is(const wchar_t *path,const char *expected){HANDLE f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);if(f==INVALID_HANDLE_VALUE)return 0;char buffer[1024];DWORD n=0;BOOL ok=ReadFile(f,buffer,sizeof buffer,&n,NULL);CloseHandle(f);return ok&&n==strlen(expected)&&!memcmp(buffer,expected,n);}
static void test_archive_name_and_folder_model(void){
 CHECK("logical parent escape refused",!arc_name_safe(L"../escape.md")&&!arc_name_safe(L"folder/../escape.md"));
 CHECK("absolute Windows/Unix paths refused",!arc_name_safe(L"C:/escape.md")&&!arc_name_safe(L"/escape.md"));
 CHECK("Unicode and literal Linux punctuation retain logical identity",arc_name_safe(L"folder/你好 😀.md")&&arc_name_safe(L"a:b.md"));
 CHECK("unsafe Windows leaf names are never used for temporary files",!arc_leaf_safe(L"CON.txt")&&!arc_leaf_safe(L"name.")&&!arc_leaf_safe(L"a:b.md")&&arc_leaf_safe(L"你好 😀.md"));
 arc_entry entries[]={{{0,0,0,0,5,3},L"folder/one.md"},{{1,0,0,0,6,4},L"folder/deep/two.md"},{{2,0,0,0,1,1},L"Case.txt"},{{3,0,0,0,1,1},L"case.txt"}};
 arc_row *rows=NULL;uint32_t n=0;CHECK("root folder synthesized once without flattening case-distinct files",arc_rows_build(entries,4,L"",&rows,&n)&&n==3&&rows[0].flags==ARCP_DIR);arc_rows_free(rows,n);
 rows=NULL;n=0;CHECK("folder navigation preserves relative hierarchy",arc_rows_build(entries,4,L"folder/",&rows,&n)&&n==2&&!wcscmp(rows[0].name,L"deep")&&!wcscmp(rows[1].name,L"one.md"));arc_rows_free(rows,n);
}
static void test_archive_zip_nested_preview_and_restore(void){
 if(!open_archive(L"preview.zip",0))return;CHECK("ordinary ZIP starts miniz backend",A.backend==1);CHECK("initial archive listing does not auto-decode an entry",!g.ar_kind);
 wchar_t *left=_wcsdup(g.panes[0].path),*right=_wcsdup(g.panes[1].path);int tabs=g.panes[1].ntabs;
 if(choose(L"notes.md",3)){CHECK("decoded Markdown bytes match",A.preview&&file_is(A.preview->path,"# Archive preview\nExact Markdown bytes: hello!\n"));CHECK("text preview is physically present as third pane",IsWindowVisible(textview_window())&&IsWindowVisible(archiveview_window())&&IsWindowVisible(g.panes[0].hwnd));
 RECT archive,content;GetWindowRect(archiveview_window(),&archive);GetWindowRect(textview_window(),&content);CHECK("archive and content occupy separate adjacent columns",archive.right<content.left&&archive.bottom==content.bottom);
 screenshot();app_preview_close();CHECK("closing content leaves archive visible",g.pv_shown==4&&!g.ar_kind&&IsWindowVisible(archiveview_window()));}
 choose(L"picture.png",2);choose(L"document.pdf",1);
 int row=row_named(L"folder");CHECK("implicit archive folder exists",row>=0);if(row>=0){select_row(row);archiveview_preview(NULL);CHECK("folder metadata navigation completes",idle());CHECK("folder root has expected children",row_named(L"deep")>=0&&row_named(L"upper.txt")>=0&&!g.ar_kind);int deep=row_named(L"deep");if(deep>=0){select_row(deep);archiveview_preview(NULL);CHECK("nested folder navigation completes",idle());choose(L"readme.md",3);}archiveview_up(NULL);idle();archiveview_up(NULL);idle();CHECK("Up returns to the archive root",A.folder&&!*A.folder&&row_named(L"notes.md")>=0);}
 int unicode=row_named(L"unicode");if(unicode>=0){select_row(unicode);archiveview_preview(NULL);idle();if(choose(L"你好 😀.md",3))CHECK("Unicode entry name and content survive helper and temporary-file transport",A.preview&&file_is(A.preview->path,"Unicode entry"));archiveview_up(NULL);idle();}
 preview_hide();CHECK("both original pane paths and tabs restored",!g.pv_shown&&!wcscmp(left,g.panes[0].path)&&!wcscmp(right,g.panes[1].path)&&g.panes[1].ntabs==tabs&&IsWindowVisible(g.panes[1].hwnd));free(left);free(right);
 if(open_archive(L"cp437.zip",0)&&choose(L"café.txt",3))CHECK("legacy CP437 ZIP names resolve exact entry contents",A.preview&&file_is(A.preview->path,"legacy name"));
}
static void test_archive_general_and_zip_method_fallback(void){
 const wchar_t *names[]={L"preview.tar",L"preview.tar.gz",L"preview.7z",L"fallback.zip"};
 for(unsigned i=0;i<sizeof names/sizeof *names;i++)if(open_archive(names[i],0)){CHECK("general format / unsupported ZIP method uses libarchive",A.backend==2);if(choose(L"notes.md",3))CHECK("general backend preserves exact decoded content",A.preview&&file_is(A.preview->path,"# Archive preview\nExact Markdown bytes: hello!\n"));}
}
static void test_archive_unsafe_corrupt_and_size_limits(void){
 if(open_archive(L"unsafe.zip",0)){int row=row_named(L"../escape.md");CHECK("unsafe path is visible and excluded",row>=0&&(A.rows[row].flags&ARCP_UNSAFE));if(row>=0){select_row(row);CHECK("unsafe entries offer neither preview nor extraction",!archiveview_can_open(NULL)&&!archiveview_can_extract(NULL));}
  row=row_named(L"link.md");CHECK("literal archive symlink is excluded",row>=0&&(A.rows[row].flags&ARCP_SPECIAL));if(row>=0){select_row(row);CHECK("archive symlink is not followed",!archiveview_can_extract(NULL)&&!g.ar_kind);}}
 if(open_archive(L"corrupt.zip",0)){int row=row_named(L"bad.md");if(row>=0){select_row(row);A.due=GetTickCount64();archiveview_tick();CHECK("corrupt entry finishes with a visible error",idle()&&!g.ar_kind&&!A.preview&&wcsstr(A.status,L"integrity"));}}
 if(open_archive(L"too-large.zip",0)){int row=row_named(L"large.md");if(row>=0){select_row(row);A.due=GetTickCount64();archiveview_tick();CHECK("automatic preview refuses oversized entry",idle()&&!g.ar_kind&&!A.preview&&wcsstr(A.status,L"size limit"));}}
 if(open_archive(L"empty.zip",0))CHECK("empty ZIP is a successful empty listing",!A.count&&wcsstr(A.status,L"0 items"));
}
static void test_archive_superseded_selection_and_context(void){
 if(!open_archive(L"preview.zip",0))return;int image=row_named(L"picture.png"),notes=row_named(L"notes.md");if(image<0||notes<0)return;
 select_row(image);A.due=GetTickCount64();archiveview_tick();select_row(notes);A.due=GetTickCount64();archiveview_tick();CHECK("latest selection finishes after cancellation",idle());CHECK("only latest entry can own the third pane",g.ar_kind==3&&A.preview&&file_is(A.preview->path,"# Archive preview\nExact Markdown bytes: hello!\n"));
 fn_ctx c;app_preview_ctx(&c,archiveview_window(),FN_FROM_MENU);HMENU menu=CreatePopupMenu();fn_menu(menu,FN_MENU_ARCHIVE,&c);int count=GetMenuItemCount(menu),extract=0,foreign=0;
 for(int i=0;i<count;i++){wchar_t name[200];GetMenuStringW(menu,i,name,200,MF_BYPOSITION);if(wcsstr(name,L"Extract"))extract++;if(wcsstr(name,L"Delete")||wcsstr(name,L"Rename")||wcsstr(name,L"Paste"))foreign++;}
 CHECK("archive context has extraction and excludes normal file mutations",extract==1&&!foreign&&count>=4);DestroyMenu(menu);app_ctx_free(&c);
}
static void test_archive_command_scope_refuses_outer_file_mutations(void){
 if(!open_archive(L"preview.zip",0)||!choose(L"notes.md",3))return;
 fn_ctx c;app_preview_ctx(&c,archiveview_window(),FN_FROM_MENU);
 CHECK("archive listing has a separate logical command scope",c.where==FN_ARCHIVE);
 CHECK("all mutation controls resolve real registered commands",fn_find("file.delete")&&fn_find("file.rename")&&fn_find("edit.paste"));
 CHECK("normal file mutations cannot target the outer archive",!fn_applies(fn_find("file.delete"),&c)&&!fn_applies(fn_find("file.rename"),&c)&&!fn_applies(fn_find("edit.paste"),&c));
 CHECK("archive commands remain applicable",fn_applies(fn_find("archive.extract"),&c));app_ctx_free(&c);
 app_preview_ctx(&c,textview_window(),FN_FROM_MENU);CHECK("nested reader also refuses outer-file mutations",c.where==FN_ARCHIVE&&!fn_applies(fn_find("file.delete"),&c));
 CHECK("nested text reader keeps its own actions",fn_applies(fn_find("text.find"),&c));app_ctx_free(&c);
 preview_hide();app_preview_ctx(&c,textview_window(),FN_FROM_MENU);CHECK("late hidden-reader commands still cannot mutate the outer file",c.where==FN_ARCHIVE&&!fn_applies(fn_find("file.delete"),&c));app_ctx_free(&c);
 Pane *p=active_pane();p->cursor=fpe_view_find(p->view,L"notes.md");preview_note(p);KillTimer(g.hwnd,TIMER_PREVIEW);preview_show();app_preview_ctx(&c,textview_window(),FN_FROM_MENU);
 CHECK("ordinary text preview restores normal file command scope",g.pv_shown==3&&c.where==FN_PANE&&fn_applies(fn_find("file.delete"),&c));app_ctx_free(&c);
}
static void test_archive_operations_source_lifetime(void){
 if(!open_archive(L"preview.zip",0)||!choose(L"notes.md",3)||!A.preview)return;
 wchar_t destination[ARCP_NAME];swprintf(destination,ARCP_NAME,L"%ls\\destination",fixture);arc_file *file=A.preview;arc_file_hold(file);wchar_t *path=_wcsdup(file->path);
 int ok=app_stage_archive_copy(file->path,destination,arc_file_release,file);CHECK("explicit extraction uses pending Operations copy",ok);if(!ok){arc_file_release(file);free(path);return;}
 uint64_t token=g_op_requests->token;preview_hide();CHECK("closing archive preserves pending extraction source",GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES&&g_archive_sources&&g_archive_sources->token==token);
 ops_action cancel={.token=token,.action=OPS_CANCEL};operations_action(&cancel);
 uint64_t until=GetTickCount64()+3000;while(GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES&&GetTickCount64()<until)pump(5);
 CHECK("cancelled pending copy releases only its owned temporary source",GetFileAttributesW(path)==INVALID_FILE_ATTRIBUTES&&!g_archive_sources);free(path);
 if(!open_archive(L"preview.zip",0)||!choose(L"notes.md",3)||!A.preview)return;
 file=A.preview;arc_file_hold(file);path=_wcsdup(file->path);ok=app_stage_archive_copy(path,destination,arc_file_release,file);CHECK("completed extraction is staged through the same Operations path",ok);if(!ok){arc_file_release(file);free(path);return;}
 token=g_op_requests->token;preview_hide();ops_action start={.token=token,.action=OPS_START,.policy=FPE_CONFLICT_RENAME};operations_action(&start);ops_entry *entry=ops_find(&g_operations,token);until=GetTickCount64()+15000;
 while(entry&&!ops_finished(entry->h.state)&&GetTickCount64()<until){pump(5);entry=ops_find(&g_operations,token);}
 if(!entry||entry->h.state!=FPE_JOB_DONE||entry->h.errors)printf("extraction diagnostic: token=%llu state=%u errors=%u last_error=%u details=%ls\n",(unsigned long long)token,entry?entry->h.state:~0u,entry?entry->h.errors:0,entry?entry->h.last_error:0,entry?entry->details:L"missing Operations entry");
 CHECK("Operations publishes the extracted file with no errors",entry&&entry->h.state==FPE_JOB_DONE&&!entry->h.errors);
 wchar_t published[ARCP_NAME];swprintf(published,ARCP_NAME,L"%ls\\notes.md",destination);CHECK("published extraction has the exact original bytes",file_is(published,"# Archive preview\nExact Markdown bytes: hello!\n"));
 until=GetTickCount64()+3000;while(GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES&&GetTickCount64()<until)pump(5);
 CHECK("finished extraction releases its retained private source",GetFileAttributesW(path)==INVALID_FILE_ATTRIBUTES&&!g_archive_sources);free(path);
}
/* Children use inherited private events, real extraction/Operations owners,
 * WM_DESTROY and the same final archive-source shutdown helper as production. */
void archive_test_cleanup_gate(const wchar_t *,HANDLE,HANDLE,HANDLE,HANDLE);
void archive_test_cleanup_fail(arc_file *);
arc_file *archive_test_cleanup_source(void);
int archive_test_ready_source(arc_client *,wchar_t *,wchar_t *);
typedef struct archive_exit_source {uint32_t magic,mode;wchar_t path[ARCP_NAME],directory[ARCP_NAME];} archive_exit_source;
static void exit_report_path(wchar_t out[ARCP_NAME],int mode){swprintf(out,ARCP_NAME,L"%ls\\shutdown-%d.bin",fixture,mode);}
static int exit_source_write(const archive_exit_source *source){wchar_t path[ARCP_NAME];exit_report_path(path,source->mode);HANDLE h=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_NEW,0,NULL);if(h==INVALID_HANDLE_VALUE)return 0;DWORD n;int ok=WriteFile(h,source,sizeof *source,&n,NULL)&&n==sizeof *source;CloseHandle(h);return ok;}
static int exit_source_read(int mode,archive_exit_source *source){wchar_t path[ARCP_NAME],temp[ARCP_NAME],prefix[ARCP_NAME],expected[ARCP_NAME];exit_report_path(path,mode);HANDLE h=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);DWORD n=0;if(h==INVALID_HANDLE_VALUE)return 0;int ok=ReadFile(h,source,sizeof *source,&n,NULL)&&n==sizeof *source;CloseHandle(h);if(!ok||source->magic!=0x41524337||source->mode!=(uint32_t)mode||source->path[ARCP_NAME-1]||source->directory[ARCP_NAME-1])return 0;
 GetTempPathW(ARCP_NAME,temp);swprintf(prefix,ARCP_NAME,L"%lsOpenPilot-archive-",temp);size_t length=wcslen(prefix);GUID guid;if(wcsncmp(source->directory,prefix,length)||wcslen(source->directory)!=length+38||FAILED(CLSIDFromString(source->directory+length,&guid)))return 0;
 swprintf(expected,ARCP_NAME,L"%ls%ls\\%ls",source->directory[1]==L':'?L"\\\\?\\":L"",source->directory,mode==3?L"probe.txt":L"notes.md");return !wcscmp(expected,source->path);
}
static int archive_exit_child(HWND window,int mode,HANDLE events[6]){archive_exit_source source={.magic=0x41524337,.mode=(uint32_t)mode};arc_client *client=NULL;int ok=1;
 if(mode==1){
  ok=open_archive(L"preview.zip",0);int row=ok?row_named(L"notes.md"):-1;if(row<0)ok=0;
  if(ok){select_row(row);A.due=0;fn_timer(fn_find("preview.archive"),0);wchar_t dest[ARCP_NAME];swprintf(dest,ARCP_NAME,L"%ls\\destination",fixture);free(A.destination);A.destination=_wcsdup(dest);A.busy=1;A.generation=arc_client_request(A.client,ARC_EXTRACT,A.path,A.folder,A.rows[row].index);ok=idle()&&g_archive_sources&&g_op_requests;}
  if(ok){arc_file *file=g_archive_sources->owner;ops_entry *entry=ops_find(&g_operations,g_archive_sources->token);ok=file->refs==1&&entry&&entry->h.state==OPS_PENDING;wcscpy(source.path,file->path);wcscpy(source.directory,file->directory);preview_hide();}
 }else if(mode==2){
  wchar_t archive[ARCP_NAME];swprintf(archive,ARCP_NAME,L"%ls\\preview.zip",fixture);client=arc_client_start(NULL,0);arc_result *result=NULL;uint32_t index=UINT32_MAX;
  if(client){arc_client_request(client,ARC_OPEN,archive,L"",0);uint64_t until=GetTickCount64()+15000;while(!(result=arc_client_take(client))&&GetTickCount64()<until)Sleep(2);}
  if(result&&!result->error)for(uint32_t i=0;i<result->count;i++)if(!wcscmp(result->rows[i].name,L"notes.md"))index=result->rows[i].index;arc_result_free(result);ok=client&&index!=UINT32_MAX;
  if(ok){arc_client_request(client,ARC_EXTRACT,archive,L"",index);uint64_t until=GetTickCount64()+15000;while(!(ok=archive_test_ready_source(client,source.path,source.directory))&&GetTickCount64()<until)Sleep(2);}
 }else if(mode==3){arc_file *file=archive_test_cleanup_source();ok=file!=NULL;if(file){wcscpy(source.path,file->path);wcscpy(source.directory,file->directory);archive_test_cleanup_fail(file);arc_file_release(file);}}else ok=0;
 if(ok)ok=exit_source_write(&source);if(ok&&mode!=3)archive_test_cleanup_gate(source.path,events[0],events[1],mode==2?events[2]:NULL,events[3]);
 if(client)arc_client_stop(&client);DestroyWindow(window);pump(100);settings_save_join();folderwatch_join();fpe_search_index_shutdown();fpe_engine_destroy(g.engine);SetEvent(events[4]);archive_sources_finish();SetEvent(events[5]);
 if(ok&&mode==3)ok=GetFileAttributesW(source.path)!=INVALID_FILE_ATTRIBUTES&&GetFileAttributesW(source.directory)!=INVALID_FILE_ATTRIBUTES;CoUninitialize();return ok?0:1;
}
static void archive_exit_case(int mode){HANDLE events[6]={0};SECURITY_ATTRIBUTES sa={sizeof sa,NULL,TRUE};int ready=1;for(unsigned i=0;i<6;i++){events[i]=CreateEventW(&sa,TRUE,FALSE,NULL);if(!events[i])ready=0;}CHECK("owned shutdown gate events created",ready);if(!ready){for(unsigned i=0;i<6;i++)if(events[i])CloseHandle(events[i]);return;}
 wchar_t exe[MAX_PATH],cmd[ARCP_NAME*2];GetModuleFileNameW(NULL,exe,MAX_PATH);swprintf(cmd,ARCP_NAME*2,L"\"%ls\" --exit-cleanup \"%ls\" %d %llu %llu %llu %llu %llu %llu",exe,fixture,mode,(unsigned long long)(uintptr_t)events[0],(unsigned long long)(uintptr_t)events[1],(unsigned long long)(uintptr_t)events[2],(unsigned long long)(uintptr_t)events[3],(unsigned long long)(uintptr_t)events[4],(unsigned long long)(uintptr_t)events[5]);
 SIZE_T bytes=0;InitializeProcThreadAttributeList(NULL,1,0,&bytes);LPPROC_THREAD_ATTRIBUTE_LIST attrs=malloc(bytes);int initialized=attrs&&InitializeProcThreadAttributeList(attrs,1,0,&bytes);STARTUPINFOEXW si={{sizeof si}};si.lpAttributeList=attrs;PROCESS_INFORMATION pi={0};int started=initialized&&UpdateProcThreadAttribute(attrs,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,events,sizeof events,NULL,NULL)&&CreateProcessW(exe,cmd,NULL,NULL,TRUE,CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,NULL,NULL,&si.StartupInfo,&pi);CHECK("owned archive shutdown child launched",started);
 if(started){if(mode!=3){HANDLE waits[]={mode==2?events[2]:events[0],events[4]};DWORD state=WaitForMultipleObjects(2,waits,TRUE,20000);CHECK("child reaches final shutdown with owned cleanup held",state==WAIT_OBJECT_0);CHECK("final shutdown cannot return or exit while its cleanup producer/task is held",WaitForSingleObject(events[5],0)==WAIT_TIMEOUT&&WaitForSingleObject(pi.hProcess,150)==WAIT_TIMEOUT);SetEvent(events[3]);if(mode==2){CHECK("late worker release actually queues owned-source deletion",WaitForSingleObject(events[0],15000)==WAIT_OBJECT_0);CHECK("final drain includes cleanup enqueued after worker shutdown starts",WaitForSingleObject(events[5],0)==WAIT_TIMEOUT&&WaitForSingleObject(pi.hProcess,150)==WAIT_TIMEOUT);}}
  SetEvent(events[3]);SetEvent(events[1]);DWORD state=WaitForSingleObject(pi.hProcess,20000);CHECK("child exits once final cleanup is permitted",state==WAIT_OBJECT_0);if(state!=WAIT_OBJECT_0)TerminateProcess(pi.hProcess,1);DWORD code=1;GetExitCodeProcess(pi.hProcess,&code);CHECK("child completes the shared final archive-source path",state==WAIT_OBJECT_0&&code==0&&WaitForSingleObject(events[5],0)==WAIT_OBJECT_0);
  archive_exit_source source={0};int reported=exit_source_read(mode,&source);CHECK("child source report identifies only its exact owned GUID directory",reported);if(reported){if(mode==3){CHECK("failed cleanup scheduling retains exact owned data for review",GetFileAttributesW(source.path)!=INVALID_FILE_ATTRIBUTES&&GetFileAttributesW(source.directory)!=INVALID_FILE_ATTRIBUTES);CHECK("synthetic retained source can be removed exactly without recursion",DeleteFileW(source.path)&&RemoveDirectoryW(source.directory));}else CHECK("owned source file and directory are gone after actual process exit",GetFileAttributesW(source.path)==INVALID_FILE_ATTRIBUTES&&GetFileAttributesW(source.directory)==INVALID_FILE_ATTRIBUTES);}
  CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
 }
 if(initialized)DeleteProcThreadAttributeList(attrs);free(attrs);for(unsigned i=0;i<6;i++)CloseHandle(events[i]);
}
static void test_archive_pending_source_shutdown_waits_for_cleanup(void){archive_exit_case(1);}
static void test_archive_shutdown_waits_for_late_worker_cleanup(void){archive_exit_case(2);}
static void test_archive_shutdown_queue_failure_retains_owned_data(void){archive_exit_case(3);}

static void test_archive_right_source_single_pane_and_deactivation(void){
 if(open_archive(L"preview.zip",1)){CHECK("right source remains visible after swap",g.pv_swapped==1&&IsWindowVisible(g.panes[1].hwnd)&&!IsWindowVisible(g.panes[0].hwnd));choose(L"notes.md",3);preview_hide();CHECK("right-source close restores ordinary pane visibility",IsWindowVisible(g.panes[0].hwnd)&&IsWindowVisible(g.panes[1].hwnd));}
 g.dual=0;set_active(0);layout();if(open_archive(L"preview.zip",0)){choose(L"notes.md",3);preview_hide();CHECK("single pane layout restored",IsWindowVisible(g.panes[0].hwnd)&&!IsWindowVisible(g.panes[1].hwnd));}g.dual=1;layout();
 if(open_archive(L"preview.zip",0)){choose(L"notes.md",3);const char *off="[functions]\npreview.archive = off\nsvc.shell = off\nsvc.icons = off\n";fn_settings(off,strlen(off));fn_configure_all();CHECK("disabling archive preview removes both auxiliary panes",!g.ar_on&&!g.pv_shown&&!g.ar_kind);}
}
int wmain(int argc,wchar_t **argv){setvbuf(stdout,NULL,_IONBF,0);int exit_child=argc==10&&!wcscmp(argv[1],L"--exit-cleanup");if(argc!=2&&!exit_child)return 2;wcsncpy(fixture,argv[exit_child?2:1],ARCP_NAME-1);SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
 fn_host host={host_now,host_arm,host_status};fn_init(&host);fn_register(app_functions,sizeof app_functions/sizeof *app_functions);static const fn_def ops={.id="operations.show",.title=L"Operations",.life=FN_COLD,.where=FN_ANYWHERE,.opts=operations_opts,.run=run_operations_show};fn_register(&ops,1);functions_register();relay_register();palette_register();
 const char *config="[functions]\nsvc.shell = off\nsvc.icons = off\n[panel.side]\nslide = off\n";fn_settings(config,strlen(config));fn_configure_all();g.engine=fpe_engine_create(NULL);g.dual=1;g.ratio=0.4;g.show_tree=0;session_defaults(&g_ses);
 for(int i=0;i<2;i++){Pane *p=&g.panes[i];p->idx=i;p->ntabs=1;p->drop_row=-1;p->opts.dirs_first=1;}
 HINSTANCE inst=GetModuleHandleW(NULL);register_class(L"OpenPilot",main_proc,0,inst);register_class(L"OpenPilotPane",pane_proc,CS_DBLCLKS,inst);register_class(L"OpenPilotTree",tree_proc,CS_DBLCLKS,inst);register_class(L"OpenPilotDebug",dbg_proc,0,inst);
 HWND window=CreateWindowExW(0,L"OpenPilot",L"Owned archive acceptance",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,100,100,1300,760,NULL,NULL,inst,NULL);if(!window)return 3;
 for(int i=0;i<2;i++){fpe_dir *dir=read_start(fixture);if(dir){p_show_dir(&g.panes[i],dir,NULL,0);fpe_dir_release(dir);}}
 ShowWindow(window,SW_SHOWNOACTIVATE);UpdateWindow(window);pump(20);
 if(exit_child){HANDLE events[6];for(unsigned i=0;i<6;i++)events[i]=(HANDLE)(uintptr_t)wcstoull(argv[4+i],NULL,10);return archive_exit_child(window,_wtoi(argv[3]),events);}

 test_archive_name_and_folder_model();test_archive_zip_nested_preview_and_restore();test_archive_general_and_zip_method_fallback();test_archive_unsafe_corrupt_and_size_limits();test_archive_superseded_selection_and_context();test_archive_command_scope_refuses_outer_file_mutations();test_archive_operations_source_lifetime();test_archive_right_source_single_pane_and_deactivation();test_archive_pending_source_shutdown_waits_for_cleanup();test_archive_shutdown_waits_for_late_worker_cleanup();test_archive_shutdown_queue_failure_retains_owned_data();
 char report[65536];fn_report_csv(report,sizeof report);CHECK("archive functions have no lifetime violations",strstr(report,"metric,fn_violations,,,,0,")!=NULL);
 DestroyWindow(window);pump(100);settings_save_join();folderwatch_join();fpe_engine_destroy(g.engine);archive_sources_finish();CoUninitialize();printf("%d passed, %d failed; 11 archive selectors\n",passed,failed);return failed?1:0;
}
