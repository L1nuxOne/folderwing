#ifndef FW_DESKTOP_H
#define FW_DESKTOP_H
#include <gio/gio.h>
typedef struct FwDesktopTarget {
    char *path,*uri,*content_type;
    GList *applications;
} FwDesktopTarget;
/* Metadata/association discovery stays off the GTK thread. Follows symlinks,
   rejects special nodes, and never executes a filename as a command. */
void fw_desktop_query_async(const char *path,gboolean directory,gboolean applications,
    GCancellable *cancel,GAsyncReadyCallback callback,gpointer data);
FwDesktopTarget *fw_desktop_query_finish(GAsyncResult *result,GError **error);
void fw_desktop_target_free(FwDesktopTarget *target);
GAppInfo *fw_desktop_terminal_app(const char *helper,const char *path,GError **error);
#endif
