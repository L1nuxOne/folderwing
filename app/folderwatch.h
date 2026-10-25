#pragma once
#include "../include/fpe.h"

/* UI-thread ownership for the host's two folder watches. Notifications run on
 * the UI thread and only for the current watch. The engine's blocking stop
 * contract is preserved; a separate worker owns retired watches until joined. */
typedef void (*folderwatch_notify)(void *user);
fpe_err folderwatch_set(unsigned pane, const wchar_t *path, folderwatch_notify notify, void *user);
/* Clear one pane immediately; retire its owner off-thread. Does not start a
 * manager or wait for filesystem IO. Invalid pane numbers remain errors. */
fpe_err folderwatch_clear(unsigned pane);
/* Invalidates notifications and queues cleanup, without joining a worker. */
void folderwatch_shutdown(void);
/* Call after the UI message loop exits, before engine teardown. May block. */
void folderwatch_join(void);
