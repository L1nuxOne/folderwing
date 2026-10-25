/* Installed action metadata only. No IContextMenu/Explorer extension is
 * instantiated during discovery; execution stays in the existing shell helper. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "relayedit.h"
static void utf8(const wchar_t *w, char *out, int cap)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL))
        out[0] = 0;
    for (char *p = out; *p; ++p)
        if ((unsigned char)*p < 32)
            *p = ' ';
}
static int value(HKEY k, const wchar_t *name, wchar_t *out, DWORD cap)
{
    DWORD type = 0, bytes = cap * 2;
    out[0] = 0;
    if (RegQueryValueExW(k, name, NULL, &type, (BYTE *)out, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < 2 || bytes > cap * 2) {
        out[0] = 0;
        return 0;
    }
    out[cap - 1] = 0;
    return out[0] != 0;
}
static int present(HKEY k, const wchar_t *name)
{
    return RegQueryValueExW(k, name, NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
}
static int valid_guid(const wchar_t *s)
{
    CLSID id;
    return wcslen(s) == 38 && SUCCEEDED(IIDFromString((wchar_t *)s, &id));
}
static void clean_title(wchar_t *s)
{
    if (s[0] == '@') {
        s[0] = 0;
        return;
    } /* a resource reference is not an item name */
    wchar_t *d = s;
    for (wchar_t *p = s; *p; ++p) {
        if (*p == '&') {
            if (p[1] != '&')
                continue;
            ++p;
        }
        *d++ = *p < 32 ? L' ' : *p;
    }
    *d = 0;
}
static void scan_node(relay_catalog *c, const wchar_t *node, uint32_t scopes, const char *types)
{
    wchar_t path[1024];
    _snwprintf(path, 1024, L"%ls\\shell", node);
    HKEY shell;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, path, 0, KEY_READ, &shell) == ERROR_SUCCESS) {
        for (DWORD i = 0; i < 256; ++i) {
            wchar_t name[256];
            DWORD chars = 256;
            LONG rc = RegEnumKeyExW(shell, i, name, &chars, NULL, NULL, NULL, NULL);
            if (rc == ERROR_NO_MORE_ITEMS)
                break;
            if (rc != ERROR_SUCCESS)
                continue;
            HKEY key;
            if (RegOpenKeyExW(shell, name, 0, KEY_READ, &key) != ERROR_SUCCESS)
                continue;
            if (present(key, L"LegacyDisable") || present(key, L"ProgrammaticAccessOnly")) {
                RegCloseKey(key);
                continue;
            }
            wchar_t title[256], handler[256], command[RELAY_LINE_MAX];
            if (!value(key, L"MUIVerb", title, 256) && !value(key, NULL, title, 256))
                wcscpy(title, name);
            int indirect = title[0] == L'@';
            clean_title(title);
            if (!title[0])
                wcscpy(title, name);
            handler[0] = 0;
            value(key, L"ExplorerCommandHandler", handler, 256);
            HKEY cmd;
            int executable = 0;
            if (RegOpenKeyExW(key, L"command", 0, KEY_READ, &cmd) == ERROR_SUCCESS) {
                executable = value(cmd, NULL, command, RELAY_LINE_MAX);
                RegCloseKey(cmd);
            }
            if (executable || valid_guid(handler)) {
                relay_catalog_entry e = {.kind = executable ? RELAY_RUN : RELAY_ITEM, .scopes = scopes};
                utf8(title, e.title, sizeof e.title);
                if (executable)
                    utf8(command, e.command, sizeof e.command);
                else
                    utf8(indirect ? L"" : title, e.action, sizeof e.action);
                if (!executable)
                    utf8(handler, e.handler, sizeof e.handler);
                snprintf(e.types, sizeof e.types, "%s", types);
                utf8(path, e.source, sizeof e.source);
                if (!executable || e.command[0])
                    relay_catalog_add(c, &e);
            }
            RegCloseKey(key);
        }
        RegCloseKey(shell);
    }
    _snwprintf(path, 1024, L"%ls\\shellex\\ContextMenuHandlers", node);
    HKEY handlers;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, path, 0, KEY_READ, &handlers) != ERROR_SUCCESS)
        return;
    for (DWORD i = 0; i < 256; ++i) {
        wchar_t name[256];
        DWORD chars = 256;
        LONG rc = RegEnumKeyExW(handlers, i, name, &chars, NULL, NULL, NULL, NULL);
        if (rc == ERROR_NO_MORE_ITEMS)
            break;
        if (rc != ERROR_SUCCESS)
            continue;
        HKEY key;
        if (RegOpenKeyExW(handlers, name, 0, KEY_READ, &key) != ERROR_SUCCESS)
            continue;
        wchar_t guid[256];
        if (!value(key, NULL, guid, 256))
            wcscpy(guid, name);
        RegCloseKey(key);
        if (!valid_guid(guid))
            continue;
        wchar_t label[256], clsid[300];
        wcscpy(label, name);
        _snwprintf(clsid, 300, L"CLSID\\%ls", guid);
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, clsid, 0, KEY_READ, &key) == ERROR_SUCCESS) {
            value(key, NULL, label, 256);
            RegCloseKey(key);
        }
        if (!label[0])
            wcscpy(label, name);
        clean_title(label);
        relay_catalog_entry e = {.kind = RELAY_ITEM, .scopes = scopes};
        utf8(label, e.title, sizeof e.title);
        utf8(guid, e.handler, sizeof e.handler);
        snprintf(e.types, sizeof e.types, "%s", types);
        utf8(path, e.source, sizeof e.source);
        relay_catalog_add(c, &e);
    }
    RegCloseKey(handlers);
}
void relay_catalog_scan(relay_catalog *c, const wchar_t *file)
{
    memset(c, 0, sizeof *c);
    scan_node(c, L"*", RELAY_FILES, "");
    scan_node(c, L"AllFilesystemObjects", RELAY_FILES | RELAY_FOLDERS, "");
    scan_node(c, L"Directory", RELAY_FOLDERS, "");
    scan_node(c, L"Folder", RELAY_FOLDERS, "");
    scan_node(c, L"Directory\\Background", RELAY_BACKGROUND, "");
    const wchar_t *slash = file ? wcsrchr(file, L'\\') : NULL, *dot = file ? wcsrchr(file, L'.') : NULL;
    if (dot && (!slash || dot > slash) && wcslen(dot) < 64) {
        char types[256];
        utf8(dot + 1, types, sizeof types);
        wchar_t node[256], prog[256];
        HKEY key;
        _snwprintf(node, 256, L"SystemFileAssociations\\%ls", dot);
        scan_node(c, node, RELAY_FILES, types);
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, dot, 0, KEY_READ, &key) == ERROR_SUCCESS) {
            if (value(key, NULL, prog, 256) && !wcschr(prog, L'\\'))
                scan_node(c, prog, RELAY_FILES, types);
            wchar_t perceived[128];
            if (value(key, L"PerceivedType", perceived, 128) && !wcschr(perceived, L'\\')) {
                _snwprintf(node, 256, L"SystemFileAssociations\\%ls", perceived);
                scan_node(c, node, RELAY_FILES, types);
            }
            RegCloseKey(key);
        }
        wchar_t user[512];
        _snwprintf(user, 512,
                   L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\%ls\\UserChoice", dot);
        if (RegOpenKeyExW(HKEY_CURRENT_USER, user, 0, KEY_READ, &key) == ERROR_SUCCESS) {
            if (value(key, L"ProgId", prog, 256) && !wcschr(prog, L'\\'))
                scan_node(c, prog, RELAY_FILES, types);
            RegCloseKey(key);
        }
    }
    wchar_t appdata[MAX_PATH], pattern[MAX_PATH + 80];
    if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH))
        return;
    _snwprintf(pattern, MAX_PATH + 80, L"%ls\\Microsoft\\Windows\\SendTo\\*", appdata);
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE)
        return;
    unsigned seen = 0;
    do {
        if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN))
            continue;
        wchar_t title[MAX_PATH];
        wcscpy(title, data.cFileName);
        wchar_t *ext = wcsrchr(title, L'.');
        if (ext)
            *ext = 0;
        relay_catalog_entry e = {.kind = RELAY_SENDTO, .scopes = RELAY_FILES | RELAY_FOLDERS};
        utf8(title, e.title, sizeof e.title);
        utf8(title, e.action, sizeof e.action);
        snprintf(e.source, sizeof e.source, "Windows Send to");
        relay_catalog_add(c, &e);
    } while (++seen < 256 && FindNextFileW(find, &data));
    FindClose(find);
}
