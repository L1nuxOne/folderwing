#include <gtk/gtk.h>
#include <sys/stat.h>
#include <string.h>
#include <signal.h>
#include "model.h"
#include "operations.h"
#include "desktop.h"

typedef struct App App;
typedef struct Draft Draft;
typedef struct NameEdit NameEdit;
typedef struct ContextMenu ContextMenu;
typedef struct Pane {
    App *app;
    FwBrowser *browser;
    GtkWidget *box, *address, *view, *status, *spinner, *back, *forward, *rename_button, *new_button;
    GtkFilterListModel *filtered;
    GtkSortListModel *sorted;
    GtkSelectionModel *selection;
    GPtrArray *history;
    guint history_at, revision;
    gint history_target;
    gboolean busy, requested, restore_selection;
    GHashTable *selected_names;
    char *select_after, *select_root;
    ContextMenu *context;
} Pane;
struct App {
    GtkWindow *window; Pane panes[2]; Pane *active; gboolean hidden, closing, close_wait, close_queued, job_reported;
    char *helper, **job_env; FwJob *job; Draft *draft; NameEdit *edit;
    GtkWidget *copy, *move, *trash, *cancel, *job_status, *job_progress, *job_details, *details;
    GCancellable *launch_cancel; GtkWindow *chooser; GtkWidget *desktop_status;gboolean opening;
};
static void update_actions(App *app);
static void draft_discard(App *app);
static void edit_discard(App *app);
static void edit_finished(App *app, const FwJobProgress *progress);
static void begin_name_edit(Pane *pane, FwOperation operation, gboolean popup);
static void pane_update(Pane *pane);
static void pane_navigate(Pane *pane, const char *path);
static void context_discard(Pane *pane);
static void show_context(Pane *pane,const char *name,double x,double y);
static void open_path(Pane *pane,const char *path,guint purpose);
static void app_stop(App *app)
{
    app->closing = TRUE;
    draft_discard(app); edit_discard(app);
    if(app->launch_cancel)g_cancellable_cancel(app->launch_cancel);
    if(app->chooser){GtkWindow *chooser=app->chooser;app->chooser=NULL;gtk_window_destroy(chooser);}
    if (app->job) { g_signal_handlers_disconnect_by_data(app->job, app); fw_job_cancel(app->job); }
    for (guint i = 0; i < 2; i++) {
        Pane *pane = &app->panes[i];
        context_discard(pane);
        if (pane->selection) g_signal_handlers_disconnect_by_data(pane->selection, pane);
        if (pane->sorted) g_signal_handlers_disconnect_by_data(pane->sorted, pane);
        if (pane->filtered) g_signal_handlers_disconnect_by_data(pane->filtered, pane);
        if (pane->browser) {
            g_signal_handlers_disconnect_by_data(pane->browser, pane);
            fw_browser_close(pane->browser);
        }
    }
}
static void app_free(gpointer data)
{
    App *app = data;
    app_stop(app);
    for (guint i = 0; i < 2; i++) {
        Pane *pane = &app->panes[i];
        if (pane->browser) {
            g_object_unref(pane->browser);
        }
        g_clear_object(&pane->selection); g_clear_object(&pane->sorted); g_clear_object(&pane->filtered);
        if (pane->history) g_ptr_array_unref(pane->history);
        g_clear_pointer(&pane->selected_names, g_hash_table_unref); g_free(pane->select_after); g_free(pane->select_root);
    }
    g_clear_object(&app->launch_cancel);g_clear_object(&app->job); g_free(app->helper); g_strfreev(app->job_env); g_free(app);
}
static gint compare_name(gconstpointer a, gconstpointer b, gpointer unused)
{
    (void)unused;
    gint order = strcmp(g_object_get_data(G_OBJECT(a), "sort-key"), g_object_get_data(G_OBJECT(b), "sort-key"));
    if (!order) order = strcmp(g_file_info_get_name(G_FILE_INFO((gpointer)a)), g_file_info_get_name(G_FILE_INFO((gpointer)b)));
    return (order > 0) - (order < 0);
}
static gint compare_dirs(gconstpointer a, gconstpointer b, gpointer unused)
{
    (void)unused;
    return fw_entry_directory(G_FILE_INFO((gpointer)b))-fw_entry_directory(G_FILE_INFO((gpointer)a));
}
static gint compare_numeric(gconstpointer a, gconstpointer b, gpointer field)
{
    gint64 x, y;
    if (GPOINTER_TO_UINT(field) == 1) {
        x = g_file_info_get_size(G_FILE_INFO((gpointer)a)); y = g_file_info_get_size(G_FILE_INFO((gpointer)b));
    } else {
        x = g_file_info_get_attribute_int64(G_FILE_INFO((gpointer)a), "fw::mtime");
        y = g_file_info_get_attribute_int64(G_FILE_INFO((gpointer)b), "fw::mtime");
    }
    return (x > y) - (x < y);
}
static gboolean filter_entry(gpointer item, gpointer data)
{
    Pane *pane = data;
    return pane->app->hidden || g_file_info_get_name(item)[0] != '.';
}
static void setup_cell(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer field)
{
    (void)factory;
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), GPOINTER_TO_UINT(field) == 1 ? 1 : 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_margin_top(label, 6); gtk_widget_set_margin_bottom(label, 6);
    gtk_widget_set_margin_start(label, 9); gtk_widget_set_margin_end(label, 9);
    if (!GPOINTER_TO_UINT(field)) {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5), *icon = gtk_image_new();
        gtk_widget_set_margin_start(icon, 8); gtk_widget_set_hexpand(label, TRUE);
        gtk_box_append(GTK_BOX(box), icon); gtk_box_append(GTK_BOX(box), label);
        gtk_list_item_set_child(item, box);
    } else gtk_list_item_set_child(item, label);
}
static void bind_cell(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer field)
{
    (void)factory;
    GFileInfo *info = gtk_list_item_get_item(item);
    GtkWidget *child = gtk_list_item_get_child(item), *label = child;
    g_object_set_data_full(G_OBJECT(child),"fw-row",g_strdup(g_file_info_get_name(info)),g_free);
    guint column = GPOINTER_TO_UINT(field);
    if (!column) {
        GtkWidget *icon = gtk_widget_get_first_child(child); label = gtk_widget_get_next_sibling(icon);
        const char *name = fw_entry_directory(info) ? "folder-symbolic" : "text-x-generic-symbolic";
        if (g_file_info_get_attribute_uint32(info, "fw::flags") & 2) name = "dialog-warning-symbolic";
        gtk_image_set_from_icon_name(GTK_IMAGE(icon), name);
        gtk_label_set_text(GTK_LABEL(label), g_file_info_get_display_name(info));
        g_object_set_data_full(G_OBJECT(child), "fw-name", g_strdup(g_file_info_get_name(info)), g_free);
        guint mode = g_file_info_get_attribute_uint32(info, "unix::mode");
        const char *target = g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET) ? g_file_info_get_symlink_target(info) : NULL;
        char *link = target ? fw_display_bytes(target) : g_strdup("");
        char *tip = g_strdup_printf("%s\nMode %04o · UID %u · GID %u%s%s", g_file_info_get_display_name(info), mode & 07777,
            g_file_info_get_attribute_uint32(info, "unix::uid"), g_file_info_get_attribute_uint32(info, "unix::gid"), target ? "\nLink → " : "", link);
        gtk_widget_set_tooltip_text(child, tip); g_free(tip); g_free(link);
    } else if (column == 1) {
        char *size = fw_entry_directory(info) ? g_strdup("—") : g_format_size_full(g_file_info_get_size(info), G_FORMAT_SIZE_IEC_UNITS);
        gtk_label_set_text(GTK_LABEL(label), size); g_free(size);
    } else {
        gint64 stamp = g_file_info_get_attribute_int64(info, "fw::mtime");
        GDateTime *date = g_date_time_new_from_unix_local(stamp/1000000000);
        char *text = date ? g_date_time_format(date, "%Y-%m-%d %H:%M") : g_strdup("—");
        gtk_label_set_text(GTK_LABEL(label), text); g_free(text); if (date) g_date_time_unref(date);
    }
}
static void unbind_cell(GtkSignalListItemFactory *factory,GtkListItem *item,gpointer unused)
{
    (void)factory;(void)unused;GtkWidget *child=gtk_list_item_get_child(item);
    g_object_set_data(G_OBJECT(child),"fw-row",NULL);g_object_set_data(G_OBJECT(child),"fw-name",NULL);
}
static GtkColumnViewColumn *make_column(Pane *pane, const char *name, guint field)
{
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(setup_cell), GUINT_TO_POINTER(field));
    g_signal_connect(factory, "bind", G_CALLBACK(bind_cell), GUINT_TO_POINTER(field));
    g_signal_connect(factory,"unbind",G_CALLBACK(unbind_cell),NULL);
    GtkColumnViewColumn *column = gtk_column_view_column_new(name, factory);
    gtk_column_view_column_set_resizable(column, TRUE);
    gtk_column_view_column_set_expand(column, field == 0);
    gtk_column_view_column_set_fixed_width(column, field == 0 ? 260 : field == 1 ? 95 : 160);
    GtkSorter *sorter = GTK_SORTER(gtk_custom_sorter_new(field ? compare_numeric : compare_name, GUINT_TO_POINTER(field), NULL));
    gtk_column_view_column_set_sorter(column, sorter); g_object_unref(sorter);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(pane->view), column);
    return column;
}
static void update_history(Pane *pane)
{
    const char *path = fw_browser_path(pane->browser);
    if (pane->history_target >= 0) pane->history_at = (guint)pane->history_target;
    else if (!pane->history->len || strcmp(path, pane->history->pdata[pane->history_at])) {
        if (pane->history->len) g_ptr_array_set_size(pane->history, pane->history_at+1);
        g_ptr_array_add(pane->history, g_strdup(path)); pane->history_at = pane->history->len-1;
    }
    pane->history_target = -1;
}
static void pane_update(Pane *pane)
{
    if (pane->app->closing) return;
    gboolean busy = fw_browser_busy(pane->browser);
    if(busy)context_discard(pane);
    if (busy && !pane->busy) {
        g_clear_pointer(&pane->selected_names, g_hash_table_unref);
        if (!pane->requested) {
            pane->selected_names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
            GtkBitset *selected = gtk_selection_model_get_selection(pane->selection);
            GtkBitsetIter iter; guint position;
            for (gboolean more = gtk_bitset_iter_init_first(&iter, selected, &position); more; more = gtk_bitset_iter_next(&iter, &position)) {
                GFileInfo *info = g_list_model_get_item(G_LIST_MODEL(pane->sorted), position);
                if (info) { g_hash_table_add(pane->selected_names, g_strdup(g_file_info_get_name(info))); g_object_unref(info); }
            }
            gtk_bitset_unref(selected);
        }
    }
    pane->busy = busy;
    guint revision = fw_browser_revision(pane->browser);
    gboolean committed = revision != pane->revision;
    if (committed) {
        pane->revision = revision; update_history(pane); pane->restore_selection = pane->selected_names && g_hash_table_size(pane->selected_names);
    }
    if (committed || (!busy && pane->requested && *fw_browser_path(pane->browser))) {
        char *display = fw_display_bytes(fw_browser_path(pane->browser));
        gtk_editable_set_text(GTK_EDITABLE(pane->address), display); g_free(display);
    }
    if (!busy) { pane->requested = FALSE; pane->history_target = -1; }
    guint pending = gtk_sort_list_model_get_pending(pane->sorted) + gtk_filter_list_model_get_pending(pane->filtered);
    gtk_widget_set_sensitive(pane->view, !busy && !pending);
    if (busy || pending) gtk_spinner_start(GTK_SPINNER(pane->spinner)); else gtk_spinner_stop(GTK_SPINNER(pane->spinner));
    gtk_widget_set_sensitive(pane->back, pane->history->len && pane->history_at > 0);
    gtk_widget_set_sensitive(pane->forward, pane->history->len && pane->history_at+1 < pane->history->len);
    guint visible = g_list_model_get_n_items(G_LIST_MODEL(pane->sorted));
    if (!busy && !pending && pane->restore_selection) {
        pane->restore_selection = FALSE;
        GtkBitset *selected = gtk_bitset_new_empty(), *mask = gtk_bitset_new_range(0, visible);
        for (guint i = 0; i < visible; i++) {
            GFileInfo *item = g_list_model_get_item(G_LIST_MODEL(pane->sorted), i);
            if (g_hash_table_contains(pane->selected_names, g_file_info_get_name(item))) gtk_bitset_add(selected, i);
            g_object_unref(item);
        }
        gtk_selection_model_set_selection(pane->selection, selected, mask);
        gtk_bitset_unref(selected); gtk_bitset_unref(mask);
    }
    if (!busy && !pending && pane->select_after) {
        if (strcmp(fw_browser_path(pane->browser), pane->select_root)) { g_clear_pointer(&pane->select_after,g_free); g_clear_pointer(&pane->select_root,g_free); }
        else for (guint i=0;i<visible;i++) {
            GFileInfo *info=g_list_model_get_item(G_LIST_MODEL(pane->sorted),i);
            gboolean match=!strcmp(g_file_info_get_name(info),pane->select_after);g_object_unref(info);
            if(match){gtk_selection_model_select_item(pane->selection,i,TRUE);g_clear_pointer(&pane->select_after,g_free);g_clear_pointer(&pane->select_root,g_free);break;}
        }
    }
    char *message;
    const char *error = fw_browser_error(pane->browser);
    if (*error) message = g_strdup(error);
    else if (busy) message = g_strdup("Loading folder…");
    else if (pending) message = g_strdup_printf("Arranging %u items…", pending);
    else message = g_strdup_printf("%u items · %.1f ms listing · %s", visible, fw_browser_elapsed_ms(pane->browser),
        fw_browser_watching(pane->browser) ? "Live" : "Not watching");
    gtk_label_set_text(GTK_LABEL(pane->status), message); gtk_widget_set_tooltip_text(pane->status, message);
    if (*error) gtk_widget_add_css_class(pane->status, "error"); else gtk_widget_remove_css_class(pane->status, "error");
    g_free(message); update_actions(pane->app);
}
static void model_changed(FwBrowser *browser, gpointer data) { (void)browser; pane_update(data); }
static void pending_changed(GObject *object, GParamSpec *spec, gpointer data) { (void)object; (void)spec; pane_update(data); }
static void pane_navigate(Pane *pane, const char *path)
{
    context_discard(pane);
    if (!*fw_browser_path(pane->browser)) {
        char *display = fw_display_bytes(path);
        gtk_editable_set_text(GTK_EDITABLE(pane->address), display); g_free(display);
    }
    if (pane->app->edit) edit_discard(pane->app);
    g_clear_pointer(&pane->select_after,g_free);g_clear_pointer(&pane->select_root,g_free);
    pane->requested = TRUE; g_clear_pointer(&pane->selected_names, g_hash_table_unref);
    fw_browser_navigate(pane->browser, path);
}
static void address_activate(GtkEntry *entry, gpointer data)
{
    Pane *pane = data; GError *error = NULL;
    char *path = fw_parse_path(gtk_editable_get_text(GTK_EDITABLE(entry)), fw_browser_path(pane->browser), &error);
    if (!path) { gtk_label_set_text(GTK_LABEL(pane->status), error->message); g_error_free(error); return; }
    pane_navigate(pane, path); g_free(path);
}
static void row_activate(GtkColumnView *view, guint position, gpointer data)
{
    (void)view; Pane *pane = data;
    if (fw_browser_busy(pane->browser) || gtk_sort_list_model_get_pending(pane->sorted)||gtk_filter_list_model_get_pending(pane->filtered)||*fw_browser_error(pane->browser)) return;
    GFileInfo *info = g_list_model_get_item(G_LIST_MODEL(pane->sorted), position);
    if (!info) return;
    char *path = g_build_filename(fw_browser_path(pane->browser), g_file_info_get_name(info), NULL);
    if (fw_entry_directory(info)) {
        pane->history_target = -1; pane_navigate(pane, path);
    } else open_path(pane,path,0);
    g_free(path);
    g_object_unref(info);
}
static void history_step(Pane *pane, gint delta)
{
    gint target = (gint)pane->history_at+delta;
    if (target < 0 || target >= (gint)pane->history->len) return;
    pane->history_target = target; pane_navigate(pane, pane->history->pdata[target]);
}
static void pane_up(Pane *pane)
{
    if (!*fw_browser_path(pane->browser)) return;
    char *parent = g_path_get_dirname(fw_browser_path(pane->browser));
    pane->history_target = -1; pane_navigate(pane, parent); g_free(parent);
}
static void toolbar_clicked(GtkButton *button, gpointer data)
{
    Pane *pane = data; pane->app->active = pane;
    switch (GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "action"))) {
    case 0: history_step(pane, -1); break;
    case 1: history_step(pane, 1); break;
    case 2: pane_up(pane); break;
    case 3: pane->history_target = -1; pane_navigate(pane, g_get_home_dir()); break;
    case 4: fw_browser_refresh(pane->browser); break;
    }
}
static GtkWidget *toolbar_button(Pane *pane, const char *icon, const char *tip, guint action)
{
    GtkWidget *button = gtk_button_new_from_icon_name(icon);
    gtk_widget_set_tooltip_text(button, tip); g_object_set_data(G_OBJECT(button), "action", GUINT_TO_POINTER(action));
    g_signal_connect(button, "clicked", G_CALLBACK(toolbar_clicked), pane); return button;
}
struct Draft {
    App *app; GtkWindow *window; GtkWidget *policy;
    char **sources, *destination; guint count; FwOperation operation;
};
static gboolean job_active(App *app)
{
    if (!app->job) return FALSE;
    FwJobProgress progress; fw_job_progress(app->job, &progress);
    gboolean active = progress.active; fw_job_progress_clear(&progress); return active;
}
static void update_actions(App *app)
{
    if (!app->copy || app->closing) return;
    gboolean ready = app->active && !app->draft && !app->edit && !job_active(app) && !fw_browser_busy(app->active->browser) &&
        !*fw_browser_error(app->active->browser) && !gtk_sort_list_model_get_pending(app->active->sorted) && !gtk_filter_list_model_get_pending(app->active->filtered);
    GtkBitset *selection = app->active ? gtk_selection_model_get_selection(app->active->selection) : NULL;
    guint64 selected = selection ? gtk_bitset_get_size(selection) : 0; if (selection) gtk_bitset_unref(selection);
    ready = ready && selected > 0 && selected <= 4096;
    gtk_button_set_label(GTK_BUTTON(app->copy), app->active == app->panes ? "Copy →" : "← Copy");
    gtk_button_set_label(GTK_BUTTON(app->move), app->active == app->panes ? "Move →" : "← Move");
    Pane *other = app->active == app->panes ? app->panes+1 : app->panes;
    gboolean destination = other->browser && *fw_browser_path(other->browser) && !fw_browser_busy(other->browser) && !*fw_browser_error(other->browser);
    for(guint i=0;i<2;i++) {
        Pane *pane=&app->panes[i];GtkBitset *items=gtk_selection_model_get_selection(pane->selection);guint64 n=gtk_bitset_get_size(items);gtk_bitset_unref(items);
        gboolean can_name=!app->draft&&!app->edit&&!job_active(app)&&!fw_browser_busy(pane->browser)&&!*fw_browser_error(pane->browser)&&!gtk_sort_list_model_get_pending(pane->sorted)&&!gtk_filter_list_model_get_pending(pane->filtered);
        if(pane->rename_button)gtk_widget_set_sensitive(pane->rename_button,(can_name&&n==1)||g_object_get_data(G_OBJECT(pane->rename_button),"editing")!=NULL);
        if(pane->new_button)gtk_widget_set_sensitive(pane->new_button,can_name||g_object_get_data(G_OBJECT(pane->new_button),"editing")!=NULL);
    }
    gtk_widget_set_sensitive(app->copy, ready && destination); gtk_widget_set_sensitive(app->move, ready && destination); gtk_widget_set_sensitive(app->trash, ready);
}
static void selection_changed(GtkSelectionModel *selection, guint position, guint count, gpointer data)
{
    (void)selection; (void)position; (void)count; Pane *pane = data;context_discard(pane);update_actions(pane->app);
}
static void draft_discard(App *app)
{
    Draft *draft = app->draft; if (!draft) return;
    app->draft = NULL;
    g_signal_handlers_disconnect_by_data(draft->window, draft);
    gtk_window_destroy(draft->window); g_strfreev(draft->sources); g_free(draft->destination); g_free(draft);
    update_actions(app);
}
static gboolean draft_close(GtkWindow *window, gpointer data)
{
    (void)window; Draft *draft = data; draft_discard(draft->app); return TRUE;
}
static void draft_cancel(GtkButton *button, gpointer data) { (void)button; Draft *draft = data; draft_discard(draft->app); }
static gboolean close_after_job(gpointer data) { gtk_window_close(GTK_WINDOW(data)); return G_SOURCE_REMOVE; }
static void operation_changed(FwJob *job, gpointer data)
{
    App *app = data; if (app->closing) return;
    FwJobProgress p; fw_job_progress(job, &p);
    const char *text = p.active && p.cancel_requested ? "Cancelling… waiting for the helper's result." : p.message;
    gtk_label_set_text(GTK_LABEL(app->job_status), text); gtk_widget_set_tooltip_text(app->job_status, text);
    gtk_widget_set_visible(app->cancel, p.active); gtk_widget_set_sensitive(app->cancel, p.active && !p.cancel_requested);
    double fraction = p.bytes_total ? (double)p.bytes_done/p.bytes_total : p.items_total ? (double)p.items_done/p.items_total : 0;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->job_progress), CLAMP(fraction, 0, 1));
    gtk_widget_set_visible(app->job_progress, p.active || p.state != FW_JOB_IDLE);
    gtk_label_set_text(GTK_LABEL(app->details), p.details); gtk_widget_set_visible(app->job_details, *p.details != 0);
    if (p.state == FW_JOB_ERRORS || p.state == FW_JOB_UNCONFIRMED) gtk_widget_add_css_class(app->job_status, "error"); else gtk_widget_remove_css_class(app->job_status, "error");
    if (!p.active && !app->job_reported) {
        app->job_reported = TRUE; edit_finished(app,&p);
        for (guint i = 0; i < 2; i++) fw_browser_refresh(app->panes[i].browser);
        if (app->close_wait && !app->close_queued) {
            app->close_queued = TRUE;
            g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, close_after_job, g_object_ref(app->window), g_object_unref);
        }
    }
    fw_job_progress_clear(&p); update_actions(app);
}
static void draft_start(GtkButton *button, gpointer data)
{
    (void)button; Draft *draft = data; App *app = draft->app; GError *error = NULL;
    FwConflict policy = draft->policy ? (FwConflict)gtk_drop_down_get_selected(GTK_DROP_DOWN(draft->policy)) : FW_FAIL_CONFLICT;
    FwJob *job = fw_job_new(app->helper, draft->operation, (const char *const *)draft->sources, draft->count,
        draft->destination, policy, (const char *const *)app->job_env, &error);
    if (!job) { gtk_label_set_text(GTK_LABEL(app->job_status), error->message); g_error_free(error); return; }
    if (app->job) g_signal_handlers_disconnect_by_data(app->job, app);
    g_clear_object(&app->job); app->job = job; app->job_reported = FALSE;
    draft_discard(app); g_signal_connect(job, "changed", G_CALLBACK(operation_changed), app);
    fw_job_start(job); operation_changed(job, app);
}
static void prepare_operation(App *app, FwOperation operation)
{
    update_actions(app);
    GtkWidget *action = operation == FW_COPY ? app->copy : operation == FW_MOVE ? app->move : app->trash;
    if (!gtk_widget_get_sensitive(action)) return;
    Pane *source = app->active, *other = source == app->panes ? app->panes+1 : app->panes;
    GtkBitset *selected = gtk_selection_model_get_selection(source->selection);
    guint64 count = gtk_bitset_get_size(selected);
    if (!count || count > 4096) { gtk_bitset_unref(selected); return; }
    Draft *draft = g_new0(Draft, 1); draft->app = app; draft->count = (guint)count; draft->operation = operation;
    draft->sources = g_new0(char *, count+1); draft->destination = g_strdup(operation == FW_TRASH ? "" : fw_browser_path(other->browser));
    GtkBitsetIter iter; guint position, index = 0;
    for (gboolean more = gtk_bitset_iter_init_first(&iter, selected, &position); more; more = gtk_bitset_iter_next(&iter, &position)) {
        GFileInfo *info = g_list_model_get_item(G_LIST_MODEL(source->sorted), position);
        if (info) { draft->sources[index++] = g_build_filename(fw_browser_path(source->browser), g_file_info_get_name(info), NULL); g_object_unref(info); }
    }
    gtk_bitset_unref(selected); draft->count = index;
    app->draft = draft; draft->window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_transient_for(draft->window, app->window); gtk_window_set_modal(draft->window, TRUE); gtk_window_set_default_size(draft->window, 600, -1);
    const char *verb = operation == FW_COPY ? "Copy" : operation == FW_MOVE ? "Move" : "Move to Trash";
    gtk_window_set_title(draft->window, verb);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16); gtk_widget_set_margin_top(box, 22); gtk_widget_set_margin_bottom(box, 22); gtk_widget_set_margin_start(box, 22); gtk_widget_set_margin_end(box, 22);
    char *heading = g_strdup_printf("%s %u selected item%s?", verb, draft->count, draft->count == 1 ? "" : "s");
    GtkWidget *label = gtk_label_new(heading); gtk_widget_add_css_class(label, "title-3"); gtk_label_set_xalign(GTK_LABEL(label), 0); gtk_box_append(GTK_BOX(box), label); g_free(heading);
    GString *summary = g_string_new(NULL);
    for (guint i = 0; i < MIN(draft->count, 4u); i++) { char *name = fw_display_bytes(draft->sources[i]); g_string_append_printf(summary, "%s\n", name); g_free(name); }
    if (draft->count > 4) g_string_append_printf(summary, "…and %u more\n", draft->count-4);
    if (operation != FW_TRASH) { char *dest = fw_display_bytes(draft->destination); g_string_append_printf(summary, "\nTo: %s", dest); g_free(dest); }
    else g_string_append(summary, "\nRecovery information is kept. There is no permanent-delete fallback.");
    label = gtk_label_new(summary->str); gtk_label_set_xalign(GTK_LABEL(label), 0); gtk_label_set_wrap(GTK_LABEL(label), TRUE); gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR); gtk_label_set_max_width_chars(GTK_LABEL(label), 65); gtk_label_set_selectable(GTK_LABEL(label), TRUE); gtk_box_append(GTK_BOX(box), label); g_string_free(summary, TRUE);
    if (operation != FW_TRASH) {
        const char *policies[] = {"Keep both (automatic names)", "Skip files; merge folders", "Replace files; merge folders", "Report conflicts as errors", NULL};
        GtkWidget *description = gtk_label_new("If an item already exists:"); gtk_label_set_xalign(GTK_LABEL(description), 0); gtk_box_append(GTK_BOX(box), description);
        draft->policy = gtk_drop_down_new_from_strings(policies); gtk_box_append(GTK_BOX(box), draft->policy);
    }
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10); gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel"), *start = gtk_button_new_with_label(verb);
    gtk_widget_add_css_class(start, operation == FW_TRASH ? "destructive-action" : "suggested-action");
    g_signal_connect(cancel, "clicked", G_CALLBACK(draft_cancel), draft); g_signal_connect(start, "clicked", G_CALLBACK(draft_start), draft);
    g_object_set_data(G_OBJECT(draft->window), "start-button", start);
    gtk_box_append(GTK_BOX(buttons), cancel); gtk_box_append(GTK_BOX(buttons), start); gtk_box_append(GTK_BOX(box), buttons);
    gtk_window_set_child(draft->window, box); g_signal_connect(draft->window, "close-request", G_CALLBACK(draft_close), draft);
    gtk_window_present(draft->window); gtk_widget_grab_focus(cancel); update_actions(app);
}
static void operation_clicked(GtkButton *button, gpointer data)
{
    App *app = data; prepare_operation(app, button == GTK_BUTTON(app->copy) ? FW_COPY : button == GTK_BUTTON(app->move) ? FW_MOVE : FW_TRASH);
}
static void cancel_operation(GtkButton *button, gpointer data) { (void)button; App *app = data; if (app->job) fw_job_cancel(app->job); }
static GtkWidget *make_operations(App *app)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8), *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
    gtk_widget_set_margin_start(box, 10); gtk_widget_set_margin_end(box, 10); gtk_widget_set_margin_top(box, 9); gtk_widget_set_margin_bottom(box, 9);
    app->copy = gtk_button_new_with_label("Copy →"); app->move = gtk_button_new_with_label("Move →"); app->trash = gtk_button_new_with_label("Trash"); app->cancel = gtk_button_new_with_label("Cancel operation");
    gtk_widget_set_tooltip_text(app->copy, "Copy selection to the other pane (Shift+F5)"); gtk_widget_set_tooltip_text(app->move, "Move selection to the other pane (Shift+F6)"); gtk_widget_set_tooltip_text(app->trash, "Move selection to Trash (Delete)");
    GtkWidget *buttons[] = {app->copy, app->move, app->trash};
    for (guint i = 0; i < G_N_ELEMENTS(buttons); i++) { gtk_box_append(GTK_BOX(row), buttons[i]); g_signal_connect(buttons[i], "clicked", G_CALLBACK(operation_clicked), app); }
    app->job_status = gtk_label_new("Select files or folders in either pane."); gtk_widget_set_hexpand(app->job_status, TRUE); gtk_label_set_xalign(GTK_LABEL(app->job_status), 0); gtk_label_set_ellipsize(GTK_LABEL(app->job_status), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(row), app->job_status); gtk_box_append(GTK_BOX(row), app->cancel); gtk_widget_set_visible(app->cancel, FALSE); g_signal_connect(app->cancel, "clicked", G_CALLBACK(cancel_operation), app);
    gtk_box_append(GTK_BOX(box), row); app->job_progress = gtk_progress_bar_new(); gtk_widget_set_visible(app->job_progress, FALSE); gtk_box_append(GTK_BOX(box), app->job_progress);
    app->job_details = gtk_expander_new("Operation details"); app->details = gtk_label_new(""); gtk_label_set_xalign(GTK_LABEL(app->details), 0); gtk_label_set_wrap(GTK_LABEL(app->details), TRUE); gtk_label_set_wrap_mode(GTK_LABEL(app->details), PANGO_WRAP_WORD_CHAR); gtk_label_set_selectable(GTK_LABEL(app->details), TRUE);
    GtkWidget *scroll = gtk_scrolled_window_new(); gtk_widget_set_size_request(scroll, -1, 120);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), app->details);
    gtk_expander_set_child(GTK_EXPANDER(app->job_details), scroll); gtk_widget_set_visible(app->job_details, FALSE); gtk_box_append(GTK_BOX(box), app->job_details); update_actions(app); return box;
}

