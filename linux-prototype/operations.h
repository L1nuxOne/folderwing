/* Native operations coordinator. All filesystem mutations belong to --jobs. */
#pragma once
#include <gio/gio.h>
#define FW_TYPE_JOB (fw_job_get_type())
G_DECLARE_FINAL_TYPE(FwJob, fw_job, FW, JOB, GObject)
typedef enum { FW_COPY=0, FW_MOVE=1, FW_TRASH=3 } FwOperation;
typedef enum { FW_KEEP_BOTH, FW_SKIP, FW_REPLACE, FW_FAIL_CONFLICT } FwConflict;
typedef enum { FW_JOB_IDLE, FW_JOB_SCANNING, FW_JOB_RUNNING, FW_JOB_COMPLETE,
    FW_JOB_CANCELLED, FW_JOB_ERRORS, FW_JOB_UNCONFIRMED } FwJobState;
typedef struct FwJobProgress {
    FwJobState state;
    guint64 bytes_done, bytes_total;
    guint items_done, items_total, errors, skipped;
    gboolean active, cancel_requested, cancel_sent;
    char *message, *details; /* UTF-8; free with fw_job_progress_clear */
} FwJobProgress;
/* Paths/env are cloned. NULL env captures the current environment. At most
 * 4096 absolute native sources and a bounded 16 MiB wire request. */
FwJob *fw_job_new(const char *helper, FwOperation operation, const char *const *sources,
    guint count, const char *destination, FwConflict policy, const char *const *env, GError **error);
void fw_job_start(FwJob *job);
void fw_job_cancel(FwJob *job); /* cooperative; NEVER turns a request into completion */
void fw_job_progress(FwJob *job, FwJobProgress *out);
void fw_job_progress_clear(FwJobProgress *progress);
