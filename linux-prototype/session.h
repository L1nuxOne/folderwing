#ifndef FW_SESSION_H
#define FW_SESSION_H
#include <gio/gio.h>
#define FW_SESSION_TABS 32u
#define FW_SESSION_LIMIT (2u*1024u*1024u)
typedef struct FwSession {
    GPtrArray *paths[2];guint active[2],pane;gboolean hidden,maximized;
    gint width,height,split; /* split in 1/10000 of the window width */
} FwSession;
FwSession *fw_session_new(void);
void fw_session_free(FwSession *session);
char *fw_session_encode(const FwSession *session,gsize *size,GError **error);
FwSession *fw_session_decode(const char *bytes,gsize size,GError **error);
char *fw_session_default_path(void);
#define FW_TYPE_SESSION_STORE (fw_session_store_get_type())
G_DECLARE_FINAL_TYPE(FwSessionStore,fw_session_store,FW,SESSION_STORE,GObject)
typedef struct FwSessionLoad { FwSession *session;char *warning;gboolean writable; } FwSessionLoad;
void fw_session_load_free(FwSessionLoad *load);
FwSessionStore *fw_session_store_new(const char *path);
void fw_session_load_async(FwSessionStore *store,GAsyncReadyCallback callback,gpointer data);
FwSessionLoad *fw_session_load_finish(FwSessionStore *store,GAsyncResult *result,GError **error);
void fw_session_save_async(FwSessionStore *store,const FwSession *session,GAsyncReadyCallback callback,gpointer data);
gboolean fw_session_save_finish(FwSessionStore *store,GAsyncResult *result,GError **error);
#endif