struct NameEdit {
    App *app; Pane *pane; GtkWidget *button,*popover,*entry,*error,*save,*cancel;
    char *root,*source,*target; FwOperation operation; gboolean pending,dismissed;
};
typedef struct EditCleanup { GtkWidget *button,*popover,*view; } EditCleanup;
static gboolean edit_cleanup(gpointer data)
{
    EditCleanup *cleanup=data;
    /* A closed handler runs inside GTK's popover/focus dispatch. Detach only
       after that dispatch unwinds, and never detach a newer editor. */
    if(gtk_menu_button_get_popover(GTK_MENU_BUTTON(cleanup->button))==GTK_POPOVER(cleanup->popover))
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(cleanup->button),NULL);
    GtkRoot *root=cleanup->view?gtk_widget_get_root(cleanup->view):NULL;
    App *app=root?g_object_get_data(G_OBJECT(root),"browser-state"):NULL;
    if(app&&!app->closing&&!app->edit&&!app->draft&&gtk_widget_get_mapped(cleanup->view))
        gtk_widget_grab_focus(cleanup->view);
    g_object_unref(cleanup->button);g_object_unref(cleanup->popover);
    if(cleanup->view)g_object_unref(cleanup->view);
    g_free(cleanup);return G_SOURCE_REMOVE;
}
static void edit_discard(App *app)
{
    NameEdit *edit=app->edit;if(!edit)return;
    app->edit=NULL;g_signal_handlers_disconnect_by_data(edit->popover,edit);
    g_signal_handlers_disconnect_by_data(edit->entry,edit);
    g_signal_handlers_disconnect_by_data(edit->save,edit);
    g_signal_handlers_disconnect_by_data(edit->cancel,edit);
    EditCleanup *cleanup=g_new0(EditCleanup,1);
    cleanup->button=g_object_ref(edit->button);cleanup->popover=g_object_ref(edit->popover);
    if(!app->closing)cleanup->view=g_object_ref(edit->pane->view);
    GtkWidget *focus=gtk_window_get_focus(app->window);
    if(focus&&(focus==edit->entry||gtk_widget_is_ancestor(focus,edit->popover)))gtk_window_set_focus(app->window,NULL);
    gtk_popover_popdown(GTK_POPOVER(edit->popover));
    g_object_set_data(G_OBJECT(edit->button),"editing",NULL);
    g_idle_add(edit_cleanup,cleanup);
    g_free(edit->root);g_free(edit->source);g_free(edit->target);g_free(edit);update_actions(app);
}
static void edit_closed(GtkPopover *popover,gpointer data)
{
    (void)popover;NameEdit *edit=data;
    if(edit->pending){edit->dismissed=TRUE;fw_job_cancel(edit->app->job);}
    else edit_discard(edit->app);
}
static void edit_cancel(GtkButton *button,gpointer data)
{
    (void)button;NameEdit *edit=data;
    if(edit->pending)gtk_popover_popdown(GTK_POPOVER(edit->popover));else edit_discard(edit->app);
}
static void edit_finished(App *app,const FwJobProgress *progress)
{
    NameEdit *edit=app->edit;if(!edit||!edit->pending)return;edit->pending=FALSE;
    if(progress->state==FW_JOB_COMPLETE){
        Pane *pane=edit->pane;g_free(pane->select_after);g_free(pane->select_root);
        pane->select_after=g_path_get_basename(edit->target);pane->select_root=g_strdup(edit->root);
        if(pane->select_after[0]=='.'&&!app->hidden){g_clear_pointer(&pane->select_after,g_free);g_clear_pointer(&pane->select_root,g_free);}
        edit_discard(app);
    }else if(progress->state==FW_JOB_CANCELLED||edit->dismissed||app->close_wait)edit_discard(app);
    else {
        gtk_widget_set_sensitive(edit->entry,TRUE);gtk_widget_set_sensitive(edit->save,TRUE);
        gtk_label_set_text(GTK_LABEL(edit->error),*progress->details?progress->details:progress->message);
        gtk_widget_set_tooltip_text(edit->error,*progress->details?progress->details:progress->message);
        gtk_widget_grab_focus(edit->entry);
    }
}
static void edit_submit(GtkWidget *widget,gpointer data)
{
    (void)widget;NameEdit *edit=data;App *app=edit->app;if(edit->pending)return;GError *error=NULL;
    char *name=fw_parse_name(gtk_editable_get_text(GTK_EDITABLE(edit->entry)),&error);
    if(!name){gtk_label_set_text(GTK_LABEL(edit->error),error->message);g_error_free(error);return;}
    char *target=g_build_filename(edit->root,name,NULL);g_free(name);
    const char *sources[]={edit->operation==FW_RENAME?edit->source:target};
    FwJob *job=fw_job_new(app->helper,edit->operation,sources,1,edit->operation==FW_RENAME?target:NULL,FW_FAIL_CONFLICT,(const char *const *)app->job_env,&error);
    if(!job){gtk_label_set_text(GTK_LABEL(edit->error),error->message);g_error_free(error);g_free(target);return;}
    g_free(edit->target);edit->target=target;edit->pending=TRUE;
    gtk_widget_grab_focus(edit->cancel);
    gtk_widget_set_sensitive(edit->entry,FALSE);gtk_widget_set_sensitive(edit->save,FALSE);gtk_label_set_text(GTK_LABEL(edit->error),"");
    if(app->job)g_signal_handlers_disconnect_by_data(app->job,app);
    g_clear_object(&app->job);app->job=job;app->job_reported=FALSE;
    g_signal_connect(job,"changed",G_CALLBACK(operation_changed),app);fw_job_start(job);operation_changed(job,app);
}
static GtkWidget *find_name_cell(GtkWidget *widget,const char *name)
{
    const char *bound=g_object_get_data(G_OBJECT(widget),"fw-name");
    if(bound&&!strcmp(bound,name)&&gtk_widget_get_mapped(widget))return widget;
    for(GtkWidget *child=gtk_widget_get_first_child(widget);child;child=gtk_widget_get_next_sibling(child)){
        GtkWidget *found=find_name_cell(child,name);if(found)return found;
    }
    return NULL;
}
static char *new_folder_name(Pane *pane)
{
    GHashTable *names=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,NULL);GListModel *model=fw_browser_entries(pane->browser);
    for(guint i=0;i<g_list_model_get_n_items(model);i++){
        GFileInfo *info=g_list_model_get_item(model,i);g_hash_table_add(names,g_strdup(g_file_info_get_name(info)));g_object_unref(info);
    }
    char *name=g_strdup("New folder");
    for(guint i=2;g_hash_table_contains(names,name)&&i<200002;i++){g_free(name);name=g_strdup_printf("New folder (%u)",i);}
    g_hash_table_unref(names);return name;
}
static void begin_name_edit(Pane *pane,FwOperation operation,gboolean popup)
{
    App *app=pane->app;if(app->closing||app->draft||app->edit||job_active(app)||fw_browser_busy(pane->browser)||!*fw_browser_path(pane->browser)||*fw_browser_error(pane->browser)||gtk_sort_list_model_get_pending(pane->sorted)||gtk_filter_list_model_get_pending(pane->filtered))return;
    char *name=NULL;GtkWidget *cell=NULL;
    if(operation==FW_RENAME){
        GtkBitset *selected=gtk_selection_model_get_selection(pane->selection);
        if(gtk_bitset_get_size(selected)!=1){gtk_bitset_unref(selected);return;}
        guint position=gtk_bitset_get_minimum(selected);gtk_bitset_unref(selected);
        GFileInfo *info=g_list_model_get_item(G_LIST_MODEL(pane->sorted),position);if(!info)return;
        name=g_strdup(g_file_info_get_name(info));g_object_unref(info);cell=find_name_cell(pane->view,name);
    }else name=new_folder_name(pane);
    NameEdit *edit=g_new0(NameEdit,1);edit->app=app;edit->pane=pane;edit->operation=operation;edit->root=g_strdup(fw_browser_path(pane->browser));edit->source=g_build_filename(edit->root,name,NULL);
    edit->button=operation==FW_RENAME?pane->rename_button:pane->new_button;app->edit=edit;app->active=pane;
    edit->popover=gtk_popover_new();gtk_popover_set_has_arrow(GTK_POPOVER(edit->popover),FALSE);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(edit->button),edit->popover);g_object_set_data(G_OBJECT(edit->button),"editing",edit);
    if(cell){graphene_rect_t bounds;if(gtk_widget_compute_bounds(cell,edit->button,&bounds)){
        GdkRectangle anchor={(int)bounds.origin.x,(int)bounds.origin.y,(int)bounds.size.width,(int)bounds.size.height};gtk_popover_set_pointing_to(GTK_POPOVER(edit->popover),&anchor);
    }}
    GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,9);gtk_widget_set_margin_top(box,10);gtk_widget_set_margin_bottom(box,10);gtk_widget_set_margin_start(box,10);gtk_widget_set_margin_end(box,10);
    GtkWidget *title=gtk_label_new(operation==FW_RENAME?"Rename":"New Folder");gtk_label_set_xalign(GTK_LABEL(title),0);gtk_box_append(GTK_BOX(box),title);
    edit->entry=gtk_entry_new();gtk_widget_set_size_request(edit->entry,300,-1);char *display=fw_display_bytes(name);gtk_editable_set_text(GTK_EDITABLE(edit->entry),display);g_free(display);g_free(name);gtk_box_append(GTK_BOX(box),edit->entry);
    edit->error=gtk_label_new("");gtk_widget_add_css_class(edit->error,"error");gtk_label_set_wrap(GTK_LABEL(edit->error),TRUE);gtk_label_set_max_width_chars(GTK_LABEL(edit->error),42);gtk_label_set_lines(GTK_LABEL(edit->error),3);gtk_label_set_ellipsize(GTK_LABEL(edit->error),PANGO_ELLIPSIZE_END);gtk_label_set_xalign(GTK_LABEL(edit->error),0);gtk_box_append(GTK_BOX(box),edit->error);
    GtkWidget *buttons=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8),*cancel=gtk_button_new_with_label("Cancel");edit->cancel=cancel;gtk_widget_set_halign(buttons,GTK_ALIGN_END);
    edit->save=gtk_button_new_with_label(operation==FW_RENAME?"Rename":"Create");gtk_widget_add_css_class(edit->save,"suggested-action");gtk_box_append(GTK_BOX(buttons),cancel);gtk_box_append(GTK_BOX(buttons),edit->save);gtk_box_append(GTK_BOX(box),buttons);
    g_signal_connect(cancel,"clicked",G_CALLBACK(edit_cancel),edit);g_signal_connect(edit->save,"clicked",G_CALLBACK(edit_submit),edit);g_signal_connect(edit->entry,"activate",G_CALLBACK(edit_submit),edit);g_signal_connect(edit->popover,"closed",G_CALLBACK(edit_closed),edit);
    gtk_popover_set_child(GTK_POPOVER(edit->popover),box);if(popup)gtk_menu_button_popup(GTK_MENU_BUTTON(edit->button));
    gtk_widget_grab_focus(edit->entry);gtk_editable_select_region(GTK_EDITABLE(edit->entry),0,-1);update_actions(app);
}
static void name_menu_create(GtkMenuButton *button,gpointer data)
{
    Pane *pane=data;begin_name_edit(pane,GTK_WIDGET(button)==pane->rename_button?FW_RENAME:FW_MKDIR,FALSE);
}

