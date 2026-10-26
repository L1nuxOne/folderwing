#define _GNU_SOURCE
#include "operations.h"
#include "model.h"
#include "../src/wslproto.h"
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/vfs.h>

struct _FwJob {
    GObject parent;
    char *helper, **sources, *destination, **env;
    guint count, operation, policy;
    GBytes *request;
    GMutex lock;
    FwJobProgress progress;
    GCancellable *cancel;
    GMainContext *context;
    gint notification;
    gboolean started;
};
G_DEFINE_TYPE(FwJob, fw_job, G_TYPE_OBJECT)
static guint changed;
static gboolean emit_changed(gpointer data)
{
    FwJob *job = data; g_atomic_int_set(&job->notification, 0);
    g_signal_emit(job, changed, 0); return G_SOURCE_REMOVE;
}
static void notify(FwJob *job)
{
    if (!g_atomic_int_compare_and_exchange(&job->notification, 0, 1)) return;
    GSource *source = g_idle_source_new();
    g_source_set_callback(source, emit_changed, g_object_ref(job), g_object_unref);
    g_source_attach(source, job->context); g_source_unref(source);
}
static void message(FwJob *job, const char *text)
{
    g_mutex_lock(&job->lock); g_free(job->progress.message); job->progress.message = g_strdup(text); g_mutex_unlock(&job->lock); notify(job);
}
static void fw_job_finalize(GObject *object)
{
    FwJob *job = FW_JOB(object);
    g_free(job->helper); g_strfreev(job->sources); g_free(job->destination); g_strfreev(job->env);
    g_bytes_unref(job->request); g_free(job->progress.message); g_free(job->progress.details);
    g_clear_object(&job->cancel); g_main_context_unref(job->context); g_mutex_clear(&job->lock);
    G_OBJECT_CLASS(fw_job_parent_class)->finalize(object);
}
static void fw_job_class_init(FwJobClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = fw_job_finalize;
    changed = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}
