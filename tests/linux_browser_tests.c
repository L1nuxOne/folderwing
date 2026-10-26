/* Linux integration / native desktop acceptance. Owner: linux-prototype/.
 * Whole executable, owned temporary trees; requires GTK4 display and the real
 * helper. No Windows input, distro shutdown, privilege changes or user fixtures. */
#define _GNU_SOURCE
#define FW_BROWSER_TEST
#include "../linux-prototype/browser.c"
#include "../src/wslproto.h"
#include <glib/gstdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/xattr.h>
#include <gio/gdesktopappinfo.h>

static const char *helper, *icon, *executable;
static void wait_visible_count(Pane *pane,guint count);
static char *fixture;
static guint assertions;
#define CHECK(x) do { assertions++; if (!(x)) g_error("%s:%d: %s", __FILE__, __LINE__, #x); } while (0)
static void pump(void) { while (g_main_context_iteration(NULL, FALSE)) {} }
static void settle(guint ms)
{
    gint64 until = g_get_monotonic_time()+1000*(gint64)ms;
    do { pump(); g_usleep(1000); } while (g_get_monotonic_time() < until);
}
static gboolean wait_revision(FwBrowser *browser, guint after)
{
    gint64 end = g_get_monotonic_time()+10000000;
    while (g_get_monotonic_time() < end) {
        pump();
        if (!fw_browser_busy(browser) && (fw_browser_revision(browser) > after || *fw_browser_error(browser))) break;
        g_usleep(1000);
    }
    if (fw_browser_revision(browser) <= after) g_printerr("Browser error: %s\n", fw_browser_error(browser));
    return !fw_browser_busy(browser) && fw_browser_revision(browser) > after;
}
static void wait_failure(FwBrowser *browser)
{
    gint64 end = g_get_monotonic_time()+3000000;
    while (fw_browser_busy(browser) && g_get_monotonic_time() < end) { pump(); g_usleep(1000); }
    CHECK(!fw_browser_busy(browser)); CHECK(*fw_browser_error(browser)); CHECK(!fw_browser_watching(browser));
}
static char *directory(const char *name)
{
    char *path = g_build_filename(fixture, name, NULL); CHECK(g_mkdir(path, 0700) == 0); return path;
}
static void file(const char *root, const char *name, const char *contents)
{
    char *path = g_build_filename(root, name, NULL); GError *error = NULL;
    CHECK(g_file_set_contents(path, contents, -1, &error)); CHECK(!error); g_free(path);
}
static GFileInfo *find_entry(GListModel *model, const char *name)
{
    for (guint i = 0; i < g_list_model_get_n_items(model); i++) {
        GFileInfo *info = g_list_model_get_item(model, i);
        if (!strcmp(g_file_info_get_name(info), name)) return info;
        g_object_unref(info);
    }
    return NULL;
}
static void discard_browser(FwBrowser *browser) { fw_browser_close(browser); g_object_unref(browser); settle(20); }
static void test_literal_paths(void)
{
    const char raw[] = "/tmp/é/byte-\xff-\\-\n";
    char *display = fw_display_bytes(raw); CHECK(g_utf8_validate(display, -1, NULL));
    GError *error = NULL; char *roundtrip = fw_parse_path(display, "/", &error);
    CHECK(!error); CHECK(!strcmp(roundtrip, raw)); CHECK(strstr(display, "\\xFF")); CHECK(strstr(display, "\\x0A"));
    g_free(display); g_free(roundtrip);
    roundtrip = fw_parse_path("../two words", "/tmp/example", &error); CHECK(!error); CHECK(!strcmp(roundtrip, "/tmp/example/../two words")); g_free(roundtrip);
    CHECK(!fw_parse_path("/tmp/\\x00", "/", &error)); CHECK(error); g_clear_error(&error);
    CHECK(!fw_parse_path("/tmp/\\xQ1", "/", &error)); CHECK(error); g_clear_error(&error);
}
static void test_native_listing_metadata(void)
{
    char *root = directory("listing"); file(root, "日本語.txt", "hello"); file(root, ".hidden", "hidden"); file(root, "bad-\xff", "raw");
    char *folder = g_build_filename(root, "child", NULL), *link = g_build_filename(root, "folder-link", NULL);
    CHECK(!g_mkdir(folder, 0700)); CHECK(!symlink("child", link));
    FwBrowser *browser = fw_browser_new(helper); fw_browser_navigate(browser, root); CHECK(wait_revision(browser, 0));
    CHECK(fw_browser_watching(browser)); CHECK(!strcmp(root, fw_browser_path(browser))); CHECK(g_list_model_get_n_items(fw_browser_entries(browser)) == 5);
    GFileInfo *info = find_entry(fw_browser_entries(browser), "日本語.txt"); CHECK(info); CHECK(g_file_info_get_size(info) == 5);
    CHECK(g_file_info_get_attribute_uint32(info, "unix::uid") == getuid()); g_object_unref(info);
    info = find_entry(fw_browser_entries(browser), "bad-\xff"); CHECK(info); CHECK(strstr(g_file_info_get_display_name(info), "\\xFF")); g_object_unref(info);
    info = find_entry(fw_browser_entries(browser), "folder-link"); CHECK(info); CHECK(fw_entry_directory(info)); CHECK(g_file_info_get_is_symlink(info)); g_object_unref(info);
    discard_browser(browser); g_free(folder); g_free(link); g_free(root);
}
static void test_live_watches_independent(void)
{
    char *left = directory("watch-left"), *right = directory("watch-right");
    FwBrowser *a = fw_browser_new(helper), *b = fw_browser_new(helper);
    fw_browser_navigate(a, left); fw_browser_navigate(b, right); CHECK(wait_revision(a, 0)); CHECK(wait_revision(b, 0));
    guint left_revision = fw_browser_revision(a), revision = fw_browser_revision(b);
    file(right, "created", "x"); CHECK(wait_revision(b, revision)); CHECK(fw_browser_revision(a) == left_revision);
    GFileInfo *info = find_entry(fw_browser_entries(b), "created"); CHECK(info); g_object_unref(info);
    revision = fw_browser_revision(b); char *from = g_build_filename(right, "created", NULL), *to = g_build_filename(right, "renamed", NULL);
    CHECK(!g_rename(from, to)); CHECK(wait_revision(b, revision)); CHECK(!find_entry(fw_browser_entries(b), "created"));
    revision = fw_browser_revision(b); file(right, "renamed", "longer contents"); CHECK(wait_revision(b, revision));
    info = find_entry(fw_browser_entries(b), "renamed"); CHECK(info); CHECK(g_file_info_get_size(info) == 15); g_object_unref(info);
    revision = fw_browser_revision(b); CHECK(!g_unlink(to)); CHECK(wait_revision(b, revision)); CHECK(!g_list_model_get_n_items(fw_browser_entries(b)));
    fw_browser_navigate(a, right); CHECK(wait_revision(a, left_revision));
    left_revision = fw_browser_revision(a); revision = fw_browser_revision(b);
    file(right, "both-panes", "same root"); CHECK(wait_revision(a, left_revision)); CHECK(wait_revision(b, revision));
    discard_browser(a); discard_browser(b); g_free(from); g_free(to); g_free(left); g_free(right);
}
static void test_navigation_failure_and_supersession(void)
{
    char *a = directory("navigation-a"), *b = directory("navigation-b"); file(a, "first", "a"); file(b, "last", "b");
    FwBrowser *browser = fw_browser_new(helper); fw_browser_navigate(browser, a); CHECK(wait_revision(browser, 0));
    guint revision = fw_browser_revision(browser);
    char *missing = g_build_filename(a, "missing", NULL); fw_browser_navigate(browser, missing); wait_failure(browser);
    CHECK(!strcmp(fw_browser_path(browser), a)); CHECK(fw_browser_revision(browser) == revision);
    GFileInfo *info = find_entry(fw_browser_entries(browser), "first"); CHECK(info); g_object_unref(info);
    for (guint i = 0; i < 12; i++) fw_browser_navigate(browser, i%2 ? a : b);
    fw_browser_navigate(browser, b); CHECK(wait_revision(browser, revision)); settle(150);
    CHECK(!strcmp(fw_browser_path(browser), b)); CHECK(!*fw_browser_error(browser));
    info = find_entry(fw_browser_entries(browser), "last"); CHECK(info); g_object_unref(info);
    CHECK(!find_entry(fw_browser_entries(browser), "first"));
    discard_browser(browser);
    browser = fw_browser_new(helper); fw_browser_navigate(browser, missing); wait_failure(browser);
    CHECK(!g_mkdir(missing, 0700)); fw_browser_refresh(browser); CHECK(wait_revision(browser, 0));
    CHECK(!strcmp(fw_browser_path(browser), missing)); discard_browser(browser);
    g_free(a); g_free(b); g_free(missing);
}
static void test_protocol_disconnect_and_close(void)
{
    const char *names[] = {"bad-helper", "disconnect-helper", "truncated-helper"};
    for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
        char *path = g_build_filename(fixture, names[i], NULL); CHECK(!symlink(executable, path));
        FwBrowser *browser = fw_browser_new(path); fw_browser_navigate(browser, fixture); wait_failure(browser);
        CHECK(fw_browser_revision(browser) == 0); CHECK(!g_list_model_get_n_items(fw_browser_entries(browser)));
        discard_browser(browser); g_free(path);
    }
    FwBrowser *browser = fw_browser_new(helper); fw_browser_navigate(browser, fixture);
    gpointer weak = browser; g_object_add_weak_pointer(G_OBJECT(browser), &weak); discard_browser(browser); settle(100);
    CHECK(!weak);
}
static void wait_panes(App *app)
{
    gint64 end = g_get_monotonic_time()+10000000;
    for (;;) {
        pump(); gboolean ready = TRUE;
        for (guint i = 0; i < 2; i++) {
            Pane *p = &app->panes[i];
            if (fw_browser_busy(p->browser) || !fw_browser_revision(p->browser) || gtk_sort_list_model_get_pending(p->sorted) || gtk_filter_list_model_get_pending(p->filtered)) ready = FALSE;
        }
        if (ready) return;
        CHECK(g_get_monotonic_time() < end); g_usleep(1000);
    }
}
static void save_snapshot(App *app)
{
    const char *output = g_getenv("FW_LINUX_SCREENSHOT"); if (!output) return;
    settle(100);
    GtkWidget *widget = GTK_WIDGET(app->window); int width = gtk_widget_get_width(widget), height = gtk_widget_get_height(widget);
    GdkPaintable *paintable = gtk_widget_paintable_new(widget); GtkSnapshot *snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, snapshot, width, height);
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot); CHECK(node);
    GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(widget)); CHECK(renderer);
    GdkTexture *texture = gsk_renderer_render_texture(renderer, node, NULL); CHECK(texture);
    CHECK(gdk_texture_save_to_png(texture, output)); g_object_unref(texture); gsk_render_node_unref(node); g_object_unref(paintable);
}
static void test_two_pane_desktop(void)
{
    char *left = directory("desktop-left"), *right = directory("desktop-right");
    file(left, "file10.txt", "1234567890"); file(left, "file2.txt", "12"); file(left, ".hidden", "x");
    char *child = g_build_filename(left, "Folder", NULL); CHECK(!g_mkdir(child, 0700));
    App *app = create_window(NULL, helper, icon, left, right); gtk_window_present(app->window); wait_panes(app);
    Pane *pane = app->panes; CHECK(g_list_model_get_n_items(G_LIST_MODEL(pane->sorted)) == 3);
    GFileInfo *info = g_list_model_get_item(G_LIST_MODEL(pane->sorted), 0); CHECK(fw_entry_directory(info)); g_object_unref(info);
    info = g_list_model_get_item(G_LIST_MODEL(pane->sorted), 1); CHECK(!strcmp(g_file_info_get_name(info), "file2.txt")); g_object_unref(info);
    GtkToggleButton *hidden = g_object_get_data(G_OBJECT(app->window), "hidden-button"); gtk_toggle_button_set_active(hidden, TRUE); wait_panes(app);
    CHECK(g_list_model_get_n_items(G_LIST_MODEL(pane->sorted)) == 4);
    GListModel *columns = gtk_column_view_get_columns(GTK_COLUMN_VIEW(pane->view));
    GtkColumnViewColumn *size = g_list_model_get_item(columns, 1); gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(pane->view), size, GTK_SORT_DESCENDING); g_object_unref(size); wait_panes(app);
    info = g_list_model_get_item(G_LIST_MODEL(pane->sorted), 0); CHECK(fw_entry_directory(info)); g_object_unref(info);
    info = g_list_model_get_item(G_LIST_MODEL(pane->sorted), 1); CHECK(!strcmp(g_file_info_get_name(info), "file10.txt")); g_object_unref(info);
    g_signal_emit_by_name(pane->view, "activate", 0); CHECK(wait_revision(pane->browser, 1)); wait_panes(app); CHECK(!strcmp(fw_browser_path(pane->browser), child));
    g_signal_emit_by_name(pane->back, "clicked"); CHECK(wait_revision(pane->browser, 2)); wait_panes(app); CHECK(!strcmp(fw_browser_path(pane->browser), left));
    g_signal_emit_by_name(pane->forward, "clicked"); CHECK(wait_revision(pane->browser, 3)); wait_panes(app); CHECK(!strcmp(fw_browser_path(pane->browser), child));
    pane_up(pane); CHECK(wait_revision(pane->browser, 4)); wait_panes(app);
    char *literal = g_build_filename(left, "bytes-\xff", NULL); CHECK(!g_mkdir(literal, 0700));
    char *escaped = fw_display_bytes(literal); guint before = fw_browser_revision(pane->browser);
    gtk_editable_set_text(GTK_EDITABLE(pane->address), escaped); g_signal_emit_by_name(pane->address, "activate");
    CHECK(wait_revision(pane->browser, before)); wait_panes(app); CHECK(!strcmp(fw_browser_path(pane->browser), literal));
    before = fw_browser_revision(pane->browser); pane_up(pane); CHECK(wait_revision(pane->browser, before)); wait_panes(app);
    char *absent = g_build_filename(left, "does-not-exist", NULL);
    gtk_editable_set_text(GTK_EDITABLE(pane->address), absent); g_signal_emit_by_name(pane->address, "activate"); wait_failure(pane->browser);
    CHECK(!strcmp(gtk_editable_get_text(GTK_EDITABLE(pane->address)), left));
    before = fw_browser_revision(pane->browser); fw_browser_refresh(pane->browser); CHECK(wait_revision(pane->browser, before)); wait_panes(app);
    g_free(literal); g_free(escaped); g_free(absent);
    guint revision = fw_browser_revision(app->panes[1].browser); file(right, "live update.txt", "hello"); CHECK(wait_revision(app->panes[1].browser, revision)); wait_panes(app);
    CHECK(g_list_model_get_n_items(G_LIST_MODEL(app->panes[1].sorted)) == 1);
    gtk_window_set_default_size(app->window, 1100, 680); settle(100); save_snapshot(app);
    gtk_window_close(app->window); settle(100); g_free(left); g_free(right); g_free(child);
}
static void test_large_listing_and_cleanup(void)
{
    char *root = directory("large");
    for (guint i = 0; i < 10000; i++) { char *name = g_strdup_printf("item-%05u.txt", i); file(root, name, "data"); g_free(name); }
    gint64 start = g_get_monotonic_time(); App *app = create_window(NULL, helper, icon, root, root);
    gtk_window_present(app->window); wait_panes(app);
    for(guint i=0;i<2;i++) {
        Pane *p=&app->panes[i];
        if(g_list_model_get_n_items(G_LIST_MODEL(p->sorted))!=10000)
            g_printerr("READINESS pane %u snapshot=%u filtered=%u sorted=%u filter-pending=%u sort-pending=%u revision=%u error=%s\n", i,
                g_list_model_get_n_items(fw_browser_entries(p->browser)),g_list_model_get_n_items(G_LIST_MODEL(p->filtered)),g_list_model_get_n_items(G_LIST_MODEL(p->sorted)),
                gtk_filter_list_model_get_pending(p->filtered),gtk_sort_list_model_get_pending(p->sorted),fw_browser_revision(p->browser),fw_browser_error(p->browser));
        wait_visible_count(p,10000);
        CHECK(g_list_model_get_n_items(G_LIST_MODEL(p->sorted))==10000);
    }
    g_print("MEASURE two-pane 10000-file load and sort %.1f ms; helper listing %.1f / %.1f ms\n", (g_get_monotonic_time()-start)/1000.0,
        fw_browser_elapsed_ms(app->panes[0].browser), fw_browser_elapsed_ms(app->panes[1].browser));
    gtk_window_close(app->window); settle(100); g_free(root);
}
static void remove_owned_tree(const char *path)
{
    struct stat st; if (lstat(path, &st)) return;
    if (!S_ISDIR(st.st_mode)) { CHECK(!g_unlink(path)); return; }
    GDir *dir = g_dir_open(path, 0, NULL); CHECK(dir); const char *name;
    while ((name = g_dir_read_name(dir))) { char *child = g_build_filename(path, name, NULL); remove_owned_tree(child); g_free(child); }
    g_dir_close(dir); CHECK(!g_rmdir(path));
}
static char **operation_env;
static char *fault_helper;
static FwJobProgress wait_job(FwJob *job)
{
    gint64 end=g_get_monotonic_time()+15000000; FwJobProgress p;
    for(;;){pump();fw_job_progress(job,&p);if(!p.active)return p;fw_job_progress_clear(&p);CHECK(g_get_monotonic_time()<end);g_usleep(1000);}
}
static FwJob *new_job(const char *binary,FwOperation op,const char *source,const char *dest,FwConflict policy,char **env)
{
    const char *sources[]={source};GError *error=NULL;
    FwJob *job=fw_job_new(binary,op,sources,1,dest,policy,(const char *const *)(env?env:operation_env),&error);
    CHECK(job);CHECK(!error);return job;
}
static FwJobProgress perform(FwJob *job)
{
    fw_job_start(job);FwJobProgress p=wait_job(job);g_object_unref(job);return p;
}
static void bytes_equal(const char *path,const char *expected)
{
    char *data=NULL;gsize size=0;CHECK(g_file_get_contents(path,&data,&size,NULL));CHECK(size==strlen(expected));CHECK(!memcmp(data,expected,size));g_free(data);
}
static void test_operations_copy_move_conflicts(void)
{
    char *src=directory("ops-source"),*dst=directory("ops-destination"),*moved=directory("ops-moved");
    file(src,"document.txt","first version");char *source=g_build_filename(src,"document.txt",NULL),*target=g_build_filename(dst,"document.txt",NULL);
    CHECK(!g_chmod(source,0640));CHECK(!setxattr(source,"user.folderwing", "metadata",8,0));
    FwJobProgress p=perform(new_job(helper,FW_COPY,source,dst,FW_KEEP_BOTH,NULL));CHECK(p.state==FW_JOB_COMPLETE);CHECK(!p.errors);fw_job_progress_clear(&p);bytes_equal(target,"first version");bytes_equal(source,"first version");
    struct stat st;CHECK(!stat(target,&st));CHECK((st.st_mode&0777)==0640);char attr[32];CHECK(getxattr(target,"user.folderwing",attr,sizeof attr)==8);CHECK(!memcmp(attr,"metadata",8));
    p=perform(new_job(helper,FW_COPY,source,dst,FW_KEEP_BOTH,NULL));CHECK(p.state==FW_JOB_COMPLETE);fw_job_progress_clear(&p);
    char *other=g_build_filename(dst,"document (2).txt",NULL);bytes_equal(other,"first version");
    file(src,"document.txt","replacement");
    p=perform(new_job(helper,FW_COPY,source,dst,FW_SKIP,NULL));CHECK(p.state==FW_JOB_COMPLETE);CHECK(p.skipped==1);fw_job_progress_clear(&p);bytes_equal(target,"first version");
    p=perform(new_job(helper,FW_COPY,source,dst,FW_FAIL_CONFLICT,NULL));CHECK(p.state==FW_JOB_ERRORS);CHECK(p.errors==1);CHECK(strstr(p.details,"Error"));fw_job_progress_clear(&p);bytes_equal(target,"first version");
    p=perform(new_job(helper,FW_COPY,source,dst,FW_REPLACE,NULL));CHECK(p.state==FW_JOB_COMPLETE);fw_job_progress_clear(&p);bytes_equal(target,"replacement");
    p=perform(new_job(helper,FW_MOVE,source,moved,FW_FAIL_CONFLICT,NULL));CHECK(p.state==FW_JOB_COMPLETE);fw_job_progress_clear(&p);CHECK(!g_file_test(source,G_FILE_TEST_EXISTS));
    char *out=g_build_filename(moved,"document.txt",NULL);bytes_equal(out,"replacement");
    char *tree=g_build_filename(src,"tree",NULL);CHECK(!g_mkdir(tree,0700));file(tree,"child","child data");char *link=g_build_filename(tree,"link",NULL);CHECK(!symlink("child",link));
    p=perform(new_job(helper,FW_COPY,tree,dst,FW_KEEP_BOTH,NULL));CHECK(p.state==FW_JOB_COMPLETE);fw_job_progress_clear(&p);
    char *child=g_build_filename(dst,"tree","child",NULL),*out_link=g_build_filename(dst,"tree","link",NULL);bytes_equal(child,"child data");CHECK(g_file_test(out_link,G_FILE_TEST_IS_SYMLINK));
    const char *paths[]={tree};GError *error=NULL;CHECK(!fw_job_new(helper,(FwOperation)2,paths,1,NULL,FW_KEEP_BOTH,NULL,&error));CHECK(error);g_clear_error(&error);
    g_free(src);g_free(dst);g_free(moved);g_free(source);g_free(target);g_free(other);g_free(out);g_free(tree);g_free(link);g_free(child);g_free(out_link);
}
static void test_operations_cross_device_move_barriers(void)
{
    char *src=directory("ops-cross-source"),*source=g_build_filename(src,"move.txt",NULL);
    char *dest=g_strdup("/dev/shm/folderwing-linux-ops-XXXXXX");CHECK(g_mkdtemp(dest));struct stat a,b;CHECK(!stat(src,&a));CHECK(!stat(dest,&b));CHECK(a.st_dev!=b.st_dev);
    file(src,"move.txt","cross-device bytes");FwJobProgress p=perform(new_job(helper,FW_MOVE,source,dest,FW_KEEP_BOTH,NULL));CHECK(p.state==FW_JOB_COMPLETE);fw_job_progress_clear(&p);CHECK(!g_file_test(source,G_FILE_TEST_EXISTS));
    char *target=g_build_filename(dest,"move.txt",NULL);bytes_equal(target,"cross-device bytes");
    file(src,"retain.txt","retain on failed barrier");char *retain=g_build_filename(src,"retain.txt",NULL),*failed=g_build_filename(dest,"retain.txt",NULL);
    char **env=g_environ_setenv(g_strdupv(operation_env),"FW_OPS_FAIL_CONTAINS","/payload",TRUE);
    p=perform(new_job(fault_helper,FW_MOVE,retain,dest,FW_KEEP_BOTH,env));CHECK(p.state==FW_JOB_ERRORS);CHECK(p.errors);fw_job_progress_clear(&p);
    bytes_equal(retain,"retain on failed barrier");CHECK(!g_file_test(failed,G_FILE_TEST_EXISTS));
    remove_owned_tree(dest);g_strfreev(env);g_free(src);g_free(source);g_free(dest);g_free(target);g_free(retain);g_free(failed);
}
static char *only_child(const char *root)
{
    GDir *dir=g_dir_open(root,0,NULL);CHECK(dir);const char *name=g_dir_read_name(dir);CHECK(name);char *path=g_build_filename(root,name,NULL);CHECK(!g_dir_read_name(dir));g_dir_close(dir);return path;
}
static void test_operations_trash_durability(void)
{
    char *src=directory("ops-trash-source"),*data=directory("ops-trash-data"),*source=g_build_filename(src,"trash ' me",NULL);
    char **env=g_environ_setenv(g_strdupv(operation_env),"XDG_DATA_HOME",data,TRUE);
    char *log=g_build_filename(data,"sync.log",NULL);env=g_environ_setenv(env,"FW_OPS_LOG",log,TRUE);
    file(src,"trash ' me","recoverable");FwJobProgress p=perform(new_job(fault_helper,FW_TRASH,source,NULL,FW_FAIL_CONFLICT,env));CHECK(p.state==FW_JOB_COMPLETE);fw_job_progress_clear(&p);CHECK(!g_file_test(source,G_FILE_TEST_EXISTS));
    char *files=g_build_filename(data,"Trash","files",NULL),*infos=g_build_filename(data,"Trash","info",NULL);
    char *trashed=only_child(files),*info=only_child(infos);bytes_equal(trashed,"recoverable");char *text=NULL;CHECK(g_file_get_contents(info,&text,NULL,NULL));CHECK(strstr(text,"trash%20%27%20me"));CHECK(strstr(text,"DeletionDate="));g_free(text);
    CHECK(g_file_get_contents(log,&text,NULL,NULL));char *meta_sync=strstr(text,info);CHECK(meta_sync);char *info_sync=strstr(meta_sync,infos);CHECK(info_sync);char *file_sync=strstr(info_sync,files);CHECK(file_sync);CHECK(strstr(file_sync,src));g_free(text);
    char *predata=directory("ops-trash-fail-before"),*preinfo=g_build_filename(predata,"Trash","info",NULL);
    g_strfreev(env);env=g_environ_setenv(g_strdupv(operation_env),"XDG_DATA_HOME",predata,TRUE);env=g_environ_setenv(env,"FW_OPS_FAIL_PATH",preinfo,TRUE);env=g_environ_setenv(env,"FW_OPS_FAIL_NTH","2",TRUE);
    file(src,"retain","not removed");char *retain=g_build_filename(src,"retain",NULL);
    p=perform(new_job(fault_helper,FW_TRASH,retain,NULL,FW_FAIL_CONFLICT,env));CHECK(p.state==FW_JOB_ERRORS);CHECK(p.errors);fw_job_progress_clear(&p);bytes_equal(retain,"not removed");
    char *latedata=directory("ops-trash-fail-after"),*latefiles=g_build_filename(latedata,"Trash","files",NULL),*lateinfos=g_build_filename(latedata,"Trash","info",NULL);
    g_strfreev(env);env=g_environ_setenv(g_strdupv(operation_env),"XDG_DATA_HOME",latedata,TRUE);env=g_environ_setenv(env,"FW_OPS_FAIL_PATH",latefiles,TRUE);env=g_environ_setenv(env,"FW_OPS_FAIL_NTH","2",TRUE);
    p=perform(new_job(fault_helper,FW_TRASH,retain,NULL,FW_FAIL_CONFLICT,env));CHECK(p.state==FW_JOB_ERRORS);CHECK(p.errors);fw_job_progress_clear(&p);
    char *kept=only_child(latefiles),*kept_info=only_child(lateinfos);bytes_equal(kept,"not removed");CHECK(g_file_test(kept_info,G_FILE_TEST_IS_REGULAR));
    g_strfreev(env);g_free(src);g_free(data);g_free(source);g_free(log);g_free(files);g_free(infos);g_free(trashed);g_free(info);g_free(predata);g_free(preinfo);g_free(retain);g_free(latedata);g_free(latefiles);g_free(lateinfos);g_free(kept);g_free(kept_info);
}
static char **pause_environment(const char *root,char **marker,char **release)
{
    *marker=g_build_filename(root,"paused",NULL);*release=g_build_filename(root,"release",NULL);
    char **env=g_environ_setenv(g_strdupv(operation_env),"FW_OPS_PAUSE_CONTAINS","/payload",TRUE);
    env=g_environ_setenv(env,"FW_OPS_MARKER",*marker,TRUE);return g_environ_setenv(env,"FW_OPS_RELEASE",*release,TRUE);
}
static void wait_marker(const char *marker)
{
    gint64 end=g_get_monotonic_time()+5000000;while(!g_file_test(marker,G_FILE_TEST_EXISTS)){pump();CHECK(g_get_monotonic_time()<end);g_usleep(1000);}
}
static void wait_cancel_sent(FwJob *job)
{
    gint64 end=g_get_monotonic_time()+5000000;
    for(;;){pump();FwJobProgress p;fw_job_progress(job,&p);gboolean sent=p.cancel_sent;fw_job_progress_clear(&p);if(sent)return;CHECK(g_get_monotonic_time()<end);g_usleep(1000);}
}
static void test_operations_cancel_and_disconnect(void)
{
    char *src=directory("ops-cancel-source"),*dst=directory("ops-cancel-dest"),*source=g_build_filename(src,"retain",NULL),*target=g_build_filename(dst,"retain",NULL);file(src,"retain","keep me");
    FwJob *job=new_job(helper,FW_MOVE,source,dst,FW_KEEP_BOTH,NULL);fw_job_cancel(job);FwJobProgress p=perform(job);CHECK(p.state==FW_JOB_CANCELLED);fw_job_progress_clear(&p);bytes_equal(source,"keep me");CHECK(!g_file_test(target,G_FILE_TEST_EXISTS));
    char *marker,*release;char **env=pause_environment(dst,&marker,&release);
    job=new_job(fault_helper,FW_COPY,source,dst,FW_KEEP_BOTH,env);fw_job_start(job);wait_marker(marker);fw_job_cancel(job);wait_cancel_sent(job);CHECK(g_file_set_contents(release,"release",-1,NULL));
    p=wait_job(job);CHECK(p.state==FW_JOB_CANCELLED);fw_job_progress_clear(&p);g_object_unref(job);bytes_equal(source,"keep me");CHECK(!g_file_test(target,G_FILE_TEST_EXISTS));
    const char *peers[]={"job-disconnect-helper","job-bad-exit-helper"};
    for(guint i=0;i<G_N_ELEMENTS(peers);i++){
        char *peer=g_build_filename(fixture,peers[i],NULL);CHECK(!symlink(executable,peer));
        p=perform(new_job(peer,FW_MOVE,source,dst,FW_KEEP_BOTH,NULL));CHECK(p.state==FW_JOB_UNCONFIRMED);CHECK(strstr(p.message,"unconfirmed"));fw_job_progress_clear(&p);bytes_equal(source,"keep me");g_free(peer);
    }
    g_strfreev(env);g_free(src);g_free(dst);g_free(source);g_free(target);g_free(marker);g_free(release);
}
static void select_named(Pane *pane,const char *name,gboolean clear)
{
    for(guint i=0;i<g_list_model_get_n_items(G_LIST_MODEL(pane->sorted));i++){
        GFileInfo *info=g_list_model_get_item(G_LIST_MODEL(pane->sorted),i);gboolean match=!strcmp(g_file_info_get_name(info),name);g_object_unref(info);
        if(match){CHECK(gtk_selection_model_select_item(pane->selection,i,clear));return;}
    }CHECK(FALSE);
}
static void confirm_draft(App *app)
{
    CHECK(app->draft);GtkWidget *start=g_object_get_data(G_OBJECT(app->draft->window),"start-button");CHECK(start);g_signal_emit_by_name(start,"clicked");CHECK(!app->draft);CHECK(app->job);
}
static void wait_visible_count(Pane *pane,guint count)
{
    gint64 end=g_get_monotonic_time()+5000000;
    for(;;){pump();if(!fw_browser_busy(pane->browser)&&!gtk_sort_list_model_get_pending(pane->sorted)&&!gtk_filter_list_model_get_pending(pane->filtered)&&g_list_model_get_n_items(G_LIST_MODEL(pane->sorted))==count)return;CHECK(g_get_monotonic_time()<end);g_usleep(1000);}
}
static void test_operations_desktop_review_and_refresh(void)
{
    char *left=directory("ops-ui-left"),*right=directory("ops-ui-right");file(left,"one.txt","one");file(left,"bytes-\xff","two");
    App *app=create_window(NULL,helper,icon,left,right);g_strfreev(app->job_env);app->job_env=g_strdupv(operation_env);gtk_window_present(app->window);wait_panes(app);app->active=app->panes;
    select_named(app->panes,"one.txt",TRUE);select_named(app->panes,"bytes-\xff",FALSE);g_signal_emit_by_name(app->copy,"clicked");CHECK(app->draft);CHECK(app->draft->count==2);CHECK(!app->job);
    CHECK(!strcmp(app->draft->destination,right));draft_discard(app);char *out=g_build_filename(right,"one.txt",NULL);CHECK(!g_file_test(out,G_FILE_TEST_EXISTS));
    g_signal_emit_by_name(app->copy,"clicked");confirm_draft(app);FwJobProgress p=wait_job(app->job);CHECK(p.state==FW_JOB_COMPLETE);fw_job_progress_clear(&p);wait_panes(app);bytes_equal(out,"one");
    wait_visible_count(app->panes+1,2);wait_panes(app);
    GtkBitset *selected=gtk_selection_model_get_selection(app->panes[0].selection);CHECK(gtk_bitset_get_size(selected)==2);gtk_bitset_unref(selected);
    g_signal_emit_by_name(app->trash,"clicked");CHECK(app->draft);CHECK(app->draft->operation==FW_TRASH);confirm_draft(app);p=wait_job(app->job);CHECK(p.state==FW_JOB_COMPLETE);fw_job_progress_clear(&p);wait_visible_count(app->panes,0);wait_panes(app);
    app->active=app->panes+1;select_named(app->panes+1,"one.txt",TRUE);g_signal_emit_by_name(app->move,"clicked");confirm_draft(app);p=wait_job(app->job);CHECK(p.state==FW_JOB_COMPLETE);fw_job_progress_clear(&p);wait_visible_count(app->panes,1);wait_visible_count(app->panes+1,1);wait_panes(app);CHECK(!g_file_test(out,G_FILE_TEST_EXISTS));
    char *returned=g_build_filename(left,"one.txt",NULL);bytes_equal(returned,"one");save_snapshot(app);gtk_window_close(app->window);settle(60);g_free(returned);g_free(out);g_free(left);g_free(right);
}
static void test_operations_close_waits_for_cancel(void)
{
    char *left=directory("ops-close-left"),*right=directory("ops-close-right");file(left,"retain.txt","keep on close");char *source=g_build_filename(left,"retain.txt",NULL);
    char *marker,*release;char **env=pause_environment(right,&marker,&release);
    App *app=create_window(NULL,fault_helper,icon,left,right);g_strfreev(app->job_env);app->job_env=g_strdupv(env);gtk_window_present(app->window);wait_panes(app);app->active=app->panes;
    select_named(app->panes,"retain.txt",TRUE);g_signal_emit_by_name(app->copy,"clicked");confirm_draft(app);FwJob *job=g_object_ref(app->job);wait_marker(marker);
    gpointer weak=app->window;g_object_add_weak_pointer(G_OBJECT(app->window),&weak);gtk_window_close(app->window);CHECK(weak);CHECK(app->close_wait);wait_cancel_sent(job);CHECK(g_file_set_contents(release,"release",-1,NULL));
    FwJobProgress p=wait_job(job);CHECK(p.state==FW_JOB_CANCELLED);fw_job_progress_clear(&p);g_object_unref(job);settle(100);CHECK(!weak);bytes_equal(source,"keep on close");
    g_strfreev(env);g_free(left);g_free(right);g_free(source);g_free(marker);g_free(release);
}
static void test_names_literal_validation(void)
{
    const char *bad[]={"",".","..","../escape","a/b","a\\x2Fb","a\\x00b","bad\\q"};GError *error=NULL;
    for(guint i=0;i<G_N_ELEMENTS(bad);i++){CHECK(!fw_parse_name(bad[i],&error));CHECK(error);g_clear_error(&error);}
    const char raw[]="~ β $() \\ \xff\n";char *display=fw_display_bytes(raw),*name=fw_parse_name(display,&error);CHECK(!error);CHECK(!strcmp(name,raw));g_free(name);g_free(display);
    name=fw_parse_name("~",&error);CHECK(!error);CHECK(!strcmp(name,"~"));g_free(name);
    char long_name[257];memset(long_name,'a',256);long_name[256]=0;CHECK(!fw_parse_name(long_name,&error));g_clear_error(&error);
    long_name[255]=0;name=fw_parse_name(long_name,&error);CHECK(name);CHECK(strlen(name)==255);g_free(name);
}
static void test_names_helper_actions(void)
{
    char *root=directory("names-helper"),*source=g_build_filename(root,"old.txt",NULL),*target=g_build_filename(root,"Case β $(literal)%.txt",NULL);file(root,"old.txt","retain bytes");
    struct stat before,after;CHECK(!lstat(source,&before));FwJobProgress p=perform(new_job(helper,FW_RENAME,source,target,FW_FAIL_CONFLICT,NULL));CHECK(p.state==FW_JOB_COMPLETE);fw_job_progress_clear(&p);CHECK(!g_file_test(source,G_FILE_TEST_EXISTS));bytes_equal(target,"retain bytes");CHECK(!lstat(target,&after));CHECK(before.st_ino==after.st_ino);
    file(root,"existing","keep target");char *existing=g_build_filename(root,"existing",NULL);
    p=perform(new_job(helper,FW_RENAME,target,existing,FW_FAIL_CONFLICT,NULL));CHECK(p.state==FW_JOB_ERRORS);fw_job_progress_clear(&p);bytes_equal(existing,"keep target");bytes_equal(target,"retain bytes");
    p=perform(new_job(helper,FW_RENAME,target,target,FW_FAIL_CONFLICT,NULL));CHECK(p.state==FW_JOB_COMPLETE);CHECK(strstr(p.message,"unchanged"));fw_job_progress_clear(&p);
    char *folder=g_build_filename(root,"new-\xff",NULL);p=perform(new_job(helper,FW_MKDIR,folder,NULL,FW_FAIL_CONFLICT,NULL));CHECK(p.state==FW_JOB_COMPLETE);fw_job_progress_clear(&p);CHECK(g_file_test(folder,G_FILE_TEST_IS_DIR));
    p=perform(new_job(helper,FW_MKDIR,folder,NULL,FW_FAIL_CONFLICT,NULL));CHECK(p.state==FW_JOB_ERRORS);fw_job_progress_clear(&p);
    char *renamed=g_build_filename(root,"renamed-folder",NULL);p=perform(new_job(helper,FW_RENAME,folder,renamed,FW_FAIL_CONFLICT,NULL));CHECK(p.state==FW_JOB_COMPLETE);fw_job_progress_clear(&p);CHECK(g_file_test(renamed,G_FILE_TEST_IS_DIR));
    char *link=g_build_filename(root,"broken",NULL),*to=g_build_filename(root,"renamed-link",NULL);CHECK(!symlink("missing",link));p=perform(new_job(helper,FW_RENAME,link,to,FW_FAIL_CONFLICT,NULL));CHECK(p.state==FW_JOB_COMPLETE);fw_job_progress_clear(&p);CHECK(g_file_test(to,G_FILE_TEST_IS_SYMLINK));
    g_free(root);g_free(source);g_free(target);g_free(existing);g_free(folder);g_free(renamed);g_free(link);g_free(to);
}
static void test_names_helper_persistence_failures(void)
{
    char *root=directory("names-faults"),*source=g_build_filename(root,"old",NULL),*target=g_build_filename(root,"new",NULL),*ancestor=g_path_get_dirname(root);file(root,"old","keep");
    char **env=g_environ_setenv(g_strdupv(operation_env),"FW_OPS_FAIL_PATH",ancestor,TRUE);
    FwJobProgress p=perform(new_job(fault_helper,FW_RENAME,source,target,FW_FAIL_CONFLICT,env));CHECK(p.state==FW_JOB_ERRORS);fw_job_progress_clear(&p);bytes_equal(source,"keep");CHECK(!g_file_test(target,G_FILE_TEST_EXISTS));g_strfreev(env);
    env=g_environ_setenv(g_strdupv(operation_env),"FW_OPS_FAIL_PATH",root,TRUE);
    p=perform(new_job(fault_helper,FW_RENAME,source,target,FW_FAIL_CONFLICT,env));CHECK(p.state==FW_JOB_UNCONFIRMED);fw_job_progress_clear(&p);bytes_equal(target,"keep");g_strfreev(env);
    char *folder=g_build_filename(root,"folder",NULL);env=g_environ_setenv(g_strdupv(operation_env),"FW_OPS_FAIL_PATH",ancestor,TRUE);
    p=perform(new_job(fault_helper,FW_MKDIR,folder,NULL,FW_FAIL_CONFLICT,env));CHECK(p.state==FW_JOB_ERRORS);fw_job_progress_clear(&p);CHECK(!g_file_test(folder,G_FILE_TEST_EXISTS));g_strfreev(env);
    env=g_environ_setenv(g_strdupv(operation_env),"FW_OPS_FAIL_PATH",folder,TRUE);
    p=perform(new_job(fault_helper,FW_MKDIR,folder,NULL,FW_FAIL_CONFLICT,env));CHECK(p.state==FW_JOB_ERRORS);fw_job_progress_clear(&p);CHECK(g_file_test(folder,G_FILE_TEST_IS_DIR));g_strfreev(env);
    g_free(root);g_free(source);g_free(target);g_free(ancestor);g_free(folder);
}
static void submit_name(App *app,const char *name)
{
    CHECK(app->edit);gtk_editable_set_text(GTK_EDITABLE(app->edit->entry),name);g_signal_emit_by_name(app->edit->entry,"activate");
}
static void wait_selected_name(Pane *pane,const char *name)
{
    gint64 end=g_get_monotonic_time()+5000000;
    for(;;){
        pump();GtkBitset *selected=gtk_selection_model_get_selection(pane->selection);gboolean match=FALSE;
        if(gtk_bitset_get_size(selected)==1){GFileInfo *info=g_list_model_get_item(G_LIST_MODEL(pane->sorted),gtk_bitset_get_minimum(selected));if(info){match=!strcmp(g_file_info_get_name(info),name);g_object_unref(info);}}
        gtk_bitset_unref(selected);if(match&&!fw_browser_busy(pane->browser)&&!gtk_sort_list_model_get_pending(pane->sorted))return;
        CHECK(g_get_monotonic_time()<end);g_usleep(1000);
    }
}
static void test_names_desktop_rename(void)
{
    char *left=directory("names-ui-left"),*right=directory("names-ui-right");file(left,"old.txt","rename bytes");file(left,"existing.txt","original");
    App *app=create_window(NULL,helper,icon,left,right);gtk_window_present(app->window);wait_panes(app);Pane *pane=app->panes;app->active=pane;select_named(pane,"old.txt",TRUE);gtk_widget_grab_focus(pane->view);
    CHECK(key_pressed(NULL,GDK_KEY_F2,0,0,app));CHECK(app->edit);CHECK(!app->job);gtk_popover_popdown(GTK_POPOVER(app->edit->popover));settle(20);CHECK(!app->edit);
    GtkWidget *focus=gtk_window_get_focus(app->window);CHECK(focus&&(focus==pane->view||gtk_widget_is_ancestor(focus,pane->view)));
    char *old=g_build_filename(left,"old.txt",NULL);bytes_equal(old,"rename bytes");
    begin_name_edit(pane,FW_RENAME,TRUE);submit_name(app,"existing.txt");FwJobProgress p=wait_job(app->job);CHECK(p.state==FW_JOB_ERRORS);fw_job_progress_clear(&p);CHECK(app->edit);CHECK(gtk_widget_get_sensitive(app->edit->entry));CHECK(*gtk_label_get_text(GTK_LABEL(app->edit->error)));
    submit_name(app,"renamed-\\xFF.txt");p=wait_job(app->job);CHECK(p.state==FW_JOB_COMPLETE);fw_job_progress_clear(&p);CHECK(!app->edit);wait_visible_count(pane,2);
    char *renamed=g_build_filename(left,"renamed-\xff.txt",NULL);bytes_equal(renamed,"rename bytes");CHECK(!g_file_test(old,G_FILE_TEST_EXISTS));
    wait_selected_name(pane,"renamed-\xff.txt");wait_panes(app);select_named(pane,"existing.txt",FALSE);begin_name_edit(pane,FW_RENAME,TRUE);CHECK(!app->edit); /* one-item action */
    gtk_window_close(app->window);settle(50);g_free(old);g_free(renamed);g_free(left);g_free(right);
}
static void test_names_desktop_new_folder(void)
{
    char *left=directory("names-new-left"),*right=directory("names-new-right");char *existing=g_build_filename(left,"New folder",NULL);CHECK(!g_mkdir(existing,0700));
    App *app=create_window(NULL,helper,icon,left,right);gtk_window_present(app->window);wait_panes(app);app->active=app->panes;
    CHECK(key_pressed(NULL,GDK_KEY_N,0,GDK_CONTROL_MASK|GDK_SHIFT_MASK,app));CHECK(app->edit);CHECK(!strcmp(gtk_editable_get_text(GTK_EDITABLE(app->edit->entry)),"New folder (2)"));
    submit_name(app,"../escape");CHECK(!app->job);CHECK(app->edit);CHECK(*gtk_label_get_text(GTK_LABEL(app->edit->error)));gtk_popover_popdown(GTK_POPOVER(app->edit->popover));settle(20);CHECK(!app->edit);
    char *cancelled=g_build_filename(left,"New folder (2)",NULL);CHECK(!g_file_test(cancelled,G_FILE_TEST_EXISTS));
    begin_name_edit(app->panes,FW_MKDIR,TRUE);submit_name(app,"New folder");FwJobProgress p=wait_job(app->job);CHECK(p.state==FW_JOB_ERRORS);fw_job_progress_clear(&p);CHECK(app->edit);
    submit_name(app,"literal-~-$()-\\xFF");p=wait_job(app->job);CHECK(p.state==FW_JOB_COMPLETE);fw_job_progress_clear(&p);CHECK(!app->edit);wait_visible_count(app->panes,2);
    char *created=g_build_filename(left,"literal-~-$()-\xff",NULL);CHECK(g_file_test(created,G_FILE_TEST_IS_DIR));wait_selected_name(app->panes,"literal-~-$()-\xff");wait_panes(app);select_named(app->panes,"literal-~-$()-\xff",TRUE);
    begin_name_edit(app->panes,FW_MKDIR,TRUE);CHECK(app->edit);pane_navigate(app->panes,right);CHECK(!app->edit);wait_panes(app);
    begin_name_edit(app->panes,FW_MKDIR,TRUE);CHECK(app->edit);gtk_window_close(app->window);settle(50);
    g_free(created);g_free(cancelled);g_free(existing);g_free(left);g_free(right);
}

