#pragma once
#define FW_NAME "Folderwing"
#define FW_NAME_W L"Folderwing"
#define FW_DESCRIPTION "File manager for Windows and WSL"
#define FW_VERSION "0.2.0"
#define FW_VERSION_W L"0.2.0"
#define FW_ICON_ID 101
#define FW_APP_ID L"Folderwing.FileManager"
#ifndef RC_INVOKED
#include <windows.h>
HICON fw_brand_icon(HINSTANCE instance,int size);
void fw_brand_window(HWND window,UINT dpi);
HRESULT fw_brand_process(void);
int fw_brand_about(HWND owner);
#endif
