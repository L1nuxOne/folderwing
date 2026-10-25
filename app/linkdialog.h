#pragma once
#include "fn.h"
/* A Linux filename, mapped to the UNC representation used by the engine.
 * Rejects dot entries, slashes and names exceeding Linux's 255-byte limit. */
int linkdialog_name(const wchar_t *raw,wchar_t mapped[260]);
/* Modal draft only: no filesystem mutation. 1=Create, 0=Cancel/failure.
 * name is edited for a single item; multiple items keep their source names. */
int linkdialog_choose(HWND owner,HFONT font,const wchar_t *source,const wchar_t *destination,
                      uint32_t count,int *relative,wchar_t name[260]);