typedef struct QueryResult { gboolean done;FwDesktopTarget *target;GError *error; } QueryResult;
static void query_result(GObject *source,GAsyncResult *result,gpointer data)
{
    (void)source;QueryResult *q=data;q->target=fw_desktop_query_finish(result,&q->error);q->done=TRUE;
}
static void wait_flag(gboolean *done)
{
    gint64 end=g_get_monotonic_time()+5000000;
    while(!*done){pump();CHECK(g_get_monotonic_time()<end);g_usleep(1000);}
}
static FwDesktopTarget *query_target(const char *path,gboolean dir,gboolean apps)
{
    QueryResult q={0};fw_desktop_query_async(path,dir,apps,NULL,query_result,&q);wait_flag(&q.done);
    CHECK(!q.error);CHECK(q.target);return q.target;
}
static void test_desktop_target_paths_and_types(void)
{
    char *root=directory("desktop-targets"),*nested=g_build_filename(root,"nested",NULL),*inner=g_build_filename(nested,"inner",NULL);CHECK(!g_mkdir(nested,0700));CHECK(!g_mkdir(inner,0700));
    file(nested,"raw-\xff-%-$().txt","kernel resolved\n");file(root,"raw-\xff-%-$().txt","wrong lexical target\n");
    char *link=g_build_filename(root,"link",NULL);CHECK(!symlink("nested/inner",link));
    char *path=g_build_filename(link,"..","raw-\xff-%-$().txt",NULL);FwDesktopTarget *t=query_target(path,FALSE,FALSE);
    char *actual=g_build_filename(nested,"raw-\xff-%-$().txt",NULL),*uri=g_filename_to_uri(actual,NULL,NULL);
    CHECK(!strcmp(t->path,path));CHECK(!strcmp(t->uri,uri));CHECK(g_utf8_validate(t->uri,-1,NULL));CHECK(!strcmp(t->content_type,"text/plain"));fw_desktop_target_free(t);
    char *fifo=g_build_filename(root,"pipe",NULL);CHECK(!mkfifo(fifo,0600));
    QueryResult q={0};fw_desktop_query_async(fifo,FALSE,FALSE,NULL,query_result,&q);wait_flag(&q.done);CHECK(!q.target);CHECK(g_error_matches(q.error,G_IO_ERROR,G_IO_ERROR_NOT_SUPPORTED));g_clear_error(&q.error);
    q=(QueryResult){0};fw_desktop_query_async(root,FALSE,FALSE,NULL,query_result,&q);wait_flag(&q.done);CHECK(!q.target&&q.error);g_clear_error(&q.error);
    char *broken=g_build_filename(root,"broken",NULL);CHECK(!symlink("missing",broken));q=(QueryResult){0};fw_desktop_query_async(broken,FALSE,FALSE,NULL,query_result,&q);wait_flag(&q.done);CHECK(!q.target&&q.error);g_clear_error(&q.error);
    t=query_target(root,TRUE,FALSE);CHECK(!strcmp(t->path,root));fw_desktop_target_free(t);
    GCancellable *cancel=g_cancellable_new();g_cancellable_cancel(cancel);q=(QueryResult){0};fw_desktop_query_async(path,FALSE,TRUE,cancel,query_result,&q);wait_flag(&q.done);CHECK(!q.target);CHECK(g_error_matches(q.error,G_IO_ERROR,G_IO_ERROR_CANCELLED));g_clear_error(&q.error);g_object_unref(cancel);
    g_free(broken);g_free(fifo);g_free(actual);g_free(uri);g_free(path);g_free(link);g_free(inner);g_free(nested);g_free(root);
}
static char *desktop_capture;
static GAppInfo *make_desktop_app(const char *id,const char *tag)
{
    char *dir=g_build_filename(g_get_user_data_dir(),"applications",NULL);CHECK(!g_mkdir_with_parents(dir,0700));char *path=g_build_filename(dir,id,NULL);
    char *exec=g_strdup_printf("\"%s\" --desktop-capture %s %%f",executable,tag);
    GKeyFile *key=g_key_file_new();g_key_file_set_string(key,"Desktop Entry","Type","Application");g_key_file_set_string(key,"Desktop Entry","Name",tag);
    g_key_file_set_string(key,"Desktop Entry","Exec",exec);g_key_file_set_string(key,"Desktop Entry","MimeType","text/plain;");
    GError *error=NULL;CHECK(g_key_file_save_to_file(key,path,&error));CHECK(!error);
    GAppInfo *app=G_APP_INFO(g_desktop_app_info_new_from_filename(path));CHECK(app);
    g_key_file_unref(key);g_free(exec);g_free(path);g_free(dir);return app;
}
static void wait_open(App *app)
{
    gint64 end=g_get_monotonic_time()+5000000;
    while(app->opening&&!app->chooser){pump();CHECK(g_get_monotonic_time()<end);g_usleep(1000);}
}
static void expect_capture(const char *tag,const char *path)
{
    gint64 end=g_get_monotonic_time()+5000000;
    while(!g_file_test(desktop_capture,G_FILE_TEST_EXISTS)){pump();CHECK(g_get_monotonic_time()<end);g_usleep(1000);}
    char *data=NULL;gsize size=0;CHECK(g_file_get_contents(desktop_capture,&data,&size,NULL));
    GString *expected=g_string_new(tag);g_string_append_c(expected,0);g_string_append(expected,path);g_string_append_c(expected,0);
    CHECK(size==expected->len);CHECK(!memcmp(data,expected->str,size));g_free(data);g_string_free(expected,TRUE);CHECK(!g_unlink(desktop_capture));
}
static void test_desktop_default_and_chosen_open(void)
{
    GError *error=NULL;GAppInfo *standard=make_desktop_app("folderwing-test-default.desktop","default"),*chosen=make_desktop_app("folderwing-test-chosen.desktop","chosen");
    CHECK(g_app_info_set_as_default_for_type(standard,"text/plain",&error));CHECK(!error);CHECK(g_app_info_add_supports_type(chosen,"text/plain",&error));CHECK(!error);
    GAppInfo *current=g_app_info_get_default_for_type("text/plain",FALSE);CHECK(current);CHECK(g_app_info_equal(current,standard));g_object_unref(current);
    char *left=directory("desktop-open"),*right=directory("desktop-other");file(left,"literal-\xff-%-$().txt","document\n");char *path=g_build_filename(left,"literal-\xff-%-$().txt",NULL);
    App *app=create_window(NULL,helper,icon,left,right);gtk_window_present(app->window);wait_panes(app);select_named(app->panes,"literal-\xff-%-$().txt",TRUE);
    GtkBitset *selection=gtk_selection_model_get_selection(app->panes->selection);row_activate(GTK_COLUMN_VIEW(app->panes->view),gtk_bitset_get_minimum(selection),app->panes);gtk_bitset_unref(selection);wait_open(app);CHECK(!app->opening);CHECK(!gtk_widget_has_css_class(app->desktop_status,"error"));expect_capture("default",path);
    show_context(app->panes,"literal-\xff-%-$().txt",20,60);CHECK(app->panes->context);g_signal_emit_by_name(app->panes->context->buttons[MENU_OPEN_WITH],"clicked");wait_open(app);CHECK(app->chooser);
    GtkListBox *list=g_object_get_data(G_OBJECT(app->chooser),"applications");GtkListBoxRow *picked=NULL;
    for(int i=0;;i++){GtkListBoxRow *row=gtk_list_box_get_row_at_index(list,i);if(!row)break;GAppInfo *candidate=g_object_get_data(G_OBJECT(row),"application");if(g_app_info_equal(candidate,chosen)){picked=row;break;}}
    CHECK(picked);gtk_list_box_select_row(list,picked);chooser_open(NULL,app->chooser);wait_open(app);CHECK(!app->opening);expect_capture("chosen",path);
    current=g_app_info_get_default_for_type("text/plain",FALSE);CHECK(current);CHECK(g_app_info_equal(current,standard));g_object_unref(current);
    open_path(app->panes,path,1);wait_open(app);CHECK(app->chooser);gtk_window_close(app->chooser);settle(30);CHECK(!app->chooser&&!app->opening);CHECK(!strcmp(gtk_label_get_text(GTK_LABEL(app->desktop_status)),"Open cancelled."));CHECK(!g_file_test(desktop_capture,G_FILE_TEST_EXISTS));
    /* Failure is visible and a pending discovery can be closed safely. */
    char *missing=g_build_filename(left,"missing",NULL);open_path(app->panes,missing,0);wait_open(app);CHECK(gtk_widget_has_css_class(app->desktop_status,"error"));
    open_path(app->panes,path,1);gtk_window_close(app->window);settle(80);CHECK(!g_file_test(desktop_capture,G_FILE_TEST_EXISTS));
    g_free(missing);g_free(path);g_free(left);g_free(right);g_object_unref(chosen);g_object_unref(standard);
}
static void test_desktop_terminal_literal_command(void)
{
    const char *path="/tmp/a 'quote' % dollar$() \\ raw-\xff";GError *error=NULL;
    GAppInfo *info=fw_desktop_terminal_app(helper,path,&error);CHECK(info&&!error);
    int argc=0;char **argv=NULL;CHECK(g_shell_parse_argv(g_app_info_get_commandline(info),&argc,&argv,&error));CHECK(!error);CHECK(argc>=3);CHECK(!strcmp(argv[0],helper));CHECK(!strcmp(argv[1],"--shell"));
    GString *hex=g_string_new("x");for(const unsigned char *at=(const unsigned char *)path;*at;at++)g_string_append_printf(hex,"%02x",*at);
    CHECK(!strcmp(argv[2],hex->str));g_string_free(hex,TRUE);g_strfreev(argv);g_object_unref(info);
    CHECK(!fw_desktop_terminal_app(helper,"relative",&error));CHECK(error);g_clear_error(&error);
}
static guint check_context_cells(Pane *pane,GtkWidget *widget,const char *name)
{
    guint count=0;const char *bound=g_object_get_data(G_OBJECT(widget),"fw-row");
    if(bound&&!strcmp(bound,name)&&gtk_widget_get_mapped(widget)){
        graphene_rect_t bounds;CHECK(gtk_widget_compute_bounds(widget,pane->view,&bounds));
        const char *picked=context_row_at(pane,bounds.origin.x+bounds.size.width/2,bounds.origin.y+bounds.size.height/2);
        CHECK(picked&&!strcmp(picked,name));count++;
    }
    for(GtkWidget *child=gtk_widget_get_first_child(widget);child;child=gtk_widget_get_next_sibling(child))count+=check_context_cells(pane,child,name);
    return count;
}
static void context_click(Pane *pane,const char *name)
{
    CHECK(check_context_cells(pane,pane->view,name)==3);
    GtkWidget *cell=find_name_cell(pane->view,name);CHECK(cell);graphene_rect_t bounds;CHECK(gtk_widget_compute_bounds(cell,pane->view,&bounds));
    double x=bounds.origin.x+bounds.size.width/2,y=bounds.origin.y+bounds.size.height/2;
    CHECK(!strcmp(context_row_at(pane,x,y),name));
    GListModel *controllers=gtk_widget_observe_controllers(pane->view);gboolean emitted=FALSE;
    for(guint i=0;i<g_list_model_get_n_items(controllers);i++){
        GObject *controller=g_list_model_get_item(controllers,i);
        if(GTK_IS_GESTURE_CLICK(controller)&&gtk_gesture_single_get_button(GTK_GESTURE_SINGLE(controller))==GDK_BUTTON_SECONDARY){g_signal_emit_by_name(controller,"pressed",1,x,y);emitted=TRUE;}
        g_object_unref(controller);
    }
    g_object_unref(controllers);CHECK(emitted);CHECK(pane->context);
}
static void test_context_selection_and_operations(void)
{
    char *left=directory("context-left"),*right=directory("context-right");file(left,"a.txt","a");file(left,"b.txt","b");file(left,"raw-\xff","raw");
    App *app=create_window(NULL,helper,icon,left,right);gtk_window_present(app->window);wait_panes(app);Pane *pane=app->panes;settle(30);
    select_named(pane,"a.txt",TRUE);select_named(pane,"b.txt",FALSE);context_click(pane,"a.txt");CHECK(pane->context->count==2);CHECK(!gtk_widget_get_sensitive(pane->context->buttons[MENU_RENAME]));CHECK(!gtk_widget_get_sensitive(pane->context->buttons[MENU_OPEN]));
    g_signal_emit_by_name(pane->context->buttons[MENU_COPY],"clicked");CHECK(!pane->context);CHECK(app->draft&&app->draft->count==2);CHECK(!strcmp(app->draft->destination,right));draft_discard(app);settle(30);
    context_click(pane,"raw-\xff");CHECK(pane->context->count==1);CHECK(!strcmp(pane->context->names[0],"raw-\xff"));g_signal_emit_by_name(pane->context->buttons[MENU_RENAME],"clicked");CHECK(app->edit);CHECK(!strcmp(gtk_editable_get_text(GTK_EDITABLE(app->edit->entry)),"raw-\\xFF"));edit_discard(app);settle(30);
    show_context(pane,"a.txt",20,60);g_signal_emit_by_name(pane->context->buttons[MENU_MOVE],"clicked");CHECK(app->draft&&app->draft->operation==FW_MOVE);draft_discard(app);settle(30);
    show_context(pane,"b.txt",20,60);g_signal_emit_by_name(pane->context->buttons[MENU_TRASH],"clicked");CHECK(app->draft&&app->draft->operation==FW_TRASH);draft_discard(app);settle(30);
    show_context(pane,"b.txt",20,60);select_named(pane,"a.txt",TRUE);CHECK(!pane->context);
    gtk_widget_grab_focus(pane->view);CHECK(key_pressed(NULL,GDK_KEY_F10,0,GDK_SHIFT_MASK,app));CHECK(pane->context);CHECK(!strcmp(pane->context->names[0],"a.txt"));
    guint rev=fw_browser_revision(pane->browser);fw_browser_refresh(pane->browser);CHECK(wait_revision(pane->browser,rev));wait_panes(app);CHECK(!pane->context);
    show_context(pane,"b.txt",20,60);pane_navigate(pane,right);CHECK(!pane->context);wait_panes(app);gtk_window_close(app->window);settle(50);g_free(left);g_free(right);
}
static void provider_written(GObject *source,GAsyncResult *result,gpointer data)
{
    QueryResult *q=data;CHECK(gdk_content_provider_write_mime_type_finish(GDK_CONTENT_PROVIDER(source),result,&q->error));q->done=TRUE;
}
static void test_context_background_and_paths(void)
{
    char *left=directory("context-background"),*right=directory("context-background-right");file(left,"raw-\xff-\n-%","literal");char *folder=g_build_filename(left,"folder",NULL);CHECK(!g_mkdir(folder,0700));
    App *app=create_window(NULL,helper,icon,left,right);gtk_window_present(app->window);wait_panes(app);Pane *pane=app->panes;select_named(pane,"raw-\xff-\n-%",TRUE);
    show_context(pane,"raw-\xff-\n-%",20,60);CHECK(pane->context);GdkContentProvider *provider=path_content(pane->context);GValue value=G_VALUE_INIT;g_value_init(&value,G_TYPE_STRING);CHECK(gdk_content_provider_get_value(provider,&value,NULL));
    char *path=g_build_filename(left,"raw-\xff-\n-%",NULL),*display=fw_display_bytes(path),*uri=g_filename_to_uri(path,NULL,NULL);CHECK(!strcmp(g_value_get_string(&value),display));g_value_unset(&value);
    GOutputStream *output=g_memory_output_stream_new_resizable();QueryResult q={0};gdk_content_provider_write_mime_type_async(provider,"text/uri-list",output,0,NULL,provider_written,&q);wait_flag(&q.done);CHECK(!q.error);CHECK(g_output_stream_close(output,NULL,NULL));
    GBytes *bytes=g_memory_output_stream_steal_as_bytes(G_MEMORY_OUTPUT_STREAM(output));gsize n;const void *data=g_bytes_get_data(bytes,&n);char *expected=g_strconcat(uri,"\r\n",NULL);CHECK(n==strlen(expected));CHECK(!memcmp(data,expected,n));g_bytes_unref(bytes);g_object_unref(output);g_object_unref(provider);
    show_context(pane,NULL,30,180);CHECK(pane->context&&!pane->context->count);CHECK(!pane->context->buttons[MENU_TRASH]);CHECK(pane->context->buttons[MENU_TERMINAL]);g_signal_emit_by_name(pane->context->buttons[MENU_NEW],"clicked");CHECK(app->edit&&app->edit->operation==FW_MKDIR);edit_discard(app);settle(30);
    show_context(pane,"folder",20,60);CHECK(pane->context->buttons[MENU_TERMINAL]);g_signal_emit_by_name(pane->context->buttons[MENU_OPEN],"clicked");wait_panes(app);CHECK(!strcmp(fw_browser_path(pane->browser),folder));
    show_context(pane,NULL,30,180);CHECK(pane->context);gtk_window_close(app->window);settle(50);
    g_free(expected);g_free(uri);g_free(display);g_free(path);g_free(folder);g_free(left);g_free(right);
}

