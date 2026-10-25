/* Local drive and registered WSL2 root identities. Discovery never starts Linux.
 * Capacity queries belong to the analyzer's background catalog worker. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
#include <stdint.h>
#define SPACE_VOLUME_LIMIT 64u
typedef struct space_volume {wchar_t root[512],label[160];uint64_t free,total;DWORD error;int wsl,known;} space_volume;
static int space_volume_name_safe(const wchar_t *s){if(!s||!*s||!wcscmp(s,L".")||!wcscmp(s,L"..")||wcslen(s)>127)return 0;for(;*s;s++)if(!((*s>=L'a'&&*s<=L'z')||(*s>=L'A'&&*s<=L'Z')||(*s>=L'0'&&*s<=L'9')||*s==L'-'||*s==L'_'||*s==L'.'))return 0;return 1;}
static uint32_t space_volume_discover(space_volume *out,uint32_t capacity,int wsl){uint32_t n=0;DWORD drives=GetLogicalDrives();for(unsigned i=0;i<26&&n<capacity;i++)if(drives&(1u<<i)){wchar_t root[]={L'A'+i,L':',L'\\',0};UINT kind=GetDriveTypeW(root);if(kind!=DRIVE_FIXED&&kind!=DRIVE_REMOVABLE&&kind!=DRIVE_CDROM&&kind!=DRIVE_RAMDISK)continue;space_volume *v=&out[n++];memset(v,0,sizeof *v);wcscpy(v->root,root);swprintf(v->label,160,L"%ls",root);}
 HKEY registrations;if(wsl&&RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss",0,KEY_READ,&registrations)==ERROR_SUCCESS){for(DWORD i=0;n<capacity;i++){wchar_t key_name[128];DWORD length=128;LONG e=RegEnumKeyExW(registrations,i,key_name,&length,NULL,NULL,NULL,NULL);if(e==ERROR_NO_MORE_ITEMS)break;if(e!=ERROR_SUCCESS)continue;HKEY key;if(RegOpenKeyExW(registrations,key_name,0,KEY_QUERY_VALUE,&key)!=ERROR_SUCCESS)continue;wchar_t name[128]={0};DWORD version=0,sz=sizeof version,type=0;LONG ver=RegQueryValueExW(key,L"Version",NULL,&type,(BYTE*)&version,&sz);int second=ver==ERROR_SUCCESS&&type==REG_DWORD&&sz==sizeof version&&version==2;sz=sizeof name;e=RegQueryValueExW(key,L"DistributionName",NULL,&type,(BYTE*)name,&sz);RegCloseKey(key);if(!second||e!=ERROR_SUCCESS||type!=REG_SZ||sz<2||sz>sizeof name||sz%2||name[sz/2-1]||!space_volume_name_safe(name))continue;space_volume *v=&out[n++];memset(v,0,sizeof *v);v->wsl=1;swprintf(v->root,512,L"\\\\wsl.localhost\\%ls\\",name);swprintf(v->label,160,L"%ls · WSL disk",name);}RegCloseKey(registrations);}return n;}
static inline int space_volume_allowed(const wchar_t *root,int wsl){space_volume list[SPACE_VOLUME_LIMIT];uint32_t count=space_volume_discover(list,SPACE_VOLUME_LIMIT,wsl);for(uint32_t i=0;i<count;i++)if(!_wcsicmp(root,list[i].root))return 1;return 0;}
