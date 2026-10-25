/* palette.h - one overlay to run any function by name, jump to a folder,
 * or search with Everything (palette.c).
 *
 * It exists only while open: the window, its lists and Everything's
 * results are created when it opens and freed when it closes, and the
 * function that opened it holds the registry until then. */
#pragma once
#include "app.h"

void palette_register(void); /* palette.* and search.everything */
int  palette_shown(void);
void palette_layout(void);   /* the main window changed size */
void palette_redraw(void);   /* icons arrived */
void palette_close(void);
