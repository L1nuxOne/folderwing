/* termutil.h - the panel terminal's (app/fpterm.c) pieces that need no window:
 * which shell a program is, how it is started so it reports its folder and
 * prompts, what it reports, and how it is told to change folder. */
#pragma once
#include <stddef.h>
#include <wchar.h>

enum { TERM_SH_OTHER, TERM_SH_CMD, TERM_SH_PWSH, TERM_SH_WSL };

int term_shell_kind(const wchar_t *exe); /* by file name: cmd.exe, pwsh.exe / powershell.exe */

/* The folder an OSC reports (payload without "ESC ]" and the terminator), as a Windows path:
 * 7 file://host/C:/x%20y, 9 "9;C:\x", 1337 CurrentDir=C:\x. 1 if it is one. */
int term_osc_cwd(int cmd, const char *payload, size_t n, wchar_t *out, size_t cap);

/* What to type (without Enter) so an idle shell in from (NULL: unknown) goes to dir: relative when
 * it is the parent or inside; 0 if this shell can't be told safely. */
int term_cd_command(int kind, const wchar_t *from, const wchar_t *dir, wchar_t *out, size_t cap);

/* cmd: the PROMPT that also reports the folder and marks the prompt, around user (NULL: $P$G) */
int term_cmd_prompt(const wchar_t *user, wchar_t *out, size_t cap);

/* PowerShell: the command (run with -NoExit) that wraps the prompt so it reports the same */
const wchar_t *term_pwsh_hook(void);

/* base64 of s as UTF-16LE, for -EncodedCommand; returns the length or 0 if cap is too small */
size_t term_base64_utf16(const wchar_t *s, wchar_t *out, size_t cap);

/* UNC distro and native Linux path (reserved Windows name characters are decoded). */
int term_wsl_split(const wchar_t *path, wchar_t *distro, size_t dc, wchar_t *linux, size_t lc);
/* x + raw Linux path in hex, or w + UTF-8 Windows path in hex; safe as one argv. */
int term_wsl_arg(const wchar_t *path, wchar_t *out, size_t cap);
