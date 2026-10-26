#define _XOPEN_SOURCE 700
#include "desktop.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>
typedef struct Query { char *path; gboolean directory,applications; } Query;
static void query_free(gpointer data) { Query *q=data;g_free(q->path);g_free(q); }
void fw_desktop_target_free(FwDesktopTarget *t)
{
    if(!t)return;
    g_free(t->path);g_free(t->uri);g_free(t->content_type);
    g_list_free_full(t->applications,g_object_unref);g_free(t);
}
static void query_worker(GTask *task,gpointer object,gpointer data,GCancellable *cancel)
{
    (void)object;Query *q=data;GError *error=NULL;
    if(!g_path_is_absolute(q->path)||strlen(q->path)>16384){g_task_return_new_error(task,G_IO_ERROR,G_IO_ERROR_INVALID_ARGUMENT,"Expected an absolute Linux path of at most 16 KiB.");return;}
    /* GFile canonicalizes '..' lexically. Let the kernel resolve it first so
       a displayed symlink/../name never opens a different file. */
    char *resolved=realpath(q->path,NULL);
    if(!resolved){int e=errno;g_task_return_new_error(task,G_IO_ERROR,g_io_error_from_errno(e),"Cannot open this location: %s",g_strerror(e));return;}
    GFile *file=g_file_new_for_path(resolved);free(resolved);
    GFileInfo *info=g_file_query_info(file,G_FILE_ATTRIBUTE_STANDARD_TYPE,G_FILE_QUERY_INFO_NONE,cancel,&error);
    if(!info){g_object_unref(file);g_task_return_error(task,error);return;}
    GFileType type=g_file_info_get_file_type(info);
    if(type!=(q->directory?G_FILE_TYPE_DIRECTORY:G_FILE_TYPE_REGULAR)){
        g_object_unref(info);g_object_unref(file);
        g_task_return_new_error(task,G_IO_ERROR,G_IO_ERROR_NOT_SUPPORTED,q->directory?"This location is no longer a folder.":"Only regular files can be opened in another application.");return;
    }
    if(!q->directory){
        g_object_unref(info);info=g_file_query_info(file,G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,G_FILE_QUERY_INFO_NONE,cancel,&error);
        if(!info){g_object_unref(file);g_task_return_error(task,error);return;}
    }
    FwDesktopTarget *t=g_new0(FwDesktopTarget,1);t->path=g_strdup(q->path);t->uri=g_file_get_uri(file);
    const char *content=q->directory?"inode/directory":g_file_info_get_content_type(info);t->content_type=g_strdup(content?content:"application/octet-stream");
    if(q->applications){
        t->applications=g_app_info_get_all_for_type(t->content_type);
        GHashTable *seen=g_hash_table_new(g_str_hash,g_str_equal);
        for(GList *at=t->applications;at;at=at->next){const char *id=g_app_info_get_id(at->data);if(id)g_hash_table_add(seen,(gpointer)id);}
        GList *all=g_app_info_get_all(),*extra=NULL;
        for(GList *at=all;at;at=at->next){GAppInfo *app=at->data;const char *id=g_app_info_get_id(app);
            if(id&&!g_hash_table_contains(seen,id)){extra=g_list_prepend(extra,g_object_ref(app));g_hash_table_add(seen,(gpointer)id);}
        }
        t->applications=g_list_concat(t->applications,g_list_reverse(extra));g_hash_table_unref(seen);g_list_free_full(all,g_object_unref);
    }
    g_object_unref(info);g_object_unref(file);
    g_task_return_pointer(task,t,(GDestroyNotify)fw_desktop_target_free);
}
void fw_desktop_query_async(const char *path,gboolean directory,gboolean applications,
    GCancellable *cancel,GAsyncReadyCallback callback,gpointer data)
{
    GTask *task=g_task_new(NULL,cancel,callback,data);Query *q=g_new0(Query,1);
    q->path=g_strdup(path);q->directory=directory;q->applications=applications;
    g_task_set_task_data(task,q,query_free);g_task_run_in_thread(task,query_worker);g_object_unref(task);
}
FwDesktopTarget *fw_desktop_query_finish(GAsyncResult *result,GError **error)
{ return g_task_propagate_pointer(G_TASK(result),error); }
GAppInfo *fw_desktop_terminal_app(const char *helper,const char *path,GError **error)
{
    if(!g_path_is_absolute(helper)||!g_path_is_absolute(path)||strlen(path)>16384){
        g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_INVALID_ARGUMENT,"Terminal needs absolute helper and folder paths.");return NULL;
    }
    /* --shell already accepts a hex-encoded native path as argv. Percent signs
       in the helper filename must also survive desktop Exec field expansion. */
    GString *escaped=g_string_new(NULL),*encoded=g_string_new("x");
    for(const char *p=helper;*p;p++){g_string_append_c(escaped,*p);if(*p=='%')g_string_append_c(escaped,'%');}
    char *quoted=g_shell_quote(escaped->str);
    for(const unsigned char *p=(const unsigned char *)path;*p;p++)g_string_append_printf(encoded,"%02x",*p);
    char *command=g_strdup_printf("%s --shell %s",quoted,encoded->str);
    GAppInfo *app=g_app_info_create_from_commandline(command,"Folderwing Terminal",G_APP_INFO_CREATE_NEEDS_TERMINAL,error);
    g_free(command);g_free(quoted);g_string_free(escaped,TRUE);g_string_free(encoded,TRUE);return app;
}
