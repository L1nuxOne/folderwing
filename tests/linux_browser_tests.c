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

static const char *helper, *icon, *executable;
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
    CHECK(g_list_model_get_n_items(G_LIST_MODEL(app->panes[0].sorted)) == 10000);
    CHECK(g_list_model_get_n_items(G_LIST_MODEL(app->panes[1].sorted)) == 10000);
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
int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    char *name = g_path_get_basename(argv[0]);
    if (!strcmp(name, "bad-helper")) { wslp_head h = {WSLP_HELLO, WSLP_MAX+1, 0}; if (write(1, &h, sizeof h) != sizeof h) return 1; g_free(name); return 0; }
    if (!strcmp(name, "disconnect-helper")) { g_free(name); return 0; }
    if (!strcmp(name, "truncated-helper")) { wslp_head h = {WSLP_HELLO, sizeof(wslp_hello), 0}; if (write(1, &h, sizeof h) != sizeof h) return 1; if (write(1, "short", 5) != 5) return 1; g_free(name); return 0; }
    g_free(name); if (argc != 3) { g_printerr("Usage: tests HELPER ICON\n"); return 2; }
    helper = argv[1]; icon = argv[2]; char *self = g_file_read_link("/proc/self/exe", NULL); CHECK(self); executable = self;
    fixture = g_dir_make_tmp("folderwing-linux-tests-XXXXXX", NULL); CHECK(fixture); gtk_init();
    struct { const char *name; void (*run)(void); } cases[] = {
        {"test_literal_paths", test_literal_paths}, {"test_native_listing_metadata", test_native_listing_metadata},
        {"test_live_watches_independent", test_live_watches_independent}, {"test_navigation_failure_and_supersession", test_navigation_failure_and_supersession},
        {"test_protocol_disconnect_and_close", test_protocol_disconnect_and_close}, {"test_two_pane_desktop", test_two_pane_desktop},
        {"test_large_listing_and_cleanup", test_large_listing_and_cleanup}
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        gint64 start = g_get_monotonic_time(); cases[i].run();
        g_print("PASS %s %.1f ms\n", cases[i].name, (g_get_monotonic_time()-start)/1000.0);
    }
    remove_owned_tree(fixture); g_free(fixture); g_free(self);
    g_print("PASS %u selectors; %u assertions; zero skips\n", (guint)G_N_ELEMENTS(cases), assertions); return 0;
}
