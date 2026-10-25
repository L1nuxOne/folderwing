/* Registered distro identities only: no UNC probe, process or Linux startup. */
#pragma once
#include <windows.h>
#include <wchar.h>
#include <wctype.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#define WSL_TREE_LIMIT 64u
typedef struct wsl_tree_distro {wchar_t name[64],root[96];DWORD version;} wsl_tree_distro;
static int wsl_tree_name_safe(const wchar_t *name){size_t n=wcslen(name);if(!n||n>=64||!wcscmp(name,L".")||!wcscmp(name,L".."))return 0;for(size_t i=0;i<n;i++)if(!iswalnum(name[i])&&!wcschr(L"._-",name[i]))return 0;return 1;}
static int wsl_tree_compare(const void *a,const void *b){return _wcsicmp(((const wsl_tree_distro*)a)->name,((const wsl_tree_distro*)b)->name);}
static uint32_t wsl_tree_discover(wsl_tree_distro *out,uint32_t capacity){
 HKEY registrations;if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss",0,KEY_READ,&registrations)!=ERROR_SUCCESS)return 0;
 uint32_t count=0;for(DWORD i=0;count<capacity;i++){
  wchar_t subkey[128];DWORD length=128;LONG error=RegEnumKeyExW(registrations,i,subkey,&length,NULL,NULL,NULL,NULL);if(error==ERROR_NO_MORE_ITEMS)break;if(error!=ERROR_SUCCESS)break;
  HKEY key;if(RegOpenKeyExW(registrations,subkey,0,KEY_QUERY_VALUE,&key)!=ERROR_SUCCESS)continue;wchar_t name[64]={0};DWORD version=0,type=0,size=sizeof version;
  LONG ver=RegQueryValueExW(key,L"Version",NULL,&type,(BYTE*)&version,&size);int supported=ver==ERROR_SUCCESS&&type==REG_DWORD&&size==sizeof version&&(version==1||version==2);
  size=sizeof name;error=RegQueryValueExW(key,L"DistributionName",NULL,&type,(BYTE*)name,&size);RegCloseKey(key);
  if(!supported||error!=ERROR_SUCCESS||type!=REG_SZ||size<2||size>sizeof name||size%2||name[size/2-1]||wcslen(name)!=size/2-1||!wsl_tree_name_safe(name))continue;
  int duplicate=0;for(uint32_t j=0;j<count;j++)if(!_wcsicmp(out[j].name,name))duplicate=1;if(duplicate)continue;
  wsl_tree_distro *d=&out[count++];memset(d,0,sizeof *d);wcscpy(d->name,name);d->version=version;swprintf(d->root,96,L"\\\\wsl.localhost\\%ls\\",name);
 }
 RegCloseKey(registrations);qsort(out,count,sizeof *out,wsl_tree_compare);return count;
}
