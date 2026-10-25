/* Key names: see keys.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "keys.h"

/* The first name for a key is the one shown in menus and written to the file. */
static const struct {
    const char *name;
    uint8_t     vk;
} key_names[] = {
    { "Enter", VK_RETURN }, { "Esc", VK_ESCAPE },  { "Space", VK_SPACE },     { "Tab", VK_TAB },
    { "Backspace", VK_BACK }, { "Delete", VK_DELETE }, { "Insert", VK_INSERT }, { "Home", VK_HOME },
    { "End", VK_END },      { "PgUp", VK_PRIOR },   { "PgDn", VK_NEXT },      { "Up", VK_UP },
    { "Down", VK_DOWN },    { "Left", VK_LEFT },    { "Right", VK_RIGHT },    { "Menu", VK_APPS },
    { "Comma", VK_OEM_COMMA }, { "Plus", VK_OEM_PLUS }, { "Minus", VK_OEM_MINUS }, { "Backquote", VK_OEM_3 },
    { "Return", VK_RETURN }, { "Escape", VK_ESCAPE }, { "Del", VK_DELETE },   { "Ins", VK_INSERT },
    { "PageUp", VK_PRIOR }, { "PageDown", VK_NEXT }, { "Apps", VK_APPS },
};

static int parse_key_name(const char *s, size_t len, fn_binding *k)
{
    if (!len) return -1;
    if ((s[0] == 'F' || s[0] == 'f') && len >= 2 && len <= 3) {
        int n = 0;
        for (size_t i = 1; i < len; i++) {
            if (s[i] < '0' || s[i] > '9') { n = -1; break; }
            n = n * 10 + (s[i] - '0');
        }
        if (n >= 1 && n <= 24) { k->vk = (uint8_t)(VK_F1 + n - 1); return 0; }
    }
    for (size_t i = 0; i < sizeof key_names / sizeof key_names[0]; i++)
        if (strlen(key_names[i].name) == len && !_strnicmp(key_names[i].name, s, len)) {
            k->vk = key_names[i].vk;
            return 0;
        }
    if (len != 1) return -1;
    char c = s[0];
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) { k->vk = (uint8_t)c; return 0; }
    if (c <= ' ' || c > '~') return -1;
    /* punctuation depends on the keyboard layout */
    SHORT v = VkKeyScanW((WCHAR)c);
    if (v == -1 || (HIBYTE(v) & 6)) return -1; /* needs Ctrl or AltGr: not bindable */
    k->vk = LOBYTE(v);
    if (HIBYTE(v) & 1) k->mods |= FN_SHIFT;
    return 0;
}

int fn_parse_key(const char *s, fn_binding *out)
{
    fn_binding k = { 0, 0 };
    while (*s == ' ') s++;
    size_t len = strlen(s);
    while (len && s[len - 1] == ' ') len--;
    if (!len) return -1;
    /* the key is after the last '+', unless the key is '+' itself */
    size_t key_at = len - 1;
    if (!(s[len - 1] == '+' && (len == 1 || s[len - 2] == '+'))) {
        while (key_at > 0 && s[key_at - 1] != '+') key_at--;
    }
    for (size_t p = 0; p < key_at;) {
        size_t e = p;
        while (e < key_at && s[e] != '+') e++;
        size_t b = p, l = e - p;
        while (l && s[b] == ' ') { b++; l--; }
        while (l && s[b + l - 1] == ' ') l--;
        if (l == 4 && !_strnicmp(s + b, "Ctrl", 4)) k.mods |= FN_CTRL;
        else if (l == 7 && !_strnicmp(s + b, "Control", 7)) k.mods |= FN_CTRL;
        else if (l == 5 && !_strnicmp(s + b, "Shift", 5)) k.mods |= FN_SHIFT;
        else if (l == 3 && !_strnicmp(s + b, "Alt", 3)) k.mods |= FN_ALT;
        else return -1;
        p = e + 1;
    }
    const char *name = s + key_at;
    size_t nl = len - key_at;
    while (nl && *name == ' ') { name++; nl--; }
    if (parse_key_name(name, nl, &k)) return -1;
    *out = k;
    return 0;
}

void fn_format_key(fn_binding k, wchar_t *buf, size_t cap)
{
    wchar_t name[16] = L"";
    if (k.vk >= VK_F1 && k.vk <= VK_F24) {
        _snwprintf(name, 16, L"F%d", k.vk - VK_F1 + 1);
    } else if ((k.vk >= 'A' && k.vk <= 'Z') || (k.vk >= '0' && k.vk <= '9')) {
        name[0] = k.vk;
        name[1] = 0;
    } else {
        for (size_t i = 0; i < sizeof key_names / sizeof key_names[0] && !name[0]; i++)
            if (key_names[i].vk == k.vk) _snwprintf(name, 16, L"%hs", key_names[i].name);
        if (!name[0]) {
            UINT ch = MapVirtualKeyW(k.vk, MAPVK_VK_TO_CHAR) & 0x7FFF;
            if (ch > ' ') { name[0] = (wchar_t)ch; name[1] = 0; }
            else _snwprintf(name, 16, L"0x%02X", k.vk);
        }
    }
    name[15] = 0;
    _snwprintf(buf, cap, L"%ls%ls%ls%ls", k.mods & FN_CTRL ? L"Ctrl+" : L"", k.mods & FN_SHIFT ? L"Shift+" : L"",
               k.mods & FN_ALT ? L"Alt+" : L"", name);
    if (cap) buf[cap - 1] = 0;
}

int fn_key_nameable(fn_binding k)
{
    switch (k.vk) {
    case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL:
    case VK_RCONTROL: case VK_LMENU: case VK_RMENU: case VK_LWIN: case VK_RWIN: case VK_CAPITAL:
    case VK_NUMLOCK: case VK_SCROLL: case 0: return 0;
    }
    wchar_t w[48];
    char a[48];
    fn_format_key(k, w, 48);
    fn_binding back;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, a, sizeof a, NULL, NULL) > 0 && !fn_parse_key(a, &back) &&
           back.vk == k.vk && back.mods == k.mods;
}
