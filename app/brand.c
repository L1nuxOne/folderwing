/* Product presentation only. Persistence paths, protocols and class names remain stable. */
#include "brand.h"
#include <shobjidl.h>
HICON fw_brand_icon(HINSTANCE instance,int size){
 HICON icon=(HICON)LoadImageW(instance,MAKEINTRESOURCEW(FW_ICON_ID),IMAGE_ICON,size,size,LR_SHARED);
 return icon?icon:LoadIconW(NULL,IDI_APPLICATION);
}
void fw_brand_window(HWND window,UINT dpi){
 HINSTANCE instance=(HINSTANCE)GetWindowLongPtrW(window,GWLP_HINSTANCE);
 SendMessageW(window,WM_SETICON,ICON_BIG,(LPARAM)fw_brand_icon(instance,GetSystemMetricsForDpi(SM_CXICON,dpi)));
 SendMessageW(window,WM_SETICON,ICON_SMALL,(LPARAM)fw_brand_icon(instance,GetSystemMetricsForDpi(SM_CXSMICON,dpi)));
}
HRESULT fw_brand_process(void){return SetCurrentProcessExplicitAppUserModelID(FW_APP_ID);}
int fw_brand_about(HWND owner){
 MSGBOXPARAMSW box={sizeof box};box.hwndOwner=owner;box.hInstance=GetModuleHandleW(NULL);box.lpszCaption=L"About Folderwing";box.dwStyle=MB_OK|MB_USERICON;box.lpszIcon=MAKEINTRESOURCEW(FW_ICON_ID);
 box.lpszText=L"Folderwing\nFile manager for Windows and WSL\n\nVersion " FW_VERSION_W L" — private development build\n\nNative browsing, previews, search and workspace tools.\n\nMIT-licensed core. Helper dependency notices accompany the app.";
 return MessageBoxIndirectW(&box);
}
