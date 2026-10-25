/* menu.h - popup menus drawn by OpenPilot.
 *
 * menu_track shows an ordinary popup HMENU (strings with "\tkey" hints,
 * separators, MF_GRAYED, MF_CHECKED, MFS_DEFAULT, submenus) the way the
 * rest of the window is drawn, instead of Windows' own menu. Callers build
 * menus as they always did, so the native one stays one call away.
 * It is modal like TrackPopupMenu: the explorer keeps painting and
 * receiving its own messages meanwhile, but mouse and keys go to the menu. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

/* The id picked, or 0 (cancelled). pt: screen point the menu opens at. */
UINT menu_track(HMENU m, HWND owner, POINT pt);
