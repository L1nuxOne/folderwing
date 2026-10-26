#define _GNU_SOURCE
#include "model.h"
#include "../src/wslproto.h"
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#if G_BYTE_ORDER != G_LITTLE_ENDIAN
#error The existing helper wire ABI currently requires a little-endian host.
#endif
G_STATIC_ASSERT(sizeof(wslp_entry) == 48);
#define ENTRY_LIMIT 200000u
#define PATH_LIMIT 16384u

typedef struct Io {
    gint refs;
    GWeakRef owner;
    GSubprocess *process;
    GCancellable *cancel;
    wslp_head head;
    guint8 *payload;
    gboolean hello, writing;
    char *target;
} Io;
struct _FwBrowser {
    GObject parent;
    char *helper, *path, *error, *attempted;
    GListStore *entries;
    GPtrArray *pending;
    Io *io;
    gboolean busy, watching, dirty, closed;
    guint refresh, deadline, revision;
    gint64 started;
    double elapsed;
};
G_DEFINE_TYPE(FwBrowser, fw_browser, G_TYPE_OBJECT)
static guint changed;
static void read_head(Io *io);
static void request_list(FwBrowser *self, gboolean initial);
static void schedule_refresh(FwBrowser *self);
static Io *io_ref(Io *io) { ++io->refs; return io; }
static void io_unref(Io *io)
{
    if (--io->refs) return;
    g_weak_ref_clear(&io->owner);
    g_clear_object(&io->process); g_clear_object(&io->cancel);
    g_free(io->payload); g_free(io->target); g_free(io);
}
static FwBrowser *io_owner(Io *io)
{
    FwBrowser *self = g_weak_ref_get(&io->owner);
    if (self && self->io != io) g_clear_object(&self);
    return self;
}
static void announce(FwBrowser *self) { g_signal_emit(self, changed, 0); }
static void disconnect_io(FwBrowser *self)
{
    if (self->deadline) { g_source_remove(self->deadline); self->deadline = 0; }
    if (self->refresh) { g_source_remove(self->refresh); self->refresh = 0; }
    if (self->io) {
        Io *io = self->io; self->io = NULL;
        g_cancellable_cancel(io->cancel);
        g_subprocess_force_exit(io->process); /* Only this browser's owned child. */
        io_unref(io);
    }
    self->watching = FALSE; self->busy = FALSE; self->dirty = FALSE;
    g_ptr_array_set_size(self->pending, 0);
}
static void fail(FwBrowser *self, const char *message)
{
    g_free(self->error); self->error = g_strdup(message);
    disconnect_io(self); announce(self);
}
static gboolean timeout_request(gpointer data)
{
    FwBrowser *self = data; self->deadline = 0;
    fail(self, "The directory helper did not finish within 15 seconds. Refresh to retry.");
    return G_SOURCE_REMOVE;
}
void fw_browser_close(FwBrowser *self) { self->closed = TRUE; disconnect_io(self); }
static void fw_browser_dispose(GObject *object)
{
    FwBrowser *self = FW_BROWSER(object);
    fw_browser_close(self); g_clear_object(&self->entries);
    G_OBJECT_CLASS(fw_browser_parent_class)->dispose(object);
}
static void fw_browser_finalize(GObject *object)
{
    FwBrowser *self = FW_BROWSER(object);
    g_free(self->helper); g_free(self->path); g_free(self->error); g_free(self->attempted);
    g_ptr_array_unref(self->pending);
    G_OBJECT_CLASS(fw_browser_parent_class)->finalize(object);
}
static void fw_browser_class_init(FwBrowserClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = fw_browser_dispose;
    G_OBJECT_CLASS(klass)->finalize = fw_browser_finalize;
    changed = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}