static int peer_read(void *data,size_t size)
{
    char *at=data;while(size){ssize_t n=read(0,at,size);if(n<=0)return 0;at+=n;size-=(size_t)n;}return 1;
}
static int job_peer(gboolean bad_exit)
{
    wslp_head h={WSLP_HELLO,sizeof(wslp_hello),0};wslp_hello hi={WSLP_MAGIC,WSLP_VERSION,getuid(),getpid()};
    if(write(1,&h,sizeof h)!=sizeof h||write(1,&hi,sizeof hi)!=sizeof hi)return 1;
    if(!peer_read(&h,sizeof h)||h.size>WSLP_JOB_MAX)return 1;
    char *body=g_malloc(h.size);int ok=peer_read(body,h.size);g_free(body);if(!ok)return 1;
    wslp_progress p={.state=2,.root=WSLP_NO_ROOT};h=(wslp_head){WSLP_JOB_PROGRESS,sizeof p,1};
    if(write(1,&h,sizeof h)!=sizeof h||write(1,&p,sizeof p)!=sizeof p)return 1;
    if(!peer_read(&h,sizeof h)||h.type!=WSLP_JOB_CONTINUE)return 1;
    if(bad_exit){p.state=3;h=(wslp_head){WSLP_JOB_PROGRESS,sizeof p,1};if(write(1,&h,sizeof h)!=sizeof h||write(1,&p,sizeof p)!=sizeof p)return 1;return 7;}return 0;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    if(argc>=3&&!strcmp(argv[1],"--desktop-capture")){
        const char *capture=g_getenv("FW_DESKTOP_CAPTURE");if(!capture)return 2;
        GString *record=g_string_new(NULL);for(int i=2;i<argc;i++){g_string_append(record,argv[i]);g_string_append_c(record,0);}
        gboolean ok=g_file_set_contents(capture,record->str,record->len,NULL);g_string_free(record,TRUE);return ok?0:1;
    }
    char *name = g_path_get_basename(argv[0]);
    if (!strcmp(name,"job-disconnect-helper")||!strcmp(name,"job-bad-exit-helper")) { gboolean bad=!strcmp(name,"job-bad-exit-helper");g_free(name);return job_peer(bad); }
    if (!strcmp(name, "bad-helper")) { wslp_head h = {WSLP_HELLO, WSLP_MAX+1, 0}; if (write(1, &h, sizeof h) != sizeof h) return 1; g_free(name); return 0; }
    if (!strcmp(name, "disconnect-helper")) { g_free(name); return 0; }
    if (!strcmp(name, "truncated-helper")) { wslp_head h = {WSLP_HELLO, sizeof(wslp_hello), 0}; if (write(1, &h, sizeof h) != sizeof h) return 1; if (write(1, "short", 5) != 5) return 1; g_free(name); return 0; }
    g_free(name); if (argc != 3) { g_printerr("Usage: tests HELPER ICON\n"); return 2; }
    helper = argv[1]; icon = argv[2]; char *self = g_file_read_link("/proc/self/exe", NULL); CHECK(self); executable = self;
    fixture = g_dir_make_tmp("folderwing-linux-tests-XXXXXX", NULL); CHECK(fixture);
    /* Keep MIME defaults/desktop registration entirely inside owned fixtures. */
    char *desktop_data=g_build_filename(fixture,"desktop-data",NULL),*desktop_config=g_build_filename(fixture,"desktop-config",NULL);
    CHECK(!g_mkdir(desktop_data,0700));CHECK(!g_mkdir(desktop_config,0700));g_setenv("XDG_DATA_HOME",desktop_data,TRUE);g_setenv("XDG_CONFIG_HOME",desktop_config,TRUE);
    desktop_capture=g_build_filename(fixture,"desktop-capture",NULL);g_setenv("FW_DESKTOP_CAPTURE",desktop_capture,TRUE);g_free(desktop_data);g_free(desktop_config);
    char *data=g_build_filename(fixture,"job-data",NULL);operation_env=g_environ_setenv(g_get_environ(),"XDG_DATA_HOME",data,TRUE);g_free(data);
    char *helper_dir=g_path_get_dirname(helper);fault_helper=g_build_filename(helper_dir,"folderwing-linux-helper-faults",NULL);g_free(helper_dir);gtk_init();
    struct { const char *name; void (*run)(void); } cases[] = {
        {"test_literal_paths", test_literal_paths}, {"test_native_listing_metadata", test_native_listing_metadata},
        {"test_live_watches_independent", test_live_watches_independent}, {"test_navigation_failure_and_supersession", test_navigation_failure_and_supersession},
        {"test_protocol_disconnect_and_close", test_protocol_disconnect_and_close}, {"test_two_pane_desktop", test_two_pane_desktop},
        {"test_large_listing_and_cleanup", test_large_listing_and_cleanup},
        {"test_operations_copy_move_conflicts",test_operations_copy_move_conflicts},
        {"test_operations_cross_device_move_barriers",test_operations_cross_device_move_barriers},
        {"test_operations_trash_durability",test_operations_trash_durability},
        {"test_operations_cancel_and_disconnect",test_operations_cancel_and_disconnect},
        {"test_operations_desktop_review_and_refresh",test_operations_desktop_review_and_refresh},
        {"test_operations_close_waits_for_cancel",test_operations_close_waits_for_cancel},
        {"test_names_literal_validation",test_names_literal_validation},
        {"test_names_helper_actions",test_names_helper_actions},
        {"test_names_helper_persistence_failures",test_names_helper_persistence_failures},
        {"test_names_desktop_rename",test_names_desktop_rename},
        {"test_names_desktop_new_folder",test_names_desktop_new_folder},
        {"test_desktop_target_paths_and_types",test_desktop_target_paths_and_types},
        {"test_desktop_default_and_chosen_open",test_desktop_default_and_chosen_open},
        {"test_desktop_terminal_literal_command",test_desktop_terminal_literal_command},
        {"test_context_selection_and_operations",test_context_selection_and_operations},
        {"test_context_background_and_paths",test_context_background_and_paths}
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        gint64 start = g_get_monotonic_time(); cases[i].run();
        g_print("PASS %s %.1f ms\n", cases[i].name, (g_get_monotonic_time()-start)/1000.0);
    }
    remove_owned_tree(fixture); g_free(fixture); g_free(self); g_free(fault_helper);g_free(desktop_capture);g_strfreev(operation_env);
    g_print("PASS %u selectors; %u assertions; zero skips\n", (guint)G_N_ELEMENTS(cases), assertions); return 0;
}