/* A launch owns a window reference until the asynchronous result arrives.
   Closing cancels discovery/launch and callbacks never touch destroyed widgets. */
typedef struct Launch { GtkWindow *window; guint pane,purpose; gboolean explicit_app; FwDesktopTarget *target; } Launch;
static App *launch_app(Launch *launch) { return g_object_get_data(G_OBJECT(launch->window),"browser-state"); }
static void launch_free(gpointer data)
{
    Launch *launch=data;fw_desktop_target_free(launch->target);g_object_unref(launch->window);g_free(launch);
}
static void launch_result(Launch *launch,GError *error)
{
    App *app=launch_app(launch);
    if(!app->closing){
        app->opening=FALSE;
        const char *message=error?error->message:"Open request sent to the application.";
        /* Do not relabel a different folder after asynchronous navigation. */
        gtk_label_set_text(GTK_LABEL(app->desktop_status),message);gtk_widget_set_tooltip_text(app->desktop_status,message);
        if(error)gtk_widget_add_css_class(app->desktop_status,"error");else gtk_widget_remove_css_class(app->desktop_status,"error");
    }
    g_clear_error(&error);launch_free(launch);
}
static void launch_done(GObject *source,GAsyncResult *result,gpointer data)
{
    GError *error=NULL;
    Launch *launch=data;
    if(launch->explicit_app)g_app_info_launch_uris_finish(G_APP_INFO(source),result,&error);
    else g_app_info_launch_default_for_uri_finish(result,&error);
    launch_result(data,error);
}
static void launch_target(Launch *launch,GAppInfo *chosen)
{
    App *app=launch_app(launch);launch->explicit_app=chosen!=NULL;
    GdkAppLaunchContext *context=gdk_display_get_app_launch_context(gtk_widget_get_display(GTK_WIDGET(app->window)));
    if(chosen){GList uri={.data=launch->target->uri};g_app_info_launch_uris_async(chosen,launch->purpose==2?NULL:&uri,G_APP_LAUNCH_CONTEXT(context),app->launch_cancel,launch_done,launch);}
    else g_app_info_launch_default_for_uri_async(launch->target->uri,G_APP_LAUNCH_CONTEXT(context),app->launch_cancel,launch_done,launch);
    g_object_unref(context);
}
static void chooser_destroy(GtkWidget *widget,gpointer data)
{
    Launch *launch=data;App *app=launch_app(launch);
    if(app->chooser==GTK_WINDOW(widget))app->chooser=NULL;
    app->opening=FALSE;
    if(!app->closing)gtk_label_set_text(GTK_LABEL(app->desktop_status),"Open cancelled.");
}
static gboolean chooser_filter(GtkListBoxRow *row,gpointer data)
{
    GtkEditable *entry=data;const char *text=gtk_editable_get_text(entry);
    GAppInfo *info=g_object_get_data(G_OBJECT(row),"application");
    char *name=g_utf8_casefold(g_app_info_get_display_name(info),-1),*needle=g_utf8_casefold(text,-1);
    gboolean visible=strstr(name,needle)!=NULL;g_free(name);g_free(needle);return visible;
}
static void chooser_search(GtkSearchEntry *entry,gpointer data) { (void)entry;gtk_list_box_invalidate_filter(GTK_LIST_BOX(data)); }
static void chooser_cancel(GtkButton *button,gpointer data) { (void)button;gtk_window_destroy(GTK_WINDOW(data)); }
static gboolean chooser_key(GtkEventControllerKey *controller,guint key,guint code,GdkModifierType mods,gpointer data)
{ (void)controller;(void)code;(void)mods;if(key!=GDK_KEY_Escape)return FALSE;gtk_window_destroy(GTK_WINDOW(data));return TRUE; }
static void chooser_open(GtkWidget *widget,gpointer data)
{
    (void)widget;GtkWindow *window=data;Launch *owned=g_object_get_data(G_OBJECT(window),"launch");
    GtkListBox *list=g_object_get_data(G_OBJECT(window),"applications");GtkListBoxRow *row=gtk_list_box_get_selected_row(list);if(!row||!gtk_widget_get_child_visible(GTK_WIDGET(row)))return;
    GAppInfo *info=g_object_ref(g_object_get_data(G_OBJECT(row),"application"));
    /* Transfer the launch ownership before destroying the chooser. */
    Launch *launch=g_object_steal_data(G_OBJECT(window),"launch");App *app=launch_app(owned);
    g_signal_handlers_disconnect_by_data(window,launch);app->chooser=NULL;
    gtk_window_destroy(window);app->opening=TRUE;launch_target(launch,info);g_object_unref(info);
}
static void chooser_activate(GtkListBox *box,GtkListBoxRow *row,gpointer data) { (void)box;(void)row;chooser_open(NULL,data); }
static void show_chooser(Launch *launch)
{
    App *app=launch_app(launch);GtkWindow *window=GTK_WINDOW(gtk_window_new());app->chooser=window;
    gtk_window_set_transient_for(window,app->window);gtk_window_set_modal(window,TRUE);gtk_window_set_title(window,"Open With");gtk_window_set_default_size(window,460,460);
    g_object_set_data_full(G_OBJECT(window),"launch",launch,launch_free);g_signal_connect(window,"destroy",G_CALLBACK(chooser_destroy),launch);
    GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,10);gtk_widget_set_margin_top(box,16);gtk_widget_set_margin_bottom(box,16);gtk_widget_set_margin_start(box,16);gtk_widget_set_margin_end(box,16);
    char *name=g_path_get_basename(launch->target->path),*display=fw_display_bytes(name);g_free(name);
    GtkWidget *label=gtk_label_new(display);g_free(display);gtk_label_set_ellipsize(GTK_LABEL(label),PANGO_ELLIPSIZE_MIDDLE);gtk_box_append(GTK_BOX(box),label);
    GtkWidget *search=gtk_search_entry_new();gtk_box_append(GTK_BOX(box),search);
    GtkWidget *list=gtk_list_box_new();gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(list),FALSE);
    g_object_set_data(G_OBJECT(window),"applications",list);
    for(GList *at=launch->target->applications;at;at=at->next){
        GAppInfo *info=at->data;if(!g_app_info_should_show(info)||(!g_app_info_supports_files(info)&&!g_app_info_supports_uris(info)))continue;
        GtkWidget *row=gtk_list_box_row_new(),*item=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,10);
        GIcon *icon=g_app_info_get_icon(info);GtkWidget *image=icon?gtk_image_new_from_gicon(icon):gtk_image_new_from_icon_name("application-x-executable-symbolic");
        gtk_box_append(GTK_BOX(item),image);label=gtk_label_new(g_app_info_get_display_name(info));gtk_label_set_xalign(GTK_LABEL(label),0);gtk_box_append(GTK_BOX(item),label);
        gtk_widget_set_margin_top(item,7);gtk_widget_set_margin_bottom(item,7);gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row),item);
        g_object_set_data_full(G_OBJECT(row),"application",g_object_ref(info),g_object_unref);gtk_list_box_append(GTK_LIST_BOX(list),row);
    }
    gtk_list_box_set_filter_func(GTK_LIST_BOX(list),chooser_filter,search,NULL);g_signal_connect(search,"search-changed",G_CALLBACK(chooser_search),list);
    GtkWidget *empty=gtk_label_new("No applications found. Install a Linux application for this file type.");gtk_label_set_wrap(GTK_LABEL(empty),TRUE);gtk_list_box_set_placeholder(GTK_LIST_BOX(list),empty);
    GtkWidget *scroll=gtk_scrolled_window_new();gtk_widget_set_vexpand(scroll,TRUE);gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),list);gtk_box_append(GTK_BOX(box),scroll);
    GtkWidget *buttons=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8),*cancel=gtk_button_new_with_label("Cancel"),*open=gtk_button_new_with_label("Open");gtk_widget_set_halign(buttons,GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(buttons),cancel);gtk_box_append(GTK_BOX(buttons),open);gtk_box_append(GTK_BOX(box),buttons);gtk_widget_add_css_class(open,"suggested-action");
    g_signal_connect(cancel,"clicked",G_CALLBACK(chooser_cancel),window);g_signal_connect(open,"clicked",G_CALLBACK(chooser_open),window);g_signal_connect(list,"row-activated",G_CALLBACK(chooser_activate),window);
    gtk_window_set_default_widget(window,open);gtk_window_set_child(window,box);
    GtkEventController *keys=gtk_event_controller_key_new();gtk_event_controller_set_propagation_phase(keys,GTK_PHASE_CAPTURE);g_signal_connect(keys,"key-pressed",G_CALLBACK(chooser_key),window);gtk_widget_add_controller(GTK_WIDGET(window),keys);
    g_signal_connect(search,"activate",G_CALLBACK(chooser_open),window);
    GtkListBoxRow *first=gtk_list_box_get_row_at_index(GTK_LIST_BOX(list),0);if(first)gtk_list_box_select_row(GTK_LIST_BOX(list),first);
    gtk_window_present(window);gtk_widget_grab_focus(search);
}
static void open_queried(GObject *source,GAsyncResult *result,gpointer data)
{
    (void)source;Launch *launch=data;App *app=launch_app(launch);GError *error=NULL;
    launch->target=fw_desktop_query_finish(result,&error);
    if(app->closing||error){launch_result(launch,error);return;}
    if(launch->purpose==1){show_chooser(launch);return;}
    if(launch->purpose==2){
        GAppInfo *terminal=fw_desktop_terminal_app(app->helper,launch->target->path,&error);
        if(!terminal){launch_result(launch,error);return;}launch_target(launch,terminal);g_object_unref(terminal);
    }else launch_target(launch,NULL);
}
static void open_path(Pane *pane,const char *path,guint purpose)
{
    App *app=pane->app;if(app->closing||app->opening||app->edit||app->draft)return;
    Launch *launch=g_new0(Launch,1);launch->window=g_object_ref(app->window);launch->pane=(guint)(pane-app->panes);launch->purpose=purpose;
    app->opening=TRUE;gtk_widget_set_visible(app->desktop_status,TRUE);gtk_widget_remove_css_class(app->desktop_status,"error");
    gtk_label_set_text(GTK_LABEL(app->desktop_status),purpose==1?"Finding applications…":"Opening…");
    fw_desktop_query_async(path,purpose==2,purpose==1,app->launch_cancel,open_queried,launch);
}

