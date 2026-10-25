/* keys.h - key names as the settings file writes them ("Ctrl+Shift+N", "F7",
 * "Comma"). Shared by the function registry (app/fn.c) and the settings app
 * (app/fpsettings.c), so a key recorded in the panel is named exactly as the
 * registry reads it back. Punctuation follows the keyboard layout. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define FN_CTRL  0x1u
#define FN_SHIFT 0x2u
#define FN_ALT   0x4u

typedef struct fn_binding {
    uint8_t vk, mods;
} fn_binding;

int  fn_parse_key(const char *s, fn_binding *out); /* "Ctrl+Shift+N" -> 0 */
void fn_format_key(fn_binding k, wchar_t *buf, size_t cap);
/* Can the settings file name it? Its name must read back as the same key
 * (not so for a pure modifier, or punctuation that needs AltGr). */
int  fn_key_nameable(fn_binding k);
