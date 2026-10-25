/* session.h - what the window remembers between runs: placement, layout,
 * each pane's tabs, the folders pinned to the tree and the folders visited
 * recently.
 *
 * INI with versioned exact UTF-16 path fields, written by the app on exit.
 * Legacy UTF-8 path fields remain readable. Parsing never fails: anything
 * missing or out of range keeps its default, so a damaged file costs at most
 * the layout, never startup. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#define SES_TABS_MAX 32
#define SES_PINS_MAX 64
#define SES_RECENT_MAX 50

typedef struct ses_tab {
    wchar_t *path;
    uint8_t  sort, desc; /* fpe_sort_key, descending */
} ses_tab;

typedef struct session {
    int      has_rect;  /* rect holds a saved window position */
    RECT     rect;      /* the normal (not maximized) window rect, in screen pixels */
    int      maximized;
    int      tree, dual, hidden, active;
    int      tree_w;    /* at 96 dpi */
    double   ratio;     /* left pane share, 0.15 .. 0.85 */
    ses_tab  tabs[2][SES_TABS_MAX];
    int      ntabs[2], cur[2];
    wchar_t *pins[SES_PINS_MAX];
    int      npins;
    wchar_t *recent[SES_RECENT_MAX]; /* recently shown folders, newest first */
    int      nrecent;
} session;

void   session_defaults(session *s);
/* Fills defaults, then whatever text provides. */
void   session_parse(const char *text, size_t len, session *s);
/* ASCII/UTF-8 text; returns its length (0 if cap is too small). */
size_t session_format(const session *s, char *buf, size_t cap);
void   session_free(session *s);