static void fw_browser_init(FwBrowser *self)
{
    self->entries = g_list_store_new(G_TYPE_FILE_INFO);
    self->pending = g_ptr_array_new_with_free_func(g_object_unref);
    self->path = g_strdup(""); self->error = g_strdup("");
}
FwBrowser *fw_browser_new(const char *helper)
{
    FwBrowser *self = g_object_new(FW_TYPE_BROWSER, NULL);
    self->helper = g_strdup(helper); return self;
}
GListModel *fw_browser_entries(FwBrowser *self) { return G_LIST_MODEL(self->entries); }
const char *fw_browser_path(FwBrowser *self) { return self->path; }
const char *fw_browser_error(FwBrowser *self) { return self->error; }
gboolean fw_browser_busy(FwBrowser *self) { return self->busy; }
gboolean fw_browser_watching(FwBrowser *self) { return self->watching; }
guint fw_browser_revision(FwBrowser *self) { return self->revision; }
double fw_browser_elapsed_ms(FwBrowser *self) { return self->elapsed; }
gboolean fw_entry_directory(GFileInfo *info)
{
    return g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY ||
        (g_file_info_get_attribute_uint32(info, "fw::flags") & WSLP_E_DIRLINK);
}
char *fw_display_bytes(const char *bytes)
{
    GString *text = g_string_new(NULL);
    for (const char *p = bytes; *p;) {
        gunichar c = g_utf8_get_char_validated(p, -1);
        if (c == (gunichar)-1 || c == (gunichar)-2 || c < 32 || c == 127) {
            g_string_append_printf(text, "\\x%02X", (guint8)*p++);
        } else if (*p == '\\') { g_string_append(text, "\\\\"); p++; }
        else { const char *end = g_utf8_next_char(p); g_string_append_len(text, p, end-p); p = end; }
    }
    return g_string_free(text, FALSE);
}
char *fw_parse_path(const char *text, const char *base, GError **error)
{
    GString *raw = g_string_new(NULL);
    for (const char *p = text; *p; p++) {
        if (*p != '\\') { g_string_append_c(raw, *p); continue; }
        if (p[1] == '\\') { g_string_append_c(raw, '\\'); p++; }
        else if (p[1] == 'x' && p[2] && p[3] && g_ascii_isxdigit(p[2]) && g_ascii_isxdigit(p[3])) {
            int byte = g_ascii_xdigit_value(p[2])*16 + g_ascii_xdigit_value(p[3]);
            if (!byte) goto invalid;
            g_string_append_c(raw, byte); p += 3;
        } else goto invalid;
    }
    if (!raw->len) goto invalid;
    char *path;
    if (raw->str[0] == '~' && (!raw->str[1] || raw->str[1] == '/'))
        path = g_strconcat(g_get_home_dir(), raw->str + 1, NULL);
    else if (g_path_is_absolute(raw->str)) path = g_strdup(raw->str);
    else path = g_build_filename(*base ? base : g_get_home_dir(), raw->str, NULL);
    g_string_free(raw, TRUE);
    if (strlen(path) <= PATH_LIMIT) return path;
    g_free(path);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FILENAME_TOO_LONG, "Path exceeds the prototype's 16 KiB limit.");
    return NULL;
