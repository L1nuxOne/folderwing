#include "clipboard.h"
#include <string.h>
void fw_clipboard_data_free(FwClipboardData *data)
{ if(data){g_strfreev(data->paths);g_free(data);} }
static FwClipboardData *invalid(GError **error,const char *message)
{ g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA,message);return NULL; }
FwClipboardData *fw_clipboard_parse(const char *mime,const void *bytes,gsize size,const void *kde,gsize kde_size,GError **error)
{
    if(!mime||!bytes||!size||size>FW_CLIPBOARD_LIMIT||memchr(bytes,0,size)||!g_utf8_validate(bytes,size,NULL))return invalid(error,"The clipboard does not contain a bounded UTF-8 file list.");
    gboolean tagged=!strcmp(mime,FW_CLIPBOARD_GNOME)||!strcmp(mime,FW_CLIPBOARD_NAUTILUS);
    if(!tagged&&strcmp(mime,FW_CLIPBOARD_URIS))return invalid(error,"The clipboard does not offer supported files.");
    if(kde_size&&(kde_size!=1||!kde||(*(const char *)kde!='0'&&*(const char *)kde!='1')))return invalid(error,"Invalid clipboard cut marker.");
    char *text=g_strndup(bytes,size);char **lines=g_strsplit(text,"\n",-1);g_free(text);
    GPtrArray *paths=g_ptr_array_new_with_free_func(g_free);GHashTable *seen=g_hash_table_new(g_str_hash,g_str_equal);
    guint start=0;gboolean cut=kde_size&&*(const char *)kde=='1';const char *reason=NULL;
    for(guint i=0;lines[i];i++){gsize n=strlen(lines[i]);if(n&&lines[i][n-1]=='\r')lines[i][n-1]=0;}
    if(tagged){
        if(strcmp(lines[0],"copy")&&strcmp(lines[0],"cut")){reason="File clipboard must begin with copy or cut.";goto done;}
        cut=!strcmp(lines[0],"cut");start=1;
        if(kde_size&&cut!=(*(const char *)kde=='1')){reason="Conflicting clipboard cut markers.";goto done;}
    }
    for(guint i=start;lines[i];i++){
        const char *uri=lines[i];if(!*uri||(!tagged&&*uri=='#'))continue;
        if(paths->len>=4096){reason="Paste supports at most 4096 files.";goto done;}
        if(strlen(uri)>3u*16384u+32u||strpbrk(uri,"\r\t ?#")){reason="Invalid local file URI in clipboard.";goto done;}
        char *host=NULL;GError *decode=NULL;char *path=g_filename_from_uri(uri,&host,&decode);
        if(!path||!g_path_is_absolute(path)||strlen(path)>16384||(host&&*host&&g_ascii_strcasecmp(host,"localhost"))){
            g_free(path);g_free(host);g_clear_error(&decode);reason="Only local file URIs with paths up to 16 KiB can be pasted.";goto done;
        }
        g_free(host);g_clear_error(&decode);
        if(g_hash_table_contains(seen,path)){g_free(path);reason="The clipboard lists the same source more than once.";goto done;}
        g_hash_table_add(seen,path);g_ptr_array_add(paths,path);
    }
    if(!paths->len)reason="The clipboard contains no files.";
done:
    g_hash_table_unref(seen);g_strfreev(lines);
    if(reason){g_ptr_array_unref(paths);return invalid(error,reason);}
    FwClipboardData *out=g_new0(FwClipboardData,1);out->count=paths->len;out->cut=cut;g_ptr_array_add(paths,NULL);out->paths=(char **)g_ptr_array_free(paths,FALSE);return out;
}
static GdkContentProvider *bytes_provider(const char *mime,const char *text,gsize size)
{
    GBytes *bytes=g_bytes_new(text,size);GdkContentProvider *provider=gdk_content_provider_new_for_bytes(mime,bytes);g_bytes_unref(bytes);return provider;
}
GdkContentProvider *fw_clipboard_provider(const char *const *paths,guint count,gboolean cut,GError **error)
{
    if(!paths||!count||count>4096){invalid(error,"Choose 1–4096 files to copy or cut.");return NULL;}
    GString *uris=g_string_new(NULL),*tagged=g_string_new(cut?"cut":"copy");
    for(guint i=0;i<count;i++){
        if(!paths[i]||!g_path_is_absolute(paths[i])||strlen(paths[i])>16384)goto bad;
        char *uri=g_filename_to_uri(paths[i],NULL,NULL);if(!uri)goto bad;
        g_string_append_printf(uris,"%s\r\n",uri);g_string_append_c(tagged,'\n');g_string_append(tagged,uri);g_free(uri);
        if(uris->len>FW_CLIPBOARD_LIMIT||tagged->len>FW_CLIPBOARD_LIMIT)goto bad;
    }
    GdkContentProvider *providers[]={bytes_provider(FW_CLIPBOARD_GNOME,tagged->str,tagged->len),bytes_provider(FW_CLIPBOARD_NAUTILUS,tagged->str,tagged->len),bytes_provider(FW_CLIPBOARD_URIS,uris->str,uris->len),bytes_provider(FW_CLIPBOARD_KDE,cut?"1":"0",1),gdk_content_provider_new_typed(G_TYPE_STRING,uris->str)};
    GdkContentProvider *result=gdk_content_provider_new_union(providers,G_N_ELEMENTS(providers));g_string_free(uris,TRUE);g_string_free(tagged,TRUE);return result;
bad:
    g_string_free(uris,TRUE);g_string_free(tagged,TRUE);invalid(error,"Clipboard files exceed the native path or 16 MiB file-list limit.");return NULL;
}
gboolean fw_clipboard_has_files(GdkClipboard *clipboard)
{
    GdkContentFormats *formats=gdk_clipboard_get_formats(clipboard);
    return gdk_content_formats_contain_mime_type(formats,FW_CLIPBOARD_GNOME)||gdk_content_formats_contain_mime_type(formats,FW_CLIPBOARD_NAUTILUS)||gdk_content_formats_contain_mime_type(formats,FW_CLIPBOARD_URIS);
}
typedef struct Read {
    GdkClipboard *clipboard;GCancellable *cancel,*external;gulong changed,external_cancel;
    GInputStream *stream;GByteArray *bytes;char *mime;GBytes *files;guint timer;gboolean marker,offer_changed,timed_out;
} Read;
static void read_changed(GdkClipboard *clipboard,gpointer data)
{ (void)clipboard;Read *r=data;r->offer_changed=TRUE;g_cancellable_cancel(r->cancel); }
static void external_cancelled(GCancellable *cancel,gpointer data) { (void)cancel;g_cancellable_cancel(data); }
static gboolean read_timeout(gpointer data) { Read *r=data;r->timer=0;r->timed_out=TRUE;g_cancellable_cancel(r->cancel);return G_SOURCE_REMOVE; }
static void read_free(gpointer data)
{
    Read *r=data;if(r->timer)g_source_remove(r->timer);
    if(r->changed)g_signal_handler_disconnect(r->clipboard,r->changed);
    if(r->external_cancel)g_cancellable_disconnect(r->external,r->external_cancel);
    g_clear_object(&r->external);g_clear_object(&r->cancel);g_clear_object(&r->stream);g_clear_object(&r->clipboard);g_clear_pointer(&r->bytes,g_byte_array_unref);g_clear_pointer(&r->files,g_bytes_unref);g_free(r->mime);g_free(r);
}
static void read_fail(GTask *task,GError *error)
{
    Read *r=g_task_get_task_data(task);
    if(r->offer_changed){g_clear_error(&error);error=g_error_new_literal(G_IO_ERROR,G_IO_ERROR_CANCELLED,"Clipboard changed; paste again.");}
    else if(r->timed_out){g_clear_error(&error);error=g_error_new_literal(G_IO_ERROR,G_IO_ERROR_TIMED_OUT,"Clipboard did not respond within 15 seconds.");}
    if(!error)error=g_error_new_literal(G_IO_ERROR,G_IO_ERROR_INVALID_DATA,"Invalid clipboard file list.");
    g_task_return_error(task,error);g_object_unref(task);
}
static void read_opened(GObject *source,GAsyncResult *result,gpointer data);
static void read_chunk(GObject *source,GAsyncResult *result,gpointer data)
{
    GTask *task=data;Read *r=g_task_get_task_data(task);GError *error=NULL;GBytes *bytes=g_input_stream_read_bytes_finish(G_INPUT_STREAM(source),result,&error);
    if(!bytes){read_fail(task,error);return;}
    gsize n;const guint8 *p=g_bytes_get_data(bytes,&n);gsize limit=r->marker?1:FW_CLIPBOARD_LIMIT;
    if(n>limit-r->bytes->len){g_bytes_unref(bytes);read_fail(task,g_error_new_literal(G_IO_ERROR,G_IO_ERROR_INVALID_DATA,"Clipboard file list or cut marker exceeds its size limit."));return;}
    g_byte_array_append(r->bytes,p,n);g_bytes_unref(bytes);
    if(n){g_input_stream_read_bytes_async(r->stream,16384,G_PRIORITY_DEFAULT,r->cancel,read_chunk,task);return;}
    g_clear_object(&r->stream);
    if(!r->marker){
        r->files=g_bytes_new(r->bytes->data,r->bytes->len);
        if(gdk_content_formats_contain_mime_type(gdk_clipboard_get_formats(r->clipboard),FW_CLIPBOARD_KDE)){
            r->marker=TRUE;g_byte_array_set_size(r->bytes,0);static const char *mimes[]={FW_CLIPBOARD_KDE,NULL};gdk_clipboard_read_async(r->clipboard,mimes,G_PRIORITY_DEFAULT,r->cancel,read_opened,task);return;
        }
    }
    if(r->offer_changed||g_cancellable_is_cancelled(r->cancel)){read_fail(task,NULL);return;}
    gsize size;const void *files=g_bytes_get_data(r->files,&size);
    FwClipboardData *out=fw_clipboard_parse(r->mime,files,size,r->marker?r->bytes->data:NULL,r->marker?r->bytes->len:0,&error);
    if(!out){read_fail(task,error);return;}
    g_task_return_pointer(task,out,(GDestroyNotify)fw_clipboard_data_free);g_object_unref(task);
}
static void read_opened(GObject *source,GAsyncResult *result,gpointer data)
{
    GTask *task=data;Read *r=g_task_get_task_data(task);GError *error=NULL;const char *mime=NULL;
    r->stream=gdk_clipboard_read_finish(GDK_CLIPBOARD(source),result,&mime,&error);
    if(!r->stream){read_fail(task,error);return;}if(!r->marker)r->mime=g_strdup(mime);
    g_input_stream_read_bytes_async(r->stream,16384,G_PRIORITY_DEFAULT,r->cancel,read_chunk,task);
}
void fw_clipboard_read_async(GdkClipboard *clipboard,GCancellable *cancel,GAsyncReadyCallback callback,gpointer data)
{
    GTask *task=g_task_new(clipboard,cancel,callback,data);Read *r=g_new0(Read,1);g_task_set_task_data(task,r,read_free);
    r->clipboard=g_object_ref(clipboard);r->cancel=g_cancellable_new();r->bytes=g_byte_array_new();r->changed=g_signal_connect(clipboard,"changed",G_CALLBACK(read_changed),r);r->timer=g_timeout_add_seconds(15,read_timeout,r);
    if(cancel){r->external=g_object_ref(cancel);r->external_cancel=g_cancellable_connect(cancel,G_CALLBACK(external_cancelled),g_object_ref(r->cancel),g_object_unref);}
    static const char *mimes[]={FW_CLIPBOARD_GNOME,FW_CLIPBOARD_NAUTILUS,FW_CLIPBOARD_URIS,NULL};gdk_clipboard_read_async(clipboard,mimes,G_PRIORITY_DEFAULT,r->cancel,read_opened,task);
}
FwClipboardData *fw_clipboard_read_finish(GAsyncResult *result,GError **error)
{ return g_task_propagate_pointer(G_TASK(result),error); }
