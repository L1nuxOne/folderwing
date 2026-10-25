/* sidepanel.h - the side panel: a third surface that slides in at the right
 * edge and shows a panel app (a separate program, app/panelproto.h) while
 * the panes stay as they are. Several apps can be alive at once: the one
 * shown, and those that asked to keep running (SIDEPANEL_KEEP) while
 * another is shown or the panel is closed. The header has a tab for each. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

#define SIDEPANEL_KEEP 0x1u /* keeps running when another app is shown or the panel closes */

typedef struct sidepanel_config {
    int   slide;                       /* slide in (~100 ms) rather than appear */
    void (*relayout)(void);            /* the panel's width changed: lay the window out again */
    void (*closed)(int had_focus);     /* it closed (also from its own close button) */
    void (*ready)(const wchar_t *app); /* an app is up (also after Restart): send it what it needs */
    void (*message)(const wchar_t *app, uint32_t type, const void *data, uint32_t size); /* an app's own requests */
    void (*focus)(HWND target);        /* host focus policy (may remember it while another app is active) */
    int  (*key)(UINT vk);              /* a key the panel has: 1 = the host used it (the panel's own toggles) */
} sidepanel_config;

void sidepanel_configure(const sidepanel_config *cfg); /* any time; the slide setting applies on the next open */
/* Shows app (starting it unless it runs). toggle closes the panel instead when app is what it shows:
 * 0 = now shows app, 1 = closed, -1 = failed. */
int  sidepanel_toggle(HWND parent, const wchar_t *app, unsigned flags);
int  sidepanel_show(HWND parent, const wchar_t *app, unsigned flags, int focus);
void sidepanel_close(void);
int  sidepanel_open(void);
const wchar_t *sidepanel_app(void);         /* the app shown, or NULL */
int  sidepanel_running(const wchar_t *app); /* alive, shown or not */
uint32_t sidepanel_generation(const wchar_t *app); /* 0 if absent; identifies this launch, including while hidden */
int  sidepanel_has_focus(void);
/* To app (shown or not; NULL: the one shown). Queued; never waits for the app. */
int  sidepanel_send(const wchar_t *app, uint32_t type, const void *data, uint32_t size);
int  sidepanel_width(int avail);     /* what the panes give up, given the width they have; 0 when closed */
void sidepanel_place(int x, int y, int w, int h); /* from the window's layout */
void sidepanel_focus(void);
void sidepanel_shutdown(void);       /* cancels launches and queues app cleanup without waiting */
HWND sidepanel_window(void);

/* Retire only this owned panel, e.g. when its feature is disabled. */
void sidepanel_stop_app(const wchar_t *app);
