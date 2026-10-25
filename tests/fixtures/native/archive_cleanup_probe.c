/* Test-only API interposition around the actual archive client. Production
 * binaries contain no gate, process argument or fault-injection path. */
#include "../../../app/archiveclient.h"
#include "../../../app/previewlaunch.h"
#include <wchar.h>
static HANDLE delete_entered,delete_allowed,worker_entered,worker_allowed;
static wchar_t gated_path[ARCP_NAME];static volatile LONG pause_worker;
static void *fail_owner;
static BOOL WINAPI observed_delete(LPCWSTR path){if(delete_entered&&!wcscmp(path,gated_path)){SetEvent(delete_entered);WaitForSingleObject(delete_allowed,INFINITE);}return DeleteFileW(path);}
static void observed_cancel(preview_launch **slot){if(InterlockedExchange(&pause_worker,0)){SetEvent(worker_entered);WaitForSingleObject(worker_allowed,INFINITE);}preview_launch_cancel(slot);}
static BOOL WINAPI observed_queue(LPTHREAD_START_ROUTINE callback,void *arg,ULONG flags){if(arg==InterlockedCompareExchangePointer(&fail_owner,NULL,arg)){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return FALSE;}return QueueUserWorkItem(callback,arg,flags);}
#define DeleteFileW observed_delete
#define preview_launch_cancel observed_cancel
#define QueueUserWorkItem observed_queue
#include "../../../app/archiveclient.c"
#undef DeleteFileW
#undef preview_launch_cancel
#undef QueueUserWorkItem
void archive_test_cleanup_gate(const wchar_t *path,HANDLE entered,HANDLE allowed,HANDLE producer_entered,HANDLE producer_allowed){wcscpy(gated_path,path);delete_entered=entered;delete_allowed=allowed;worker_entered=producer_entered;worker_allowed=producer_allowed;InterlockedExchange(&pause_worker,producer_entered?1:0);}
void archive_test_cleanup_fail(arc_file *file){InterlockedExchangePointer(&fail_owner,file);}
arc_file *archive_test_cleanup_source(void){HANDLE h;arc_file *f=new_file(L"probe.txt",&h);if(!f)return NULL;DWORD n;BOOL ok=WriteFile(h,"owned cleanup probe",19,&n,NULL)&&n==19;CloseHandle(h);if(!ok){arc_file_release(f);return NULL;}return f;}
int archive_test_ready_source(arc_client *c,wchar_t path[ARCP_NAME],wchar_t directory[ARCP_NAME]){int found=0;EnterCriticalSection(&c->lock);if(c->ready&&c->ready->file){wcscpy(path,c->ready->file->path);wcscpy(directory,c->ready->file->directory);found=1;}LeaveCriticalSection(&c->lock);return found;}
