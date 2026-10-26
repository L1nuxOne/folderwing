/* Native Linux adapter for the existing listing/watching protocol. */
#pragma once
#include <gio/gio.h>
#define FW_TYPE_BROWSER (fw_browser_get_type())
G_DECLARE_FINAL_TYPE(FwBrowser, fw_browser, FW, BROWSER, GObject)
FwBrowser *fw_browser_new(const char *helper);
void fw_browser_navigate(FwBrowser *self, const char *absolute_path);
void fw_browser_refresh(FwBrowser *self);
void fw_browser_close(FwBrowser *self);
GListModel *fw_browser_entries(FwBrowser *self); /* borrowed GFileInfo objects */
const char *fw_browser_path(FwBrowser *self); /* exact native bytes, committed */
const char *fw_browser_error(FwBrowser *self); /* displayable UTF-8 */
gboolean fw_browser_busy(FwBrowser *self);
gboolean fw_browser_watching(FwBrowser *self);
guint fw_browser_revision(FwBrowser *self);
double fw_browser_elapsed_ms(FwBrowser *self);
/* Reversible UTF-8 display: controls/invalid bytes use \xHH, backslash uses \\. */
char *fw_display_bytes(const char *bytes);
char *fw_parse_path(const char *text, const char *base, GError **error);
char *fw_parse_name(const char *text, GError **error); /* literal single component */
gboolean fw_entry_directory(GFileInfo *info);
