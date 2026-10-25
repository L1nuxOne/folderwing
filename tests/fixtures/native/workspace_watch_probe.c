/* Private observability around the actual watch owner/reaper, not product API. */
#include "../../../app/folderwatch.h"
#include <stdint.h>
static void observed_stop(fpe_watch *);
#define fpe_watch_stop observed_stop
#define folderwatch_set real_folderwatch_set
#include "../../../app/folderwatch.c"
#undef fpe_watch_stop
#undef folderwatch_set
static struct {folderwatch_notify notify;void *user;unsigned calls;} notices[2];
static fpe_watch *paused_watch;static HANDLE stop_entered,stop_allowed;static volatile LONG paused_stops;
static void counted_notice(void *arg){unsigned pane=(unsigned)(uintptr_t)arg;notices[pane].calls++;notices[pane].notify(notices[pane].user);}
fpe_err folderwatch_set(unsigned pane,const wchar_t *path,folderwatch_notify notify,void *user){if(pane>=2||!path||!notify)return real_folderwatch_set(pane,path,notify,user);notices[pane].notify=notify;notices[pane].user=user;return real_folderwatch_set(pane,path,counted_notice,(void*)(uintptr_t)pane);}
static void observed_stop(fpe_watch *watch){if(watch==paused_watch){InterlockedIncrement(&paused_stops);SetEvent(stop_entered);WaitForSingleObject(stop_allowed,INFINITE);}fpe_watch_stop(watch);}
int workspace_test_watch_active(unsigned pane){return pane<2&&F.active[pane]!=NULL;}
UINT_PTR workspace_test_watch_generation(unsigned pane){return pane<2&&F.active[pane]?F.active[pane]->generation:0;}
unsigned workspace_test_watch_notices(unsigned pane){return pane<2?notices[pane].calls:0;}
int workspace_test_watch_post(unsigned pane,UINT_PTR token){return F.sink&&PostMessageW(F.sink,FOLDERWATCH_CHANGED,pane,(LPARAM)token);}
void workspace_test_watch_stop_gate(unsigned pane,HANDLE entered,HANDLE allowed){paused_watch=pane<2&&F.active[pane]?F.active[pane]->watch:NULL;stop_entered=entered;stop_allowed=allowed;InterlockedExchange(&paused_stops,0);}
LONG workspace_test_watch_stop_count(void){return InterlockedCompareExchange(&paused_stops,0,0);}
void workspace_test_watch_stop_gate_clear(void){paused_watch=NULL;stop_entered=stop_allowed=NULL;}