enum { MENU_OPEN,MENU_OPEN_WITH,MENU_COPY,MENU_MOVE,MENU_RENAME,MENU_TRASH,MENU_PATH,MENU_NEW,MENU_TERMINAL,MENU_REFRESH,MENU_COUNT };
struct ContextMenu {
    Pane *pane;GtkWidget *popover,*buttons[MENU_COUNT];char *root,**names;guint count,revision;gboolean directory;
};
typedef struct ContextCleanup { GtkWidget *popover,*parent; } ContextCleanup;
static gboolean context_unparent(gpointer data)
{
    ContextCleanup *cleanup=data;
    if(gtk_widget_get_parent(cleanup->popover))gtk_widget_unparent(cleanup->popover);
    g_object_unref(cleanup->popover);g_object_unref(cleanup->parent);g_free(cleanup);return G_SOURCE_REMOVE;
}
static void context_discard(Pane *pane)
{
    ContextMenu *menu=pane->context;if(!menu)return;pane->context=NULL;
    g_signal_handlers_disconnect_by_data(menu->popover,menu);
    for(guint i=0;i<MENU_COUNT;i++)if(menu->buttons[i])g_signal_handlers_disconnect_by_data(menu->buttons[i],menu);
    gtk_popover_popdown(GTK_POPOVER(menu->popover));
    ContextCleanup *cleanup=g_new0(ContextCleanup,1);cleanup->popover=g_object_ref(menu->popover);cleanup->parent=g_object_ref(pane->view);
    g_idle_add(context_unparent,cleanup);
    g_free(menu->root);g_strfreev(menu->names);g_free(menu);
}
static void context_closed(GtkPopover *popover,gpointer data) { (void)popover;ContextMenu *menu=data;context_discard(menu->pane); }
static gboolean pane_ready(Pane *pane)
{
    return !pane->app->closing&&!fw_browser_busy(pane->browser)&&!*fw_browser_error(pane->browser)&&*fw_browser_path(pane->browser)&&!gtk_sort_list_model_get_pending(pane->sorted)&&!gtk_filter_list_model_get_pending(pane->filtered);
}
static GdkContentProvider *path_content(ContextMenu *menu)
{
    GString *text=g_string_new(NULL),*uris=g_string_new(NULL);guint count=MAX(menu->count,1u);
    for(guint i=0;i<count;i++){
        char *path=menu->count?g_build_filename(menu->root,menu->names[i],NULL):g_strdup(menu->root);
        char *display=fw_display_bytes(path),*uri=g_filename_to_uri(path,NULL,NULL);
        if(i)g_string_append_c(text,'\n');
        g_string_append(text,display);if(uri){g_string_append_printf(uris,"%s\r\n",uri);g_free(uri);}
        g_free(path);g_free(display);
    }
    GBytes *bytes=g_bytes_new(uris->str,uris->len);
    GdkContentProvider *providers[]={gdk_content_provider_new_typed(G_TYPE_STRING,text->str),gdk_content_provider_new_for_bytes("text/uri-list",bytes)};
    GdkContentProvider *content=gdk_content_provider_new_union(providers,G_N_ELEMENTS(providers));
    g_bytes_unref(bytes);g_string_free(text,TRUE);g_string_free(uris,TRUE);return content;
}
static void context_action(GtkButton *button,gpointer data)
{
    ContextMenu *menu=data;Pane *pane=menu->pane;App *app=pane->app;guint action=GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button),"menu-action"));
    if(!pane_ready(pane)||menu->revision!=fw_browser_revision(pane->browser)||strcmp(menu->root,fw_browser_path(pane->browser))){context_discard(pane);return;}
    if(action==MENU_PATH){GdkContentProvider *content=path_content(menu);gdk_clipboard_set_content(gtk_widget_get_clipboard(pane->view),content);g_object_unref(content);}
    char *path=menu->count?g_build_filename(menu->root,menu->names[0],NULL):g_strdup(menu->root);gboolean directory=menu->directory;
    app->active=pane;context_discard(pane);
    switch(action){
    case MENU_OPEN: if(directory)pane_navigate(pane,path);else open_path(pane,path,0);break;
    case MENU_OPEN_WITH:open_path(pane,path,1);break;
    case MENU_COPY:prepare_operation(app,FW_COPY);break;
    case MENU_MOVE:prepare_operation(app,FW_MOVE);break;
    case MENU_RENAME:begin_name_edit(pane,FW_RENAME,TRUE);break;
    case MENU_TRASH:prepare_operation(app,FW_TRASH);break;
    case MENU_NEW:begin_name_edit(pane,FW_MKDIR,TRUE);break;
    case MENU_TERMINAL:open_path(pane,path,2);break;
    case MENU_REFRESH:fw_browser_refresh(pane->browser);break;
    default:break;
    }
    g_free(path);
}
static void menu_item(ContextMenu *menu,guint action,const char *title,gboolean enabled)
{
    GtkWidget *button=gtk_button_new_with_label(title);menu->buttons[action]=button;
    gtk_widget_add_css_class(button,"flat");gtk_widget_set_halign(button,GTK_ALIGN_FILL);gtk_widget_set_sensitive(button,enabled);
    g_object_set_data(G_OBJECT(button),"menu-action",GUINT_TO_POINTER(action));g_signal_connect(button,"clicked",G_CALLBACK(context_action),menu);
    gtk_box_append(GTK_BOX(gtk_popover_get_child(GTK_POPOVER(menu->popover))),button);
}
static void show_context(Pane *pane,const char *name,double x,double y)
{
    App *app=pane->app;if(!pane_ready(pane)||app->edit||app->draft)return;
    context_discard(pane);context_discard(pane==app->panes?app->panes+1:app->panes);app->active=pane;
    guint found=G_MAXUINT;GFileInfo *clicked=NULL;
    if(name){
        for(guint i=0;i<g_list_model_get_n_items(G_LIST_MODEL(pane->sorted));i++){
            GFileInfo *info=g_list_model_get_item(G_LIST_MODEL(pane->sorted),i);
            if(!strcmp(name,g_file_info_get_name(info))){found=i;clicked=info;break;}g_object_unref(info);
        }
        if(found==G_MAXUINT)return;
        if(!gtk_selection_model_is_selected(pane->selection,found))gtk_selection_model_select_item(pane->selection,found,TRUE);
    }else gtk_selection_model_unselect_all(pane->selection);
    ContextMenu *menu=g_new0(ContextMenu,1);menu->pane=pane;menu->root=g_strdup(fw_browser_path(pane->browser));menu->revision=fw_browser_revision(pane->browser);
    GtkBitset *selected=gtk_selection_model_get_selection(pane->selection);guint64 count=gtk_bitset_get_size(selected);
    if(count>4096){gtk_bitset_unref(selected);g_clear_object(&clicked);g_free(menu->root);g_free(menu);return;}
    menu->count=(guint)count;menu->names=g_new0(char *,count+1);guint pos,index=0;GtkBitsetIter iter;
    for(gboolean more=gtk_bitset_iter_init_first(&iter,selected,&pos);more;more=gtk_bitset_iter_next(&iter,&pos)){
        GFileInfo *info=g_list_model_get_item(G_LIST_MODEL(pane->sorted),pos);menu->names[index++]=g_strdup(g_file_info_get_name(info));g_object_unref(info);
    }
    gtk_bitset_unref(selected);menu->directory=clicked&&fw_entry_directory(clicked);g_clear_object(&clicked);pane->context=menu;update_actions(app);
    menu->popover=gtk_popover_new();gtk_popover_set_has_arrow(GTK_POPOVER(menu->popover),FALSE);gtk_widget_set_parent(menu->popover,pane->view);
    GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);gtk_popover_set_child(GTK_POPOVER(menu->popover),box);
    gboolean idle=!job_active(app),one=count==1;
    if(count){
        menu_item(menu,MENU_OPEN,"Open",one&&!app->opening);
        if(!menu->directory)menu_item(menu,MENU_OPEN_WITH,"Open With…",one&&!app->opening);
        menu_item(menu,MENU_COPY,"Copy to Other Pane…",gtk_widget_get_sensitive(app->copy));menu_item(menu,MENU_MOVE,"Move to Other Pane…",gtk_widget_get_sensitive(app->move));
        menu_item(menu,MENU_RENAME,"Rename…",one&&idle);menu_item(menu,MENU_TRASH,"Move to Trash…",idle);
    }else menu_item(menu,MENU_NEW,"New Folder…",idle);
    menu_item(menu,MENU_PATH,count>1?"Copy Paths":"Copy Path",TRUE);
    if(!count||menu->directory)menu_item(menu,MENU_TERMINAL,"Open Terminal Here",(count<=1)&&!app->opening);
    if(!count)menu_item(menu,MENU_REFRESH,"Refresh",TRUE);
    GdkRectangle anchor={(int)x,(int)y,1,1};gtk_popover_set_pointing_to(GTK_POPOVER(menu->popover),&anchor);
    g_signal_connect(menu->popover,"closed",G_CALLBACK(context_closed),menu);gtk_popover_popup(GTK_POPOVER(menu->popover));update_actions(app);
}
static const char *context_row_at(Pane *pane,double x,double y)
{
    GtkWidget *picked=gtk_widget_pick(pane->view,x,y,GTK_PICK_DEFAULT);
    for(GtkWidget *at=picked;at&&at!=pane->view;at=gtk_widget_get_parent(at)){
        const char *name=g_object_get_data(G_OBJECT(at),"fw-row");if(name)return name;
    }
    return NULL;
}
static void context_pressed(GtkGestureClick *gesture,gint n,double x,double y,gpointer data)
{
    (void)n;Pane *pane=data;const char *name=context_row_at(pane,x,y);
    if(!name){GtkWidget *picked=gtk_widget_pick(pane->view,x,y,GTK_PICK_DEFAULT);
        for(GtkWidget *at=picked;at&&at!=pane->view;at=gtk_widget_get_parent(at))if(!strcmp(gtk_widget_get_css_name(at),"header"))return;
    }
    show_context(pane,name,x,y);if(pane->context)gtk_gesture_set_state(GTK_GESTURE(gesture),GTK_EVENT_SEQUENCE_CLAIMED);
}