invalid:
    g_string_free(raw, TRUE);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Enter a path; escape a literal backslash as \\\\ or a byte as \\xHH. NUL bytes are not allowed.");
    return NULL;
}
char *fw_parse_name(const char *text, GError **error)
{
    GString *raw = g_string_new(NULL);
    for (const char *p = text; *p; p++) {
        if (*p != '\\') g_string_append_c(raw, *p);
        else if (p[1] == '\\') { g_string_append_c(raw, '\\'); p++; }
        else if (p[1] == 'x' && p[2] && p[3] && g_ascii_isxdigit(p[2]) && g_ascii_isxdigit(p[3])) {
            int byte = g_ascii_xdigit_value(p[2])*16+g_ascii_xdigit_value(p[3]);
            if (!byte) goto invalid;
            g_string_append_c(raw, byte); p += 3;
        } else goto invalid;
    }
    if (!raw->len || raw->len > 255 || strchr(raw->str, '/') || !strcmp(raw->str, ".") || !strcmp(raw->str, "..")) goto invalid;
    return g_string_free(raw, FALSE);
invalid:
    g_string_free(raw, TRUE);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME,
        "Use one name of 1–255 bytes. Slash and NUL are invalid; '.' and '..' are reserved. Byte escaping follows the path bar.");
    return NULL;
}
static gboolean parse_entries(FwBrowser *self, const guint8 *data, guint32 size)
{
    guint32 off = 0;
    while (off < size) {
        if (size-off < sizeof(wslp_entry) || self->pending->len >= ENTRY_LIMIT) return FALSE;
        wslp_entry e; memcpy(&e, data+off, sizeof e);
        guint32 record = wslp_pad8(sizeof e + e.name_len + e.link_len);
        if (!e.name_len || record > size-off) return FALSE;
        const char *name = (const char *)data+off+sizeof e;
        if (memchr(name, 0, e.name_len) || memchr(name, '/', e.name_len) ||
            (e.name_len == 1 && *name == '.') || (e.name_len == 2 && !memcmp(name, "..", 2)) ||
            memchr(name+e.name_len, 0, e.link_len)) return FALSE;
        char *raw = g_strndup(name, e.name_len), *display = fw_display_bytes(raw);
        GFileInfo *info = g_file_info_new();
        g_file_info_set_name(info, raw); g_file_info_set_display_name(info, display);
        g_file_info_set_file_type(info, S_ISDIR(e.mode) ? G_FILE_TYPE_DIRECTORY : S_ISLNK(e.mode) ? G_FILE_TYPE_SYMBOLIC_LINK : G_FILE_TYPE_REGULAR);
        g_file_info_set_is_symlink(info, S_ISLNK(e.mode));
        g_file_info_set_size(info, (goffset)MIN(e.size, G_MAXINT64));
        g_file_info_set_attribute_uint32(info, "unix::mode", e.mode);
        g_file_info_set_attribute_uint32(info, "unix::uid", e.uid);
        g_file_info_set_attribute_uint32(info, "unix::gid", e.gid);
        g_file_info_set_attribute_uint32(info, "fw::flags", e.flags);
        g_file_info_set_attribute_int64(info, "fw::mtime", e.mtime);
        g_object_set_data_full(G_OBJECT(info), "sort-key", g_utf8_collate_key_for_filename(display, -1), g_free);
        if (e.link_len) {
            char *link = g_strndup(name+e.name_len, e.link_len);
            g_file_info_set_symlink_target(info, link); g_free(link);
        }
        g_ptr_array_add(self->pending, info); g_free(raw); g_free(display); off += record;
    }
    return off == size;
}
static gboolean validate_owners(const guint8 *data, guint32 size)
{
    guint32 off = 0;
    while (off < size) {
        if (size-off < sizeof(wslp_owner)) return FALSE;
        wslp_owner owner; memcpy(&owner, data+off, sizeof owner);
        guint32 n = wslp_pad8(sizeof owner+owner.len);
        if (n > size-off || owner.kind > WSLP_OWNER_GROUP) return FALSE;
        off += n;
    }
    return off == size;
}
static gboolean dispatch(FwBrowser *self, Io *io)
{
    guint32 n = io->head.size, id = io->head.id;
    if (!io->hello) {
        wslp_hello hi;
        if (io->head.type != WSLP_HELLO || id || n != sizeof hi) return FALSE;
        memcpy(&hi, io->payload, sizeof hi);
        if (hi.magic != WSLP_MAGIC || hi.version != WSLP_VERSION || hi.uid != (guint32)getuid()) return FALSE;
        io->hello = TRUE; return TRUE;
    }
    switch (io->head.type) {
    case WSLP_ENTRIES:
        return id == 2 && self->busy && parse_entries(self, io->payload, n);
    case WSLP_OWNERS:
        return id == 2 && self->busy && validate_owners(io->payload, n);
    case WSLP_WATCHED: {
        if (id != 1 || n != sizeof(wslp_end)) return FALSE;
        wslp_end end; memcpy(&end, io->payload, sizeof end);
        if (end.err < 0 || end.err > 4095) return FALSE;
        self->watching = !end.err;
        g_free(self->error);
        self->error = end.err ? g_strdup_printf("Live updates unavailable: %s", g_strerror(end.err)) : g_strdup("");
        announce(self); return TRUE;
    }
    case WSLP_CHANGED:
        if (id != 1 || n) return FALSE;
        self->dirty = TRUE; schedule_refresh(self); return TRUE;
    case WSLP_LIST_END: {
        if (id != 2 || n != sizeof(wslp_end) || !self->busy) return FALSE;
        wslp_end end; memcpy(&end, io->payload, sizeof end);
        if (end.err < 0 || end.err > 4095 || end.count != self->pending->len) return FALSE;
        if (end.err) {
            char *path = fw_display_bytes(io->target);
            char *message = g_strdup_printf("Cannot list %s: %s. %s", path, g_strerror(end.err), self->revision ? "Previous snapshot retained." : "No snapshot available.");
            fail(self, message); g_free(message); g_free(path); return TRUE;
        }
        if (self->deadline) { g_source_remove(self->deadline); self->deadline = 0; }
        self->busy = FALSE;
        g_free(self->path); self->path = g_strdup(io->target);
        self->elapsed = (g_get_monotonic_time()-self->started)/1000.0; self->revision++;
        g_list_store_splice(self->entries, 0, g_list_model_get_n_items(G_LIST_MODEL(self->entries)), self->pending->pdata, self->pending->len);
        g_ptr_array_set_size(self->pending, 0);
        announce(self); if (self->dirty) schedule_refresh(self); return TRUE;
    }
    default: return FALSE;
    }
}
static void read_done(GObject *source, GAsyncResult *result, gpointer data)
{
    Io *io = data; GError *error = NULL; gsize count = 0;
    gboolean ok = g_input_stream_read_all_finish(G_INPUT_STREAM(source), result, &count, &error);
    FwBrowser *self = io_owner(io);
    if (self) {
        if (!ok || count != io->head.size) fail(self, "Directory helper disconnected. Refresh to retry.");
        else if (!dispatch(self, io)) fail(self, "Invalid directory-helper response or snapshot exceeds 200,000 entries. Previous snapshot retained.");
        if (self->io == io) read_head(io);
        g_object_unref(self);
    }
    g_clear_error(&error); io_unref(io);
}
static void head_done(GObject *source, GAsyncResult *result, gpointer data)
{
    Io *io = data; GError *error = NULL; gsize count = 0;
    gboolean ok = g_input_stream_read_all_finish(G_INPUT_STREAM(source), result, &count, &error);
    FwBrowser *self = io_owner(io);
    if (self) {
        if (!ok || count != sizeof io->head) fail(self, "Directory helper disconnected. Refresh to retry.");
        else if (io->head.size > WSLP_MAX) fail(self, "Directory helper sent an oversized frame.");
        else if (!io->head.size) {
            if (!dispatch(self, io)) fail(self, "Invalid directory-helper notification.");
            if (self->io == io) read_head(io);
        } else {
            g_free(io->payload); io->payload = g_malloc(io->head.size);
            g_input_stream_read_all_async(G_INPUT_STREAM(source), io->payload, io->head.size,
                G_PRIORITY_DEFAULT, io->cancel, read_done, io_ref(io));
        }
        g_object_unref(self);
    }
    g_clear_error(&error); io_unref(io);
}
static void read_head(Io *io)
{
    g_input_stream_read_all_async(g_subprocess_get_stdout_pipe(io->process), &io->head, sizeof io->head,
        G_PRIORITY_DEFAULT, io->cancel, head_done, io_ref(io));
}
typedef struct Write { Io *io; GByteArray *bytes; } Write;
static void write_done(GObject *source, GAsyncResult *result, gpointer data)
{
    Write *write = data; Io *io = write->io; GError *error = NULL; gsize count;
    gboolean ok = g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result, &count, &error);
    FwBrowser *self = io_owner(io); io->writing = FALSE;
    if (self) {
        if (!ok) fail(self, "Cannot send request to directory helper. Refresh to retry.");
        else if (self->dirty) schedule_refresh(self);
        g_object_unref(self);
    }
    g_clear_error(&error); g_byte_array_unref(write->bytes); io_unref(io); g_free(write);
}
static void request_list(FwBrowser *self, gboolean initial)
{
    Io *io = self->io; self->busy = TRUE; self->dirty = FALSE;
    g_ptr_array_set_size(self->pending, 0); self->started = g_get_monotonic_time();
    self->deadline = g_timeout_add_seconds(15, timeout_request, self);
    Write *write = g_new0(Write, 1); write->io = io_ref(io); write->bytes = g_byte_array_new();
    guint32 size = (guint32)strlen(io->target);
    if (!initial) {
        wslp_head unwatch = { WSLP_UNWATCH, 0, 1 };
        g_byte_array_append(write->bytes, (guint8 *)&unwatch, sizeof unwatch);
    }
    self->watching = FALSE;
    {
        wslp_head head = { WSLP_WATCH, size, 1 };
        g_byte_array_append(write->bytes, (guint8 *)&head, sizeof head);
        g_byte_array_append(write->bytes, (guint8 *)io->target, size);
    }
    wslp_head head = { WSLP_LIST, size, 2 };
    g_byte_array_append(write->bytes, (guint8 *)&head, sizeof head);
    g_byte_array_append(write->bytes, (guint8 *)io->target, size);
    io->writing = TRUE;
    g_output_stream_write_all_async(g_subprocess_get_stdin_pipe(io->process), write->bytes->data, write->bytes->len,
        G_PRIORITY_DEFAULT, io->cancel, write_done, write);
    announce(self);
}
static gboolean refresh_due(gpointer data)
{
    FwBrowser *self = data; self->refresh = 0;
    if (self->io && !self->busy && !self->io->writing) request_list(self, FALSE);
    return G_SOURCE_REMOVE;
}
static void schedule_refresh(FwBrowser *self)
{
    if (!self->refresh && !self->busy && self->io && !self->io->writing)
        self->refresh = g_timeout_add(100, refresh_due, self);
}
void fw_browser_navigate(FwBrowser *self, const char *absolute_path)
{
    if (self->closed) return;
    if (!g_path_is_absolute(absolute_path) || strlen(absolute_path) > PATH_LIMIT) {
        fail(self, "Expected an absolute Linux path of at most 16 KiB."); return;
    }
    char *target = g_strdup(absolute_path); /* Caller may pass the current path. */
    g_free(self->attempted); self->attempted = g_strdup(target);
    disconnect_io(self); g_free(self->error); self->error = g_strdup("");
    GError *error = NULL;
    GSubprocess *process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE,
        &error, self->helper, NULL);
    if (!process) {
        fail(self, error->message); g_error_free(error); g_free(target); return;
    }
    Io *io = g_new0(Io, 1); io->refs = 1; io->process = process;
    io->cancel = g_cancellable_new(); io->target = target;
    g_weak_ref_init(&io->owner, self); self->io = io;
    read_head(io); request_list(self, TRUE);
}
void fw_browser_refresh(FwBrowser *self)
{
    if (self->closed) return;
    if (!self->io) {
        const char *retry = *self->path ? self->path : self->attempted;
        if (retry) fw_browser_navigate(self, retry);
        return;
    }
    self->dirty = TRUE; schedule_refresh(self);
}