static void fw_job_init(FwJob *job)
{
    g_mutex_init(&job->lock); job->cancel = g_cancellable_new(); job->context = g_main_context_ref_thread_default();
    job->progress.message = g_strdup("Ready"); job->progress.details = g_strdup("");
    job->request = g_bytes_new_static("", 0);
}
void fw_job_progress(FwJob *job, FwJobProgress *out)
{
    g_mutex_lock(&job->lock); *out = job->progress;
    out->message = g_strdup(job->progress.message); out->details = g_strdup(job->progress.details);
    g_mutex_unlock(&job->lock); out->cancel_requested = g_cancellable_is_cancelled(job->cancel);
}
void fw_job_progress_clear(FwJobProgress *progress) { g_free(progress->message); g_free(progress->details); memset(progress, 0, sizeof *progress); }
void fw_job_cancel(FwJob *job) { g_cancellable_cancel(job->cancel); notify(job); }
FwJob *fw_job_new(const char *helper, FwOperation operation, const char *const *sources,
    guint count, const char *destination, FwConflict policy, const char *const *env, GError **error)
{
    if (!helper || !*helper || !sources || !count || count > 4096 || policy > FW_FAIL_CONFLICT ||
        (operation != FW_COPY && operation != FW_MOVE && operation != FW_TRASH) ||
        (operation != FW_TRASH && (!destination || !g_path_is_absolute(destination)))) goto invalid;
    GByteArray *bytes = g_byte_array_new();
    wslp_job body = {operation, policy, count, 0}; g_byte_array_append(bytes, (guint8 *)&body, sizeof body);
    for (guint i = 0; i <= count; i++) {
        const char *path = i ? sources[i-1] : operation == FW_TRASH ? "" : destination;
        if (!path || (i && !g_path_is_absolute(path)) || strlen(path) > 16384) { g_byte_array_unref(bytes); goto invalid; }
        wslp_path item = {(guint32)strlen(path), 0};
        g_byte_array_append(bytes, (guint8 *)&item, sizeof item); g_byte_array_append(bytes, (guint8 *)path, item.bytes);
        if (bytes->len > WSLP_JOB_MAX) { g_byte_array_unref(bytes); goto invalid; }
    }
    FwJob *job = g_object_new(FW_TYPE_JOB, NULL);
    job->helper = g_strdup(helper); job->count = count; job->operation = operation; job->policy = policy;
    job->destination = g_strdup(operation == FW_TRASH ? "" : destination);
    job->sources = g_new0(char *, count+1); for (guint i = 0; i < count; i++) job->sources[i] = g_strdup(sources[i]);
    job->env = env ? g_strdupv((char **)env) : g_get_environ();
    g_bytes_unref(job->request); job->request = g_byte_array_free_to_bytes(bytes); return job;
invalid:
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Choose 1–4096 native Linux paths and a valid destination (16 KiB per path, 16 MiB per request).");
    return NULL;
}
typedef struct Wire {
    FwJob *job;
    GSubprocess *process;
    int input, output, cancel_fd;
    gboolean request_sent, cancel_sent, continued, terminal;
    guint terminal_state, error_events, skip_events;
    int failure;
} Wire;
static gboolean write_exact(Wire *wire, const void *data, gsize size)
{
    const char *at = data;
    while (size) {
        struct pollfd fd = {wire->input, POLLOUT, 0};
        int ready; do ready = poll(&fd, 1, 15000); while (ready < 0 && errno == EINTR);
        if (ready <= 0) { wire->failure = ready ? errno : ETIMEDOUT; return FALSE; }
        ssize_t n = write(wire->input, at, size);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) { wire->failure = n ? errno : EPIPE; return FALSE; }
        at += n; size -= (gsize)n;
    }
    return TRUE;
}
static gboolean control(Wire *wire, guint type)
{
    wslp_head head = {type, 0, 1}; return write_exact(wire, &head, sizeof head);
}
static gboolean send_cancel(Wire *wire)
{
    if (!control(wire, WSLP_JOB_CANCEL)) return FALSE;
    wire->cancel_sent = TRUE;
    g_mutex_lock(&wire->job->lock); wire->job->progress.cancel_sent = TRUE; g_mutex_unlock(&wire->job->lock);
    notify(wire->job); return TRUE;
}
/* 1 complete, 0 clean EOF, -1 truncated/error. Only the worker owns pipe IO. */
static int read_exact(Wire *wire, void *data, gsize size)
{
    guint8 *at = data; gsize left = size;
    while (left) {
        gboolean cancelled = g_cancellable_is_cancelled(wire->job->cancel);
        if (cancelled && !wire->request_sent) { wire->failure = ECANCELED; return -1; }
        if (cancelled && wire->request_sent && !wire->cancel_sent && !wire->terminal) {
            if (!send_cancel(wire)) return -1;
        }
        struct pollfd fds[2] = {{wire->output, POLLIN, 0}, {wire->cancel_fd, POLLIN, 0}};
        int ready, count = !wire->cancel_sent && !wire->terminal ? 2 : 1;
        do ready = poll(fds, count, wire->continued && !wire->terminal ? 30000 : 15000); while (ready < 0 && errno == EINTR);
        if (!ready && wire->continued && !wire->terminal) { message(wire->job, "Waiting for the filesystem helper to finish; cancellation remains available."); continue; }
        if (ready <= 0) { wire->failure = ready ? errno : ETIMEDOUT; return -1; }
        if (!(fds[0].revents & (POLLIN|POLLHUP|POLLERR))) continue;
        ssize_t n = read(wire->output, at, left);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n < 0) { wire->failure = errno; return -1; }
        if (!n) { if (left != size) wire->failure = EPROTO; return left == size ? 0 : -1; }
        at += n; left -= (gsize)n;
    }
    return 1;
}
static gboolean valid_path(Wire *wire, guint root, guint bytes, const guint8 *path, guint available)
{
    return bytes == available && bytes < 65536 && (root == WSLP_NO_ROOT || root <= wire->job->count) && !memchr(path, 0, bytes);
}
static void event_detail(Wire *wire, const wslp_event *event, const guint8 *suffix)
{
    const char *base = event->root == WSLP_NO_ROOT ? "" : event->root ? wire->job->sources[event->root-1] : wire->job->destination;
    char *tail = g_strndup((char *)suffix, event->path_bytes), *raw = g_strconcat(base, tail, NULL), *shown = fw_display_bytes(raw);
    char *line = g_strdup_printf("%s: %s (%s)\n", event->kind ? "Skipped" : "Error", shown, g_strerror(event->error));
    if (g_utf8_strlen(line, -1) > 2048) {
        char *prefix = g_utf8_substring(line, 0, 2048), *shortened = g_strconcat(prefix, "… (path shortened)\n", NULL);
        g_free(prefix); g_free(line); line = shortened;
    }
    g_mutex_lock(&wire->job->lock);
    if (strlen(wire->job->progress.details)+strlen(line) <= 16384) {
        char *details = g_strconcat(wire->job->progress.details, line, NULL); g_free(wire->job->progress.details); wire->job->progress.details = details;
    }
    g_mutex_unlock(&wire->job->lock);
    g_free(line); g_free(shown); g_free(raw); g_free(tail); notify(wire->job);
}
static gboolean dispatch(Wire *wire, const wslp_head *head, const guint8 *data)
{
    if (head->id != 1 || wire->terminal) return FALSE;
    if (head->type == WSLP_JOB_EVENT && head->size >= sizeof(wslp_event)) {
        wslp_event event; memcpy(&event, data, sizeof event);
        if (event.kind > 1 || event.error <= 0 || event.error > 4095 ||
            !valid_path(wire, event.root, event.path_bytes, data+sizeof event, head->size-sizeof event)) return FALSE;
        if (event.kind) wire->skip_events++; else wire->error_events++;
        event_detail(wire, &event, data+sizeof event); return TRUE;
    }
    if (head->type != WSLP_JOB_PROGRESS || head->size < sizeof(wslp_progress)) return FALSE;
    wslp_progress progress; memcpy(&progress, data, sizeof progress);
    if (progress.state < 1 || progress.state > 5 || progress.error < 0 || progress.error > 4095 ||
        !valid_path(wire, progress.root, progress.path_bytes, data+sizeof progress, head->size-sizeof progress) ||
        (progress.state == 3 && !wire->continued) || progress.errors < wire->error_events || progress.skipped < wire->skip_events ||
        (!!progress.errors != !!progress.error)) return FALSE;
    if (progress.state >= 3) { wire->terminal = TRUE; wire->terminal_state = progress.state; }
    g_mutex_lock(&wire->job->lock);
    FwJobProgress *out = &wire->job->progress;
    out->state = progress.state == 1 ? FW_JOB_SCANNING : FW_JOB_RUNNING;
    out->bytes_done = progress.bytes_done; out->bytes_total = progress.bytes_total;
    out->items_done = progress.items_done; out->items_total = progress.items_total; out->errors = progress.errors; out->skipped = progress.skipped;
    g_free(out->message); out->message = g_strdup(wire->terminal ? "Finalizing helper result…" : progress.state == 1 ? "Scanning…" : "Working…");
    g_mutex_unlock(&wire->job->lock); notify(wire->job);
    if (progress.state == 2 && !wire->continued && !wire->cancel_sent) {
        if (g_cancellable_is_cancelled(wire->job->cancel)) return send_cancel(wire);
        if (!control(wire, WSLP_JOB_CONTINUE)) return FALSE;
        wire->continued = TRUE;
    }
    return TRUE;
}
typedef struct Result { FwJobState state; char *message; } Result;
static void result_free(gpointer data) { Result *r = data; g_free(r->message); g_free(r); }
static void worker(GTask *task, gpointer source, gpointer unused, GCancellable *cancellable)
{
    (void)unused; (void)cancellable; FwJob *job = source; Result *result = g_new0(Result, 1);
    Wire wire = {.job=job, .input=-1, .output=-1, .cancel_fd=g_cancellable_get_fd(job->cancel)};
    GError *error = NULL;
    if (g_cancellable_is_cancelled(job->cancel)) { result->state = FW_JOB_CANCELLED; result->message = g_strdup("Cancelled before starting."); goto done; }
    /* DrvFS/9P paths need the Windows coordinator's stream inventory and
     * Windows durability ACKs. Never quietly label them native Linux paths. */
    for (guint i = 0; i <= job->count; i++) {
        if (!i && job->operation == FW_TRASH) continue;
        char *parent = i ? g_path_get_dirname(job->sources[i-1]) : g_strdup(job->destination);
        struct statfs fs;
        int checked = statfs(parent, &fs); g_free(parent);
        if (!checked && (unsigned long)fs.f_type == 0x01021997ul) {
            g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Transfers involving mounted Windows/9P drives require the Windows Folderwing app.");
            goto failed;
        }
    }
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE|G_SUBPROCESS_FLAGS_STDOUT_PIPE);
    g_subprocess_launcher_set_environ(launcher, job->env);
    wire.process = g_subprocess_launcher_spawn(launcher, &error, job->helper, "--jobs", NULL); g_object_unref(launcher);
    if (!wire.process) goto failed;
    wire.input = g_unix_output_stream_get_fd(G_UNIX_OUTPUT_STREAM(g_subprocess_get_stdin_pipe(wire.process)));
    wire.output = g_unix_input_stream_get_fd(G_UNIX_INPUT_STREAM(g_subprocess_get_stdout_pipe(wire.process)));
    if (fcntl(wire.input, F_SETFL, fcntl(wire.input, F_GETFL)|O_NONBLOCK) ||
        fcntl(wire.output, F_SETFL, fcntl(wire.output, F_GETFL)|O_NONBLOCK)) { wire.failure = errno; goto failed; }
    wslp_head head; wslp_hello hello;
    if (read_exact(&wire, &head, sizeof head) != 1 || head.type != WSLP_HELLO || head.id || head.size != sizeof hello ||
        read_exact(&wire, &hello, sizeof hello) != 1 || hello.magic != WSLP_MAGIC || hello.version != WSLP_VERSION || hello.uid != (guint)getuid()) { wire.failure = EPROTO; goto failed; }
    gsize bytes; const void *request = g_bytes_get_data(job->request, &bytes); head = (wslp_head){WSLP_JOB, (guint)bytes, 1};
    if (!write_exact(&wire, &head, sizeof head) || !write_exact(&wire, request, bytes)) goto failed;
    wire.request_sent = TRUE;
    for (;;) {
        int got = read_exact(&wire, &head, sizeof head);
        if (!got) break;
        if (got < 0 || head.size > WSLP_MAX) { if (!wire.failure) wire.failure = EPROTO; goto failed; }
        guint8 *data = g_malloc(MAX(head.size, 1u));
        gboolean valid = (!head.size || read_exact(&wire, data, head.size) == 1) && dispatch(&wire, &head, data);
        g_free(data); if (!valid) { if (!wire.failure) wire.failure = EPROTO; goto failed; }
    }
    if (!wire.terminal || !g_subprocess_wait_check(wire.process, NULL, &error)) goto failed;
    g_mutex_lock(&job->lock);
    guint errors = job->progress.errors, skipped = job->progress.skipped;
    g_mutex_unlock(&job->lock);
    result->state = wire.terminal_state == 4 ? FW_JOB_CANCELLED : errors || wire.terminal_state == 5 ? FW_JOB_ERRORS : FW_JOB_COMPLETE;
    result->message = g_strdup_printf("%s · %u errors · %u skipped", result->state == FW_JOB_CANCELLED ? "Cancelled; completed items were kept" : result->state == FW_JOB_ERRORS ? "Finished with errors; review the details" : "Completed", errors, skipped);
    goto done;
