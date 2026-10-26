#include <gtk/gtk.h>
#include <sys/stat.h>
#include <string.h>
#include <signal.h>
#include "model.h"
#include "operations.h"

typedef struct App App;
typedef struct Draft Draft;
typedef struct Pane {
    App *app;
    FwBrowser *browser;
    GtkWidget *box, *address, *view, *status, *spinner, *back, *forward;
    GtkFilterListModel *filtered;
    GtkSortListModel *sorted;
    GtkSelectionModel *selection;
    GPtrArray *history;
    guint history_at, revision;
    gint history_target;
    gboolean busy, requested, restore_selection;
    GHashTable *selected_names;
} Pane;
struct App {
    GtkWindow *window; Pane panes[2]; Pane *active; gboolean hidden, closing, close_wait, close_queued, job_reported;
    char *helper, **job_env; FwJob *job; Draft *draft;
    GtkWidget *copy, *move, *trash, *cancel, *job_status, *job_progress, *job_details, *details;
};
static void update_actions(App *app);
static void draft_discard(App *app);
static void pane_update(Pane *pane);
static void pane_navigate(Pane *pane, const char *path);
static void app_stop(App *app)
{
    app->closing = TRUE;
    draft_discard(app);
    if (app->job) { g_signal_handlers_disconnect_by_data(app->job, app); fw_job_cancel(app->job); }
    for (guint i = 0; i < 2; i++) {
        Pane *pane = &app->panes[i];
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
        g_clear_pointer(&pane->selected_names, g_hash_table_unref);
    }
    g_clear_object(&app->job); g_free(app->helper); g_strfreev(app->job_env); g_free(app);
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
    guint column = GPOINTER_TO_UINT(field);
    if (!column) {
        GtkWidget *icon = gtk_widget_get_first_child(child); label = gtk_widget_get_next_sibling(icon);
        const char *name = fw_entry_directory(info) ? "folder-symbolic" : "text-x-generic-symbolic";
        if (g_file_info_get_attribute_uint32(info, "fw::flags") & 2) name = "dialog-warning-symbolic";
        gtk_image_set_from_icon_name(GTK_IMAGE(icon), name);
        gtk_label_set_text(GTK_LABEL(label), g_file_info_get_display_name(info));
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
static GtkColumnViewColumn *make_column(Pane *pane, const char *name, guint field)
{
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(setup_cell), GUINT_TO_POINTER(field));
    g_signal_connect(factory, "bind", G_CALLBACK(bind_cell), GUINT_TO_POINTER(field));
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
    if (!*fw_browser_path(pane->browser)) {
        char *display = fw_display_bytes(path);
        gtk_editable_set_text(GTK_EDITABLE(pane->address), display); g_free(display);
    }
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
    if (fw_browser_busy(pane->browser) || gtk_sort_list_model_get_pending(pane->sorted)) return;
    GFileInfo *info = g_list_model_get_item(G_LIST_MODEL(pane->sorted), position);
    if (!info) return;
    if (fw_entry_directory(info)) {
        char *path = g_build_filename(fw_browser_path(pane->browser), g_file_info_get_name(info), NULL);
        pane->history_target = -1; pane_navigate(pane, path); g_free(path);
    } else gtk_label_set_text(GTK_LABEL(pane->status), "File selected · previews are outside this browsing prototype.");
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
    gboolean ready = app->active && !app->draft && !job_active(app) && !fw_browser_busy(app->active->browser) &&
        !*fw_browser_error(app->active->browser) && !gtk_sort_list_model_get_pending(app->active->sorted) && !gtk_filter_list_model_get_pending(app->active->filtered);
    GtkBitset *selection = app->active ? gtk_selection_model_get_selection(app->active->selection) : NULL;
    guint64 selected = selection ? gtk_bitset_get_size(selection) : 0; if (selection) gtk_bitset_unref(selection);
    ready = ready && selected > 0 && selected <= 4096;
    gtk_button_set_label(GTK_BUTTON(app->copy), app->active == app->panes ? "Copy →" : "← Copy");
    gtk_button_set_label(GTK_BUTTON(app->move), app->active == app->panes ? "Move →" : "← Move");
    Pane *other = app->active == app->panes ? app->panes+1 : app->panes;
    gboolean destination = other->browser && *fw_browser_path(other->browser) && !fw_browser_busy(other->browser) && !*fw_browser_error(other->browser);
    gtk_widget_set_sensitive(app->copy, ready && destination); gtk_widget_set_sensitive(app->move, ready && destination); gtk_widget_set_sensitive(app->trash, ready);
}
static void selection_changed(GtkSelectionModel *selection, guint position, guint count, gpointer data)
{
    (void)selection; (void)position; (void)count; Pane *pane = data; update_actions(pane->app);
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
        app->job_reported = TRUE;
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
    App *app = g_new0(App, 1); app->helper = g_strdup(helper); app->job_env = g_get_environ();
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
