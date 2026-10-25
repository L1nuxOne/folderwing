/* relay.h - menu items of your own: [relay.NAME] sections in the settings.
 *
 * A relay is a function made by its settings section (fn_family), so keys,
 * menus, the palette and [functions] work for it as for any other. What it
 * does is one of:
 *   run     a command line (%1, %*, %D...: relaycmd.h), started here
 *   new     a file made in the folder (empty or a template), then renamed
 *   sendto  an entry of Windows' Send to
 *   item    an item of Explorer's menu, by its text ("7-Zip > Add to archive")
 *   verb    an Explorer verb ("runas", "print")
 * The last three run in folderwing-shell.exe, so shell extensions load there and
 * never in OpenPilot; the helper is the warm function shell.helper. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define WM_APP_RELAY (WM_APP + 8) /* to the main window: wParam generation, lParam a reply (or NULL: gone) */

void relay_register(void);
void relay_message(WPARAM w, LPARAM l);
