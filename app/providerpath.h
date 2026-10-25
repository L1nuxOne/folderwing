/* Admission to Windows provider I/O. No distro discovery or native helper load.
 * The private Linux quote is an identity transport, not a Windows filename.
 * Ordinary Windows names containing that literal code point remain legal. */
#ifndef OPENPILOT_PROVIDERPATH_H
#define OPENPILOT_PROVIDERPATH_H
#include <windows.h>
#include <winnetwk.h>
#include <wchar.h>
#include "../src/namecodec.h"
static inline int provider_wsl_prefix(const wchar_t *p){
 if(!_wcsnicmp(p,L"\\\\wsl.localhost\\",16))return 16;
 if(!_wcsnicmp(p,L"\\\\wsl$\\",7))return 7;
 if(!_wcsnicmp(p,L"\\\\?\\UNC\\wsl.localhost\\",22))return 22;
 if(!_wcsnicmp(p,L"\\\\?\\UNC\\wsl$\\",13))return 13;
 return 0;
}
static inline DWORD provider_path_check(const wchar_t *path,int wsl_enabled){
 if(!path||!*path)return ERROR_INVALID_NAME;
 const wchar_t *namespace_path=path;wchar_t remote[32768];
 int quote=wcschr(path,FPE_LX_ESCAPE)!=NULL;
 if(path[1]==L':'){
  wchar_t drive[4]={path[0],L':',L'\\',0},local[3]={path[0],L':',0};
  if(GetDriveTypeW(drive)==DRIVE_REMOTE){DWORD size=32768;
   if(WNetGetConnectionW(local,remote,&size)==NO_ERROR){namespace_path=remote;quote|=wcschr(remote,FPE_LX_ESCAPE)!=NULL;}
   else return ERROR_NOT_SUPPORTED; /* An unresolved remote identity is not admitted. */
  }
 }
 return provider_wsl_prefix(namespace_path)&&(!wsl_enabled||quote)?ERROR_NOT_SUPPORTED:0;
}
#endif
