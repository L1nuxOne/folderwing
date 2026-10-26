#define _GNU_SOURCE
#include "session.h"
#include <glib/gstdio.h>
#include <sys/file.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
FwSession *fw_session_new(void)
{
    FwSession *s=g_new0(FwSession,1);for(guint i=0;i<2;i++)s->paths[i]=g_ptr_array_new_with_free_func(g_free);
    s->width=1280;s->height=760;s->split=5000;return s;
}
void fw_session_free(FwSession *s) { if(s){g_ptr_array_unref(s->paths[0]);g_ptr_array_unref(s->paths[1]);g_free(s);} }
static gboolean valid_session(const FwSession *s)
{
    if(!s||s->pane>1||s->width<640||s->width>8192||s->height<400||s->height>8192||s->split<1000||s->split>9000)return FALSE;
    for(guint i=0;i<2;i++){
        if(!s->paths[i]||!s->paths[i]->len||s->paths[i]->len>FW_SESSION_TABS||s->active[i]>=s->paths[i]->len)return FALSE;
        for(guint j=0;j<s->paths[i]->len;j++){const char *p=s->paths[i]->pdata[j];if(!p||!g_path_is_absolute(p)||strlen(p)>16384)return FALSE;}
    }
    return TRUE;
}
char *fw_session_encode(const FwSession *s,gsize *size,GError **error)
{
    if(!valid_session(s)){g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA,"Invalid Linux session layout.");return NULL;}
    GKeyFile *key=g_key_file_new();g_key_file_set_integer(key,"Session","version",1);g_key_file_set_integer(key,"Session","pane",s->pane);
    g_key_file_set_boolean(key,"Session","hidden",s->hidden);g_key_file_set_boolean(key,"Session","maximized",s->maximized);
    g_key_file_set_integer(key,"Session","width",s->width);g_key_file_set_integer(key,"Session","height",s->height);g_key_file_set_integer(key,"Session","split",s->split);
    for(guint i=0;i<2;i++){
        const char *group=i?"Right":"Left";g_key_file_set_integer(key,group,"count",s->paths[i]->len);g_key_file_set_integer(key,group,"active",s->active[i]);
        for(guint j=0;j<s->paths[i]->len;j++){char name[32];g_snprintf(name,sizeof name,"tab%u",j);const char *path=s->paths[i]->pdata[j];char *encoded=g_base64_encode((const guchar *)path,strlen(path));g_key_file_set_string(key,group,name,encoded);g_free(encoded);}
    }
    char *out=g_key_file_to_data(key,size,error);g_key_file_unref(key);
    if(out&&*size>FW_SESSION_LIMIT){g_free(out);out=NULL;g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA,"Linux session exceeds 2 MiB.");}return out;
}
FwSession *fw_session_decode(const char *bytes,gsize size,GError **error)
{
    if(!bytes||!size||size>FW_SESSION_LIMIT||memchr(bytes,0,size)){g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA,"Invalid bounded session file.");return NULL;}
    GKeyFile *key=g_key_file_new();FwSession *s=fw_session_new();GError *e=NULL;
    if(!g_key_file_load_from_data(key,bytes,size,G_KEY_FILE_NONE,&e))goto bad;
    if(g_key_file_get_integer(key,"Session","version",&e)!=1||e)goto bad;
    s->pane=g_key_file_get_integer(key,"Session","pane",&e);if(e)goto bad;
    s->hidden=g_key_file_get_boolean(key,"Session","hidden",&e);if(e)goto bad;
    s->maximized=g_key_file_get_boolean(key,"Session","maximized",&e);if(e)goto bad;
    s->width=g_key_file_get_integer(key,"Session","width",&e);if(e)goto bad;s->height=g_key_file_get_integer(key,"Session","height",&e);if(e)goto bad;s->split=g_key_file_get_integer(key,"Session","split",&e);if(e)goto bad;
    for(guint i=0;i<2;i++){
        const char *group=i?"Right":"Left";gint count=g_key_file_get_integer(key,group,"count",&e);if(e||count<1||count>(gint)FW_SESSION_TABS)goto bad;
        s->active[i]=g_key_file_get_integer(key,group,"active",&e);if(e)goto bad;
        for(gint j=0;j<count;j++){
            char name[32];g_snprintf(name,sizeof name,"tab%d",j);char *encoded=g_key_file_get_string(key,group,name,&e);if(e)goto bad;
            gsize n;guchar *raw=g_base64_decode(encoded,&n);char *canonical=g_base64_encode(raw,n);gboolean valid=n>0&&n<=16384&&!memchr(raw,0,n)&&!strcmp(canonical,encoded);g_free(canonical);g_free(encoded);
            if(!valid){g_free(raw);goto bad;}g_ptr_array_add(s->paths[i],g_strndup((const char *)raw,n));g_free(raw);
        }
    }
    if(!valid_session(s))goto bad;
    g_key_file_unref(key);return s;
