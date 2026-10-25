/* Main-view presentation state. The existing scanner/model remains authoritative. */
#pragma once
#include "spacevolumes.h"
typedef struct space_catalog {HANDLE thread,cancel;HWND hwnd;SRWLOCK lock;space_volume volumes[SPACE_VOLUME_LIMIT];uint32_t count;int wsl;volatile LONG revision;} space_catalog;
static struct {int enabled,wsl,catalog_pending,focus,top[3];space_catalog *catalog;LONG catalog_revision;space_volume volumes[SPACE_VOLUME_LIMIT];uint32_t volume_count,volume_selected,right_scope,left_selected,right_selected;sp_row *right;uint32_t right_count;uint64_t revision,epoch;int metric,sort;RECT tree,left,right_rect;wchar_t filter[128];} B;
static void browser_paint(int rebuild_rows);
static void browser_input(const panelp_input *in);
static void browser_config(int wsl);
static int browser_tick(void);
static int browser_shutdown(void);