static void pane_focus(GtkEventControllerFocus *controller, gpointer data)
{
    (void)controller; Pane *pane = data;
    pane->app->active = pane;
    for (guint i = 0; i < 2; i++) gtk_widget_remove_css_class(pane->app->panes[i].box, "active-pane");
    gtk_widget_add_css_class(pane->box, "active-pane"); update_actions(pane->app);
}
static void make_pane(App *app, guint index, const char *helper)
{
    Pane *pane = &app->panes[index]; pane->app = app; pane->history_target = -1;
    pane->history = g_ptr_array_new_with_free_func(g_free); pane->browser = fw_browser_new(helper);
    pane->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); gtk_widget_set_hexpand(pane->box, TRUE);
    gtk_widget_add_css_class(pane->box, "browser-pane");
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3); gtk_widget_add_css_class(toolbar, "pane-toolbar");
    pane->back = toolbar_button(pane, "go-previous-symbolic", "Back (Alt+Left)", 0);
    pane->forward = toolbar_button(pane, "go-next-symbolic", "Forward (Alt+Right)", 1);
    gtk_box_append(GTK_BOX(toolbar), pane->back); gtk_box_append(GTK_BOX(toolbar), pane->forward);
    gtk_box_append(GTK_BOX(toolbar), toolbar_button(pane, "go-up-symbolic", "Up (Alt+Up)", 2));
    gtk_box_append(GTK_BOX(toolbar), toolbar_button(pane, "go-home-symbolic", "Home", 3));
    pane->address = gtk_entry_new(); gtk_widget_set_hexpand(pane->address, TRUE);
    gtk_widget_set_tooltip_text(pane->address, "Linux path (Ctrl+L). Backslash: \\\\; escaped byte: \\xHH.");
    g_signal_connect(pane->address, "activate", G_CALLBACK(address_activate), pane);
    gtk_box_append(GTK_BOX(toolbar), pane->address);
    gtk_box_append(GTK_BOX(toolbar), toolbar_button(pane, "view-refresh-symbolic", "Refresh (F5)", 4));
    pane->rename_button=gtk_menu_button_new();gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(pane->rename_button),"document-edit-symbolic");gtk_widget_set_tooltip_text(pane->rename_button,"Rename selected item (F2)");
    pane->new_button=gtk_menu_button_new();gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(pane->new_button),"folder-new-symbolic");gtk_widget_set_tooltip_text(pane->new_button,"New Folder (Ctrl+Shift+N)");
    gtk_menu_button_set_create_popup_func(GTK_MENU_BUTTON(pane->rename_button),name_menu_create,pane,NULL);
    gtk_menu_button_set_create_popup_func(GTK_MENU_BUTTON(pane->new_button),name_menu_create,pane,NULL);
    gtk_box_append(GTK_BOX(toolbar),pane->rename_button);gtk_box_append(GTK_BOX(toolbar),pane->new_button);
    gtk_box_append(GTK_BOX(pane->box), toolbar);
    pane->view = gtk_column_view_new(NULL); gtk_column_view_set_enable_rubberband(GTK_COLUMN_VIEW(pane->view), TRUE);
    GtkCustomFilter *filter = gtk_custom_filter_new(filter_entry, pane, NULL);
    pane->filtered = gtk_filter_list_model_new(g_object_ref(fw_browser_entries(pane->browser)), GTK_FILTER(filter));
    gtk_filter_list_model_set_incremental(pane->filtered, TRUE);
    GtkMultiSorter *multi = gtk_multi_sorter_new();
    gtk_multi_sorter_append(multi, GTK_SORTER(gtk_custom_sorter_new(compare_dirs, NULL, NULL)));
    gtk_multi_sorter_append(multi, g_object_ref(gtk_column_view_get_sorter(GTK_COLUMN_VIEW(pane->view))));
    gtk_multi_sorter_append(multi, GTK_SORTER(gtk_custom_sorter_new(compare_name, NULL, NULL)));
    pane->sorted = gtk_sort_list_model_new(g_object_ref(G_LIST_MODEL(pane->filtered)), GTK_SORTER(multi));
    gtk_sort_list_model_set_incremental(pane->sorted, TRUE);
    pane->selection = GTK_SELECTION_MODEL(gtk_multi_selection_new(g_object_ref(G_LIST_MODEL(pane->sorted))));
    gtk_column_view_set_model(GTK_COLUMN_VIEW(pane->view), GTK_SELECTION_MODEL(pane->selection));
    GtkColumnViewColumn *name = make_column(pane, "Name", 0), *size = make_column(pane, "Size", 1), *modified = make_column(pane, "Modified", 2);
    gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(pane->view), name, GTK_SORT_ASCENDING);
    g_object_unref(name); g_object_unref(size); g_object_unref(modified);
    GtkWidget *scroll = gtk_scrolled_window_new(); gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), pane->view); gtk_box_append(GTK_BOX(pane->box), scroll);
    GtkWidget *statusbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); gtk_widget_add_css_class(statusbox, "pane-status");
    pane->spinner = gtk_spinner_new(); pane->status = gtk_label_new("Ready");
    gtk_label_set_xalign(GTK_LABEL(pane->status), 0); gtk_label_set_ellipsize(GTK_LABEL(pane->status), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(pane->status, TRUE); gtk_box_append(GTK_BOX(statusbox), pane->spinner); gtk_box_append(GTK_BOX(statusbox), pane->status);
    gtk_box_append(GTK_BOX(pane->box), statusbox);
    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "enter", G_CALLBACK(pane_focus), pane); gtk_widget_add_controller(pane->box, focus);
    GtkGesture *context=gtk_gesture_click_new();gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(context),GDK_BUTTON_SECONDARY);gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(context),GTK_PHASE_CAPTURE);
    g_signal_connect(context,"pressed",G_CALLBACK(context_pressed),pane);gtk_widget_add_controller(pane->view,GTK_EVENT_CONTROLLER(context));
    g_signal_connect(pane->view, "activate", G_CALLBACK(row_activate), pane);
    g_signal_connect(pane->selection, "selection-changed", G_CALLBACK(selection_changed), pane);
    g_signal_connect(pane->browser, "changed", G_CALLBACK(model_changed), pane);
    g_signal_connect(pane->sorted, "notify::pending", G_CALLBACK(pending_changed), pane);
    g_signal_connect(pane->filtered, "notify::pending", G_CALLBACK(pending_changed), pane);
    pane_update(pane);
}
static void hidden_toggled(GtkToggleButton *button, gpointer data)
{
    App *app = data; app->hidden = gtk_toggle_button_get_active(button);
    for (guint i = 0; i < 2; i++) {
        gtk_filter_changed(gtk_filter_list_model_get_filter(app->panes[i].filtered), GTK_FILTER_CHANGE_DIFFERENT);
        pane_update(&app->panes[i]);
    }
}
static gboolean key_pressed(GtkEventControllerKey *controller, guint key, guint code, GdkModifierType mods, gpointer data)
{
    (void)controller; (void)code; App *app = data; Pane *pane = app->active;
    if (!pane) return FALSE;
    if ((mods & (GDK_CONTROL_MASK|GDK_SHIFT_MASK)) == (GDK_CONTROL_MASK|GDK_SHIFT_MASK) && (key==GDK_KEY_n||key==GDK_KEY_N)) {begin_name_edit(pane,FW_MKDIR,TRUE);return TRUE;}
    if ((mods & GDK_CONTROL_MASK) && (key == GDK_KEY_l || key == GDK_KEY_L)) {
        gtk_widget_grab_focus(pane->address); gtk_editable_select_region(GTK_EDITABLE(pane->address), 0, -1); return TRUE;
    }
    if ((mods & GDK_CONTROL_MASK) && (key == GDK_KEY_q || key == GDK_KEY_Q)) { gtk_window_close(app->window); return TRUE; }
    if ((mods & GDK_CONTROL_MASK) && (key == GDK_KEY_h || key == GDK_KEY_H)) {
        GtkToggleButton *hidden = g_object_get_data(G_OBJECT(app->window), "hidden-button");
        gtk_toggle_button_set_active(hidden, !gtk_toggle_button_get_active(hidden)); return TRUE;
    }
    if (mods & GDK_ALT_MASK) {
        if (key == GDK_KEY_Left) { history_step(pane, -1); return TRUE; }
        if (key == GDK_KEY_Right) { history_step(pane, 1); return TRUE; }
        if (key == GDK_KEY_Up) { pane_up(pane); return TRUE; }
    }
    GtkWidget *focus = gtk_window_get_focus(app->window);
    gboolean in_files = focus && (focus == pane->view || gtk_widget_is_ancestor(focus, pane->view));
    if(in_files&&(key==GDK_KEY_Menu||(key==GDK_KEY_F10&&(mods&GDK_SHIFT_MASK)))){
        GtkBitset *selected=gtk_selection_model_get_selection(pane->selection);guint pos=gtk_bitset_is_empty(selected)?G_MAXUINT:gtk_bitset_get_minimum(selected);gtk_bitset_unref(selected);
        GFileInfo *info=pos==G_MAXUINT?NULL:g_list_model_get_item(G_LIST_MODEL(pane->sorted),pos);
        show_context(pane,info?g_file_info_get_name(info):NULL,20,40);g_clear_object(&info);return TRUE;
    }
    if (in_files && key==GDK_KEY_F2 && !(mods&(GDK_CONTROL_MASK|GDK_SHIFT_MASK|GDK_ALT_MASK))) {begin_name_edit(pane,FW_RENAME,TRUE);return TRUE;}
    if (in_files && (mods & GDK_SHIFT_MASK) && key == GDK_KEY_F5) { prepare_operation(app, FW_COPY); return TRUE; }
    if (in_files && (mods & GDK_SHIFT_MASK) && key == GDK_KEY_F6) { prepare_operation(app, FW_MOVE); return TRUE; }
    if (in_files && key == GDK_KEY_Delete && !(mods & (GDK_SHIFT_MASK|GDK_CONTROL_MASK|GDK_ALT_MASK))) { prepare_operation(app, FW_TRASH); return TRUE; }
    if (key == GDK_KEY_F5) { fw_browser_refresh(pane->browser); return TRUE; }
    if (key == GDK_KEY_F6) { Pane *other = pane == app->panes ? app->panes+1 : app->panes; gtk_widget_grab_focus(other->view); return TRUE; }
    return FALSE;
}
static gboolean window_close(GtkWindow *window, gpointer data)
{
    (void)window; App *app = data;
    if (job_active(app)) {
        app->close_wait = TRUE; fw_job_cancel(app->job); operation_changed(app->job, app); return TRUE;
    }
    app_stop(app);
    return FALSE;
}
static void configure_style(void)
{
    static gboolean configured;
    if (configured) return;
    configured = TRUE;
    GtkCssProvider *css = gtk_css_provider_new();
    const char *rules =
        "headerbar {background:#fff7ef; color:#30213c;} .pane-toolbar {padding:8px; border-bottom:1px solid alpha(currentColor,.15);}" 
        ".pane-status {padding:7px 10px; border-top:1px solid alpha(currentColor,.15); font-size:11px;} .active-pane .pane-toolbar {border-bottom:2px solid #ff6655;} .subtitle {font-size:10px;} .error {color:#ac332b;}";
#if GTK_CHECK_VERSION(4, 12, 0)
    gtk_css_provider_load_from_string(css, rules);
#else
    gtk_css_provider_load_from_data(css, rules, -1);
#endif
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION); g_object_unref(css);
}
static App *create_window(GtkApplication *application, const char *helper, const char *icon, const char *left, const char *right)
{
    configure_style();
    App *app = g_new0(App, 1); app->helper = g_strdup(helper); app->job_env = g_get_environ();app->launch_cancel=g_cancellable_new();
    app->window = GTK_WINDOW(application ? gtk_application_window_new(application) : gtk_window_new());
    g_object_set_data_full(G_OBJECT(app->window), "browser-state", app, app_free);
    gtk_window_set_title(app->window, "Folderwing · Linux prototype"); gtk_window_set_default_size(app->window, 1280, 760);
    GtkWidget *header = gtk_header_bar_new(), *title = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
    GtkWidget *logo = gtk_image_new_from_file(icon); gtk_image_set_pixel_size(GTK_IMAGE(logo), 32);
    GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), *brand = gtk_label_new("Folderwing"), *subtitle = gtk_label_new("Linux browser prototype");
    gtk_widget_add_css_class(brand, "title"); gtk_widget_add_css_class(subtitle, "subtitle");
    gtk_box_append(GTK_BOX(labels), brand); gtk_box_append(GTK_BOX(labels), subtitle);
    gtk_box_append(GTK_BOX(title), logo); gtk_box_append(GTK_BOX(title), labels); gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), title);
    GtkWidget *badge = gtk_label_new("Linux prototype"); gtk_widget_add_css_class(badge, "dim-label");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), badge);
    GtkWidget *hidden = gtk_toggle_button_new_with_label("Hidden files"); gtk_widget_set_tooltip_text(hidden, "Show hidden files (Ctrl+H)");
    g_signal_connect(hidden, "toggled", G_CALLBACK(hidden_toggled), app);
    g_object_set_data(G_OBJECT(app->window), "hidden-button", hidden); gtk_header_bar_pack_end(GTK_HEADER_BAR(header), hidden);
    gtk_window_set_titlebar(app->window, header);
    make_pane(app, 0, helper); make_pane(app, 1, helper); app->active = app->panes;
    GtkWidget *split = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(split), app->panes[0].box); gtk_paned_set_end_child(GTK_PANED(split), app->panes[1].box);
    gtk_paned_set_position(GTK_PANED(split), 640); gtk_paned_set_shrink_start_child(GTK_PANED(split), TRUE); gtk_paned_set_shrink_end_child(GTK_PANED(split), TRUE);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); gtk_widget_set_vexpand(split, TRUE);
    gtk_box_append(GTK_BOX(root), split); gtk_box_append(GTK_BOX(root), make_operations(app));
    app->desktop_status=gtk_label_new("");gtk_label_set_xalign(GTK_LABEL(app->desktop_status),0);gtk_label_set_ellipsize(GTK_LABEL(app->desktop_status),PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(app->desktop_status,10);gtk_widget_set_margin_bottom(app->desktop_status,6);gtk_widget_set_visible(app->desktop_status,FALSE);gtk_box_append(GTK_BOX(root),app->desktop_status);
    gtk_window_set_child(app->window, root);
    GtkEventController *keys = gtk_event_controller_key_new(); gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), app); gtk_widget_add_controller(GTK_WIDGET(app->window), keys);
    g_signal_connect(app->window, "close-request", G_CALLBACK(window_close), app);
    pane_navigate(app->panes, left); pane_navigate(app->panes+1, right); return app;
}
#ifndef FW_BROWSER_TEST
static char *option_left, *option_right, *option_helper, *program_dir;
static void activate(GtkApplication *application, gpointer unused)
{
    (void)unused;
    char *helper = option_helper ? g_strdup(option_helper) : g_build_filename(program_dir, "folderwing-linux-helper", NULL);
    char *icon = g_build_filename(program_dir, "folderwing.svg", NULL);
    App *app = create_window(application, helper, icon, option_left ? option_left : g_get_home_dir(), option_right ? option_right : "/");
    gtk_window_present(app->window); gtk_widget_grab_focus(app->panes[0].view); g_free(helper); g_free(icon);
}
int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    GOptionEntry options[] = {
        {"left", 'l', 0, G_OPTION_ARG_FILENAME, &option_left, "Initial left folder (absolute native path)", "PATH"},
        {"right", 'r', 0, G_OPTION_ARG_FILENAME, &option_right, "Initial right folder (absolute native path)", "PATH"},
        {"helper", 0, 0, G_OPTION_ARG_FILENAME, &option_helper, "Explicit listing/watching helper executable", "PATH"},
        {NULL, 0, 0, 0, NULL, NULL, NULL}
    };
    GOptionContext *context = g_option_context_new("— Folderwing native Linux browsing prototype");
    g_option_context_add_main_entries(context, options, NULL); GError *error = NULL;
    if (!g_option_context_parse(context, &argc, &argv, &error)) { g_printerr("%s\n", error->message); g_error_free(error); g_option_context_free(context); return 2; }
    g_option_context_free(context);
    if (argc != 1) { g_printerr("Use --left PATH and --right PATH.\n"); return 2; }
    char *binary = g_file_read_link("/proc/self/exe", NULL); program_dir = binary ? g_path_get_dirname(binary) : g_get_current_dir(); g_free(binary);
    GtkApplication *application = gtk_application_new("one.l1nux.Folderwing.Prototype", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(application, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application); g_free(option_left); g_free(option_right); g_free(option_helper); g_free(program_dir); return status;
}
#endif