bad:
    g_clear_error(&e);g_key_file_unref(key);fw_session_free(s);g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA,"Unsupported or malformed Linux session layout.");return NULL;
}
char *fw_session_default_path(void)
{
    const char *state=g_getenv("XDG_STATE_HOME");char *base=state&&g_path_is_absolute(state)?g_strdup(state):g_build_filename(g_get_home_dir(),".local","state",NULL);
    char *path=g_build_filename(base,"folderwing","linux-prototype","session.ini",NULL);g_free(base);return path;
}
struct _FwSessionStore { GObject parent;char *path;int lock; };
G_DEFINE_TYPE(FwSessionStore,fw_session_store,G_TYPE_OBJECT)
static void store_finalize(GObject *object)
{
    FwSessionStore *s=FW_SESSION_STORE(object);if(s->lock>=0)close(s->lock);g_free(s->path);G_OBJECT_CLASS(fw_session_store_parent_class)->finalize(object);
}
static void fw_session_store_class_init(FwSessionStoreClass *klass) { G_OBJECT_CLASS(klass)->finalize=store_finalize; }
static void fw_session_store_init(FwSessionStore *store) { store->lock=-1; }
FwSessionStore *fw_session_store_new(const char *path)
{ FwSessionStore *store=g_object_new(FW_TYPE_SESSION_STORE,NULL);store->path=g_strdup(path);return store; }
void fw_session_load_free(FwSessionLoad *load) { if(load){fw_session_free(load->session);g_free(load->warning);g_free(load);} }
static void load_worker(GTask *task,gpointer object,gpointer data,GCancellable *cancel)
{
    (void)data;(void)cancel;FwSessionStore *store=object;FwSessionLoad *out=g_new0(FwSessionLoad,1);GError *error=NULL;
    if(!g_path_is_absolute(store->path)){g_task_return_new_error(task,G_IO_ERROR,G_IO_ERROR_INVALID_ARGUMENT,"Session path must be absolute.");fw_session_load_free(out);return;}
    char *parent=g_path_get_dirname(store->path),*lock=g_strconcat(store->path,".lock",NULL);
    if(!g_mkdir_with_parents(parent,0700)){
        store->lock=g_open(lock,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
        if(store->lock>=0&&flock(store->lock,LOCK_EX|LOCK_NB)){close(store->lock);store->lock=-1;}
    }
    g_free(parent);g_free(lock);out->writable=store->lock>=0;
    if(!out->writable)out->warning=g_strdup("Session saving is unavailable or another window owns this session. This window will not overwrite it.");
    GFile *file=g_file_new_for_path(store->path);GFileInfo *info=g_file_query_info(file,G_FILE_ATTRIBUTE_STANDARD_TYPE,G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,NULL,&error);
    if(!info){
        if(!g_error_matches(error,G_IO_ERROR,G_IO_ERROR_NOT_FOUND)){g_free(out->warning);out->warning=g_strdup_printf("Cannot read saved session: %s",error->message);out->writable=FALSE;}
        g_clear_error(&error);goto done;
    }
    if(g_file_info_get_file_type(info)!=G_FILE_TYPE_REGULAR){out->writable=FALSE;g_free(out->warning);out->warning=g_strdup("The session path is not a regular file; it was preserved and saving is disabled.");g_object_unref(info);goto done;}
    g_object_unref(info);GFileInputStream *stream=g_file_read(file,NULL,&error);
    if(!stream){out->writable=FALSE;g_free(out->warning);out->warning=g_strdup_printf("Cannot read saved session: %s",error->message);g_clear_error(&error);goto done;}
    GByteArray *bytes=g_byte_array_new();guint8 buffer[16384];gssize n;
    while((n=g_input_stream_read(G_INPUT_STREAM(stream),buffer,MIN(sizeof buffer,FW_SESSION_LIMIT+1-bytes->len),NULL,&error))>0){g_byte_array_append(bytes,buffer,(guint)n);if(bytes->len>FW_SESSION_LIMIT)break;}
    g_object_unref(stream);
    if(error){
        out->writable=FALSE;g_free(out->warning);out->warning=g_strdup_printf("Could not finish reading the session; the file was preserved: %s",error->message);
        g_clear_error(&error);g_byte_array_unref(bytes);goto done;
    }
    out->session=fw_session_decode((const char *)bytes->data,bytes->len,&error);g_byte_array_unref(bytes);
    if(!out->session){
        if(out->writable){
            char *uuid=g_uuid_string_random(),*backup=g_strdup_printf("%s.invalid-%s",store->path,uuid);g_free(uuid);GFile *target=g_file_new_for_path(backup);GError *move=NULL;
            if(g_file_move(file,target,G_FILE_COPY_NONE,NULL,NULL,NULL,&move)){g_free(out->warning);out->warning=g_strdup_printf("Invalid saved session was kept at %s. Using default folders.",backup);}
            else{out->writable=FALSE;g_free(out->warning);out->warning=g_strdup_printf("Invalid saved session was preserved; saving disabled: %s",move->message);g_error_free(move);}
            g_object_unref(target);g_free(backup);
        }else if(!out->warning)out->warning=g_strdup("Invalid saved session was preserved. Using default folders.");
        g_clear_error(&error);
    }
done:
    if(!out->writable&&store->lock>=0){close(store->lock);store->lock=-1;}
    g_object_unref(file);g_task_return_pointer(task,out,(GDestroyNotify)fw_session_load_free);
}
void fw_session_load_async(FwSessionStore *store,GAsyncReadyCallback callback,gpointer data)
{ GTask *task=g_task_new(store,NULL,callback,data);g_task_run_in_thread(task,load_worker);g_object_unref(task); }
FwSessionLoad *fw_session_load_finish(FwSessionStore *store,GAsyncResult *result,GError **error)
{ g_return_val_if_fail(g_task_is_valid(result,store),NULL);return g_task_propagate_pointer(G_TASK(result),error); }
static void save_worker(GTask *task,gpointer object,gpointer data,GCancellable *cancel)
{
    (void)cancel;FwSessionStore *store=object;GBytes *bytes=data;gsize n;const char *text=g_bytes_get_data(bytes,&n);GError *error=NULL;
    if(store->lock<0){g_task_return_new_error(task,G_IO_ERROR,G_IO_ERROR_BUSY,"This window does not own the saved session.");return;}
    if(!g_file_set_contents_full(store->path,text,n,G_FILE_SET_CONTENTS_CONSISTENT|G_FILE_SET_CONTENTS_DURABLE,0600,&error))g_task_return_error(task,error);else g_task_return_boolean(task,TRUE);
}
void fw_session_save_async(FwSessionStore *store,const FwSession *session,GAsyncReadyCallback callback,gpointer data)
{
    GTask *task=g_task_new(store,NULL,callback,data);GError *error=NULL;gsize n=0;char *text=fw_session_encode(session,&n,&error);
    if(!text){g_task_return_error(task,error);g_object_unref(task);return;}
    g_task_set_task_data(task,g_bytes_new_take(text,n),(GDestroyNotify)g_bytes_unref);g_task_run_in_thread(task,save_worker);g_object_unref(task);
}
gboolean fw_session_save_finish(FwSessionStore *store,GAsyncResult *result,GError **error)
{ g_return_val_if_fail(g_task_is_valid(result,store),FALSE);return g_task_propagate_boolean(G_TASK(result),error); }
