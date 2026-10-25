#pragma once
#include <windows.h>
#include "archiveutil.h"
typedef struct arc_client arc_client;
typedef struct arc_file {LONG refs;HANDLE anchor,lock;int directory_created,file_created;wchar_t path[ARCP_NAME],directory[ARCP_NAME];} arc_file;
enum {ARC_OPEN=1,ARC_FOLDER,ARC_PREVIEW,ARC_EXTRACT};
typedef struct arc_result {LONG generation;uint32_t action,error,backend;arc_row *rows;uint32_t count;arc_file *file;} arc_result;
arc_client *arc_client_start(HWND window,UINT message);
LONG arc_client_cancel(arc_client *client);
void arc_client_stop(arc_client **slot);
LONG arc_client_request(arc_client *client,uint32_t action,const wchar_t *archive,const wchar_t *folder,uint32_t index);
arc_result *arc_client_take(arc_client *client);
void arc_result_free(arc_result *result);
void arc_file_hold(arc_file *file);
void arc_file_release(void *file);

/* Final shutdown only: stop all clients and release engine/pinned owners first.
 * Waits outside the UI path for workers and owned cleanup attempts to finish.
 * Scheduling/deletion failures preserve data for review; no recursive cleanup. */
void arc_cleanup_drain(void);
