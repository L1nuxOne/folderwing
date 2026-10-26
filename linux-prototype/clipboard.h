#ifndef FW_CLIPBOARD_H
#define FW_CLIPBOARD_H
#include <gtk/gtk.h>
#define FW_CLIPBOARD_LIMIT (16u*1024u*1024u)
#define FW_CLIPBOARD_GNOME "x-special/gnome-copied-files"
#define FW_CLIPBOARD_NAUTILUS "x-special/nautilus-clipboard"
#define FW_CLIPBOARD_URIS "text/uri-list"
#define FW_CLIPBOARD_KDE "application/x-kde-cutselection"
typedef struct FwClipboardData { char **paths;guint count;gboolean cut; } FwClipboardData;
void fw_clipboard_data_free(FwClipboardData *data);
FwClipboardData *fw_clipboard_parse(const char *mime,const void *bytes,gsize size,
    const void *kde,gsize kde_size,GError **error);
GdkContentProvider *fw_clipboard_provider(const char *const *paths,guint count,gboolean cut,GError **error);
gboolean fw_clipboard_has_files(GdkClipboard *clipboard);
/* Reads at most 16 MiB, times out after 15 seconds, and refuses a changed offer.
 * No text-to-file fallback. The caller owns the destination/review lifecycle. */
void fw_clipboard_read_async(GdkClipboard *clipboard,GCancellable *cancel,GAsyncReadyCallback callback,gpointer data);
FwClipboardData *fw_clipboard_read_finish(GAsyncResult *result,GError **error);
#endif