failed:
    result->state = wire.continued ? FW_JOB_UNCONFIRMED : wire.cancel_sent || g_cancellable_is_cancelled(job->cancel) ? FW_JOB_CANCELLED : FW_JOB_ERRORS;
    result->message = g_strdup_printf("%s: %s", wire.continued ? "Completion unconfirmed; review both folders" : result->state == FW_JOB_CANCELLED ? "Cancelled before mutation" : "Operation did not start", error ? error->message : g_strerror(wire.failure ? wire.failure : EPIPE));
    if (wire.process) { g_subprocess_force_exit(wire.process); g_subprocess_wait(wire.process, NULL, NULL); }
done:
    g_clear_error(&error); g_clear_object(&wire.process); g_cancellable_release_fd(job->cancel);
    g_task_return_pointer(task, result, result_free);
}
static void finished(GObject *source, GAsyncResult *async, gpointer unused)
{
    (void)unused; FwJob *job = FW_JOB(source); Result *result = g_task_propagate_pointer(G_TASK(async), NULL);
    g_mutex_lock(&job->lock); job->progress.active = FALSE; job->progress.state = result->state;
    g_free(job->progress.message); job->progress.message = g_strdup(result->message);
    if ((result->state == FW_JOB_ERRORS || result->state == FW_JOB_UNCONFIRMED) && !job->progress.errors) job->progress.errors = 1;
    g_mutex_unlock(&job->lock); result_free(result); g_signal_emit(job, changed, 0);
}
void fw_job_start(FwJob *job)
{
    if (job->started) return;
    job->started = TRUE; job->progress.active = TRUE; job->progress.state = FW_JOB_SCANNING;
    message(job, "Scanning…");
    GTask *task = g_task_new(job, NULL, finished, NULL); g_task_run_in_thread(task, worker); g_object_unref(task);
}
