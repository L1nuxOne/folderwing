/* UI-owned lifecycle for an inert OLE target on a private pumping STA.
 * Actual application drop targets and pane callbacks remain on the UI thread. */
#pragma once
#include <windows.h>
enum { DROPWARM_NONE, DROPWARM_READY, DROPWARM_DONE, DROPWARM_FAILED };
int dropwarm_start(void); /* 0 = started; -1 = no resources retained */
int dropwarm_stop(void);  /* 0 = absent; 1 = cancellation pending */
HANDLE dropwarm_wait_handle(void); /* ready event, then thread exit; never wait on UI */
int dropwarm_collect(HRESULT *error); /* UI thread, nonblocking; DONE/FAILED releases all handles */
