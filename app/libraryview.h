#pragma once
#include <windows.h>
typedef struct library_callbacks {void (*changed)(void);void (*action)(unsigned action,const wchar_t *path);void (*selected)(void);} library_callbacks;
int libraryview_start(HWND parent,const wchar_t *reference,const library_callbacks *callbacks);
void libraryview_stop(void);
HWND libraryview_window(void);
HWND libraryview_tree(void);
int libraryview_count(void);
int libraryview_height(void);
double libraryview_reader_ratio(void);
void libraryview_save_ratio(double ratio);
void libraryview_select(int index);
void libraryview_toggle(int index);
int libraryview_checked(int index);
int libraryview_checked_count(void);
void libraryview_activate(int active);
void libraryview_reader(int opened,const wchar_t *path);
void libraryview_place(int x,int y,int width,int height);
const wchar_t *libraryview_config(void);
