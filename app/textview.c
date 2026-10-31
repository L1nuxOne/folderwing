#include "../src/namecodec.h"
/* Native text reader: immutable helper snapshots, only visible rows painted. */
#include "textview.h"
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#include <windowsx.h>
#include <shellapi.h>
#include <uxtheme.h>
#include "app.h"
#include "menu.h"
#include "theme.h"
#include "docmarks.h"
#include "readingrail.h"
#include "textutil.h"
#include "previewlaunch.h"
#include "previewreader.h"
#include "pipewriter.h"
#define WM_TEXT_MSG (WM_APP + 80)
#define WM_TEXT_GONE (WM_APP + 81)
#define WM_TEXT_LAUNCHED (WM_APP + 82)
#define WM_MARKDOWN_READY (WM_APP + 83)
#define WM_MARKDOWN_FAILED (WM_APP + 84)
#define TIMER_FIND 1
#define TIMER_WATCHDOG 2
#define TIMER_EDGE 3
#define TIMER_CLIPBOARD 4
#define TIMER_MARKDOWN 5
typedef struct msg {
    struct msg *next;
    textp_head head;
    uint8_t data[];
} msg;
static struct {
    HWND hwnd, find;
    WNDPROC find_orig;
    HBRUSH find_brush;
    HFONT font;
    int cw, ch, cell, line, bw, bh;
    HDC bdc;
    HBITMAP bmp;
    HGDIOBJ old_bmp;
    HANDLE process, job, section, to, from, child_in, child_out;
    preview_reader *reader;
    pipe_writer *writer;
    preview_launch *launcher;
    uint32_t launch_gen;
    DWORD pid;
    uint8_t *pixels;
    int gone;
    wchar_t *path;
    wchar_t *nav_file,*nav_identity,*nav_key;
    reading_rail rail;docmark nav_marks[DOCMARK_PER_FILE];uint32_t nav_count;int nav_loaded,nav_targets_count;reading_target nav_targets[RR_CAPACITY];
    uint64_t size;
    int64_t mtime;
    uint32_t doc, ticket, limit_mb, encoding;
    int wrap, lines;
    int front, loaded, want_open, want_layout, copying, selecting;
    uint32_t anchor, caret, copy_start, copy_end;
    uint32_t top, left, restore_at;
    uint32_t jump_doc,jump_line,jump_column;wchar_t jump_expected[TEXTP_QUERY_MAX];
    POINT mouse;
    HRESULT err;
    textp_info info;
    struct {
        int busy, kind;
        uint32_t ticket, doc, columns;
    } banks[TEXTP_BANKS];
    uint32_t find_gen, query_chars, match_count, matches[TEXTP_MATCH_MAX], match_index, match_more;
    wchar_t query[TEXTP_QUERY_MAX];
    int finding;
    HGLOBAL clipboard;
    uint64_t clipboard_until;
    DWORD clipboard_sequence;
    uint64_t started;
    HANDLE markdown_process, markdown_job;
    DWORD markdown_pid;
    HWND markdown_hwnd;
    int markdown, markdown_rendered, markdown_wait, archived;
} R;
/* Keyboard scope follows the receiving viewer during dispatch. This keeps
 * selection/copy local even when background input leaves OS focus elsewhere. */
static HWND key_receiver;
static int view_width(void){return R.markdown_rendered?R.cw:R.cw-app_px(28);}
static int head_h(void) { return app_px(R.find ? 84 : 54); }
static int gutter(void) { return R.lines ? R.cell * 8 : app_px(12); }
static uint32_t columns(void)
{
    int n = (view_width() - gutter() - app_px(12)) / R.cell;
    return R.wrap ? (uint32_t)(n < 4 ? 4 : n) : 0;
}
static void request_work(void);
static void helper_stop(void);
static void title_update(void);
static void search_start(void);
static void find_close(void);
static int binary_preview(void) { return R.markdown == 6 || R.markdown == 8 || R.markdown == 9; }
static void markdown_stop(void)
{
    if (R.hwnd) KillTimer(R.hwnd, TIMER_MARKDOWN);
    if (R.markdown_process) {
        TerminateProcess(R.markdown_process, 0);
        CloseHandle(R.markdown_process);
    }
    if (R.markdown_job) CloseHandle(R.markdown_job);
    R.markdown_job = NULL; R.markdown_process = NULL; R.markdown_hwnd = NULL; R.markdown_pid = 0; R.markdown_wait = 0;
}
static void markdown_place(void)
{
    if (!R.markdown_hwnd) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(R.markdown_hwnd, &pid);
    if (pid != R.markdown_pid) { R.markdown_hwnd = NULL; return; }
    int top = head_h();
    SetWindowPos(R.markdown_hwnd, NULL, 0, top, R.cw > 0 ? R.cw : 1,
                 R.ch > top ? R.ch - top : 1, SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    ShowWindowAsync(R.markdown_hwnd, R.markdown_rendered ? SW_SHOWNA : SW_HIDE);
}
static void markdown_start(void)
{
    wchar_t dir[MAX_PATH], exe[MAX_PATH + 40], *cmd;
    if (!R.path || R.markdown_process) return;
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    wchar_t *slash = n && n < MAX_PATH ? wcsrchr(dir, L'\\') : NULL;
    if (!slash) { R.markdown_rendered = 0; if (binary_preview()) R.err = E_FAIL; return; }
    slash[1] = 0;
    _snwprintf(exe, MAX_PATH + 40, L"%ls%ls", dir, R.markdown == 1 ? L"folderwing-markdown.exe" : R.markdown == 2 ? L"folderwing-json.exe" : R.markdown == 3 ? L"folderwing-table.exe" : R.markdown == 4 ? L"folderwing-notebook.exe" : R.markdown == 5 ? L"folderwing-config.exe" : R.markdown == 6 ? L"folderwing-database.exe" : R.markdown == 7 ? L"folderwing-xml.exe" : R.markdown == 8 ? L"folderwing-spreadsheet.exe" : L"folderwing-word.exe");
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) { R.markdown_rendered = 0; if (binary_preview()) R.err = E_FAIL; return; }
    size_t cap = wcslen(exe) + wcslen(R.path) + (R.nav_file?wcslen(R.nav_file):0) + (R.nav_identity?wcslen(R.nav_identity):0) + 120;
    cmd = malloc(cap * sizeof *cmd);
    if (!cmd) { R.markdown_rendered = 0; if (binary_preview()) R.err = E_OUTOFMEMORY; return; }
    if (R.markdown == 6)
        _snwprintf(cmd, cap, L"\"%ls\" %llu %u \"%ls\" %d", exe,
                   (unsigned long long)(uintptr_t)R.hwnd, R.limit_mb * 1024u * 1024u, R.path, R.archived);
    else
        _snwprintf(cmd, cap, L"\"%ls\" %llu %u \"%ls\"", exe,
                   (unsigned long long)(uintptr_t)R.hwnd, R.limit_mb * 1024u * 1024u, R.path);
    size_t used=wcslen(cmd);_snwprintf(cmd+used,cap-used,L" \"%ls\" \"%ls\"",R.nav_file?R.nav_file:L"",R.nav_identity?R.nav_identity:L"");
    STARTUPINFOW si = {sizeof si}; PROCESS_INFORMATION pi = {0};
    HANDLE job = NULL;
    if (R.markdown == 8 || R.markdown == 9) {
        job = CreateJobObjectW(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION bounds = {0};
        bounds.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        bounds.ProcessMemoryLimit = (SIZE_T)1024 * 1024 * 1024;
        if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &bounds, sizeof bounds)) {
            if (job) CloseHandle(job);
            free(cmd); R.markdown_rendered = 0; R.err = E_FAIL; return;
        }
    }
    DWORD flags = CREATE_NO_WINDOW | (job ? CREATE_SUSPENDED : 0);
    if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, flags, NULL, dir, &si, &pi)) {
        if (job && (!AssignProcessToJobObject(job, pi.hProcess) || ResumeThread(pi.hThread) == (DWORD)-1)) {
            TerminateProcess(pi.hProcess, 1); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(job);
            R.markdown_rendered = 0; R.err = E_FAIL;
        } else {
            R.markdown_job = job; R.markdown_process = pi.hProcess; R.markdown_pid = pi.dwProcessId; CloseHandle(pi.hThread);
            SetTimer(R.hwnd, TIMER_MARKDOWN, 1000, NULL);
        }
    } else { if (job) CloseHandle(job); R.markdown_rendered = 0; if (binary_preview()) R.err = E_FAIL; }
    free(cmd);
}
static void markdown_mode(int rendered)
{
    R.markdown_rendered = !!rendered;
    if (R.markdown_rendered) markdown_start();
    markdown_place();
    InvalidateRect(R.hwnd, NULL, FALSE);
}
static void exe_dir(wchar_t *buf, size_t cap)
{
    DWORD n = GetModuleFileNameW(NULL, buf, (DWORD)cap);
    wchar_t *s = n && n < cap ? wcsrchr(buf, L'\\') : NULL;
    if (s)
        s[1] = 0;
    else
        buf[0] = 0;
}
int textview_available(void)
{
    wchar_t dir[MAX_PATH], p[MAX_PATH + 16];
    exe_dir(dir, MAX_PATH);
    _snwprintf(p, MAX_PATH + 16, L"%lsfolderwing-text.exe", dir);
    p[MAX_PATH + 15] = 0;
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}
static int read_all(HANDLE h, void *buf, DWORD n)
{
    uint8_t *p = buf;
    while (n) {
        DWORD g;
        if (!ReadFile(h, p, n, &g, NULL) || !g)
            return -1;
        p += g;
        n -= g;
    }
    return 0;
}
static DWORD WINAPI from_helper(void *arg)
{
    preview_reader *reader = arg;
    HANDLE in = reader->pipe;
    HWND to = reader->window;
    uint32_t generation = reader->generation;
    for (;;) {
        textp_head h;
        if (read_all(in, &h, sizeof h) || h.size > sizeof(textp_found) + TEXTP_MATCH_MAX * 4 ||
            !((h.type == TEXTP_READY && h.size == sizeof(textp_info)) ||
              (h.type == TEXTP_COPIED && h.size == sizeof(textp_copied)) ||
              (h.type == TEXTP_FOUND && h.size >= sizeof(textp_found))))
            break;
        msg *m = malloc(sizeof *m + h.size);
        if (!m || read_all(in, m->data, h.size)) {
            free(m);
            break;
        }
        m->head = h;
        if (InterlockedCompareExchange(&reader->cancelled, 0, 0) ||
            !PostMessageW(to, WM_TEXT_MSG, generation, (LPARAM)m))
            free(m);
    }
    if (!InterlockedCompareExchange(&reader->cancelled, 0, 0)) PostMessageW(to, WM_TEXT_GONE, generation, 0);
    preview_reader_finish(reader);
    return 0;
}
static int send_req(uint32_t type, uint32_t doc, uint32_t ticket, const void *data, uint32_t size)
{
    if (!R.writer) return -1;
    size_t n = sizeof(textp_head) + size;
    uint8_t *bytes = malloc(n);
    if (!bytes) { PostMessageW(R.hwnd, WM_TEXT_GONE, R.launch_gen, 0); return -1; }
    textp_head h = {type, size, doc, ticket};
    memcpy(bytes, &h, sizeof h);
    if (size) memcpy(bytes + sizeof h, data, size);
    uint32_t tag = type == TEXTP_FIND ? 1u : 0u;
    uint32_t drop = tag;
    int rc = pipe_writer_send(R.writer, bytes, n, tag, drop);
    free(bytes);
    if (rc) PostMessageW(R.hwnd, WM_TEXT_GONE, R.launch_gen, 0);
    return rc;
}
static void helper_stop(void)
{
    ++R.launch_gen;
    preview_launch_cancel(&R.launcher);
    pipe_writer_cancel(&R.writer);
    preview_reader_cancel(&R.reader);
    if (R.job)
        TerminateJobObject(R.job, 1);
    else if (R.process)
        TerminateProcess(R.process, 1);
    HANDLE *handles[] = {&R.to, &R.from, &R.process, &R.job, &R.child_in, &R.child_out, &R.section};
    if (R.pixels)
        UnmapViewOfFile(R.pixels);
    R.pixels = NULL;
    for (size_t i = 0; i < sizeof handles / sizeof *handles; ++i)
        if (*handles[i]) {
            CloseHandle(*handles[i]);
            *handles[i] = NULL;
        }
    memset(R.banks, 0, sizeof R.banks);
    R.front = -1;
    R.pid = 0;
    if (R.hwnd) {
        MSG m;
        while (PeekMessageW(&m, R.hwnd, WM_TEXT_MSG, WM_TEXT_MSG, PM_REMOVE))
            free((void *)m.lParam);
        while (PeekMessageW(&m, R.hwnd, WM_TEXT_GONE, WM_TEXT_LAUNCHED, PM_REMOVE)) {
        }
    }
}
/* Pipes and pixels now, the process on a thread: requests written before
 * it runs wait in the pipe. */
static int helper_start(void)
{
    if (R.process || R.launcher)
        return 0;
    int64_t t = fpe_prof_begin();
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    int ok = 0;
    R.section = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE | SEC_COMMIT, 0,
                                   (DWORD)TEXTP_BANKS * TEXTP_BANK_BYTES, NULL);
    R.pixels = R.section ? MapViewOfFile(R.section, FILE_MAP_READ, 0, 0, 0) : NULL;
    if (!R.pixels || !CreatePipe(&R.child_in, &R.to, &sa, 1 << 20) ||
        !CreatePipe(&R.from, &R.child_out, &sa, 1 << 20))
        goto out;
    SetHandleInformation(R.to, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(R.from, HANDLE_FLAG_INHERIT, 0);
    R.writer = pipe_writer_begin(R.to, 1 << 20, R.hwnd, WM_TEXT_GONE, R.launch_gen + 1);
    if (!R.writer) goto out;
    R.reader = preview_reader_begin(R.from, R.hwnd, R.launch_gen + 1, from_helper);
    if (!R.reader) goto out;
    CloseHandle(R.from); R.from = NULL;
    R.launcher = preview_launch_begin(L"folderwing-text.exe", R.child_in, R.child_out, R.section,
                                      (SIZE_T)256 << 20, R.hwnd, WM_TEXT_LAUNCHED, ++R.launch_gen);
    ok = R.launcher != NULL;
    if (ok) {
        CloseHandle(R.child_in); CloseHandle(R.child_out);
        R.child_in = R.child_out = NULL;
    }
out:
    if (!ok)
        helper_stop();
    R.gone = 0;
    FPE_PROF_END("text.helper_start", t);
    return ok ? 0 : -1;
}

static const wchar_t *content(void)
{
    return R.front >= 0 ? (wchar_t *)(R.pixels + (size_t)R.front * TEXTP_BANK_BYTES) : L"";
}
static const uint8_t *styles(void)
{
    return R.pixels + (size_t)R.front * TEXTP_BANK_BYTES + textp_style_offset(R.info.chars);
}
static const uint32_t *colmap(void)
{
    return (uint32_t *)(R.pixels + (size_t)R.front * TEXTP_BANK_BYTES + textp_columns_offset(R.info.chars));
}
static textp_row row_at(uint32_t i)
{
    textp_row empty = {0};
    if (R.front < 0 || i >= R.info.rows)
        return empty;
    const textp_row *rows =
        (textp_row *)(R.pixels + (size_t)R.front * TEXTP_BANK_BYTES + textp_rows_offset(R.info.chars));
    textp_row row = rows[i];
    return row.start <= row.end && row.end <= R.info.chars ? row : empty;
}
static uint32_t row_for(uint32_t at)
{
    uint32_t lo = 0, hi = R.info.rows;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (row_at(mid).start <= at)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo ? lo - 1 : 0;
}
static uint32_t page_rows(void)
{
    int n = (R.ch - head_h()) / R.line;
    return n > 0 ? (uint32_t)n : 1;
}
static void clamp_view(void)
{
    uint32_t max = R.info.rows > page_rows() ? R.info.rows - page_rows() : 0;
    if (R.top > max)
        R.top = max;
    uint32_t visible = (view_width() > gutter() ? (uint32_t)(view_width() - gutter()) / R.cell : 1),
             mx = R.info.max_columns > visible ? R.info.max_columns - visible : 0;
    if (R.wrap)
        R.left = 0;
    else if (R.left > mx)
        R.left = mx;
    SCROLLINFO si = {sizeof si,   SIF_RANGE | SIF_PAGE | SIF_POS,
                     0,           (int)(R.info.rows ? R.info.rows - 1 : 0),
                     page_rows(), (int)R.top};
    SetScrollInfo(R.hwnd, SB_VERT, &si, FALSE);ShowScrollBar(R.hwnd,SB_VERT,FALSE);
}
static void title_update(void)
{
    wchar_t s[512];
    const wchar_t *name = R.path ? wcsrchr(R.path, L'\\') : NULL;
    name = name ? name + 1 : R.path;
    if (!R.path)
        wcscpy(s, L"Text preview");
    else if (binary_preview())
        _snwprintf(s, 512, L"%ls | %ls | Read only", name, R.markdown == 6 ? L"SQLite database" : R.markdown == 8 ? L"Spreadsheet" : L"Word document");
    else if (FAILED(R.err))
        _snwprintf(s, 512, L"%ls | Text unavailable (0x%08lX)", name, (unsigned long)R.err);
    else if (!R.loaded)
        _snwprintf(s, 512, L"%ls | Loading text", name);
    else
        _snwprintf(s, 512, L"%ls | %ls | %ls | %u lines | %ls%ls", name, text_language_name(R.info.language),
                   text_encoding_name(R.info.encoding), R.info.lines, R.wrap ? L"Wrap" : L"No wrap",
                   R.info.flags & TEXTP_PARTIAL ? L" | Partial preview" : L"");
    s[511] = 0;
    if (R.find && R.loaded && wcslen(s) < 400) {
        wchar_t found[80];
        if (R.finding)
            wcscpy(found, L" | Searching");
        else
            _snwprintf(found, 80, L" | Find %u/%u%ls", R.match_count ? R.match_index + 1 : 0, R.match_count,
                       R.match_more ? L"+" : L"");
        wcscat(s, found);
    }
    if(R.path&&fpe_wsl_native(R.path)){wchar_t display[512];fpe_lx_display(s,display,512,1);wcscpy(s,display);}
    SetWindowTextW(R.hwnd, s);
}
static int free_bank(void)
{
    for (unsigned i = 0; i < TEXTP_BANKS; ++i)
        if (!R.banks[i].busy && (int)i != R.front)
            return (int)i;
    return -1;
}
static void bank_sent(unsigned bank, unsigned kind, unsigned cols)
{
    R.banks[bank].busy = 1;
    R.banks[bank].kind = kind;
    R.banks[bank].ticket = ++R.ticket;
    R.banks[bank].doc = R.doc;
    R.banks[bank].columns = cols;
}
static void request_work(void)
{
    if (!R.path || !R.to || R.gone)
        return;
    int bank = free_bank();
    if (bank < 0)
        return;
    if (R.want_open) {
        size_t size = sizeof(textp_open) + (wcslen(R.path) + 1) * 2;
        uint8_t *p = malloc(size);
        if (!p) {
            R.err = E_OUTOFMEMORY;
            return;
        }
        const wchar_t *name = wcsrchr(R.path, L'\\');
        int lang = text_file_language(name ? name + 1 : R.path);
        if (lang < 0)
            lang = TEXT_PLAIN;
        textp_open open = {bank, R.limit_mb, columns(), R.encoding, (uint32_t)lang};
        memcpy(p, &open, sizeof open);
        memcpy(p + sizeof open, R.path, size - sizeof open);
        bank_sent(bank, 1, open.columns);
        if (send_req(TEXTP_OPEN, R.doc, R.ticket, p, (uint32_t)size)) {
            R.banks[bank].busy = 0;
            R.err = E_FAIL;
        }
        free(p);
        R.want_open = 0;
        return;
    }
    if (!R.loaded || FAILED(R.err))
        return;
    if (R.copying == 2) {
        textp_copy p = {bank, R.copy_start, R.copy_end};
        bank_sent(bank, 3, 0);
        if (!send_req(TEXTP_COPY, R.doc, R.ticket, &p, sizeof p))
            R.copying = 1;
        else
            R.banks[bank].busy = 0;
        return;
    }
    if (R.want_layout || R.info.columns != columns()) {
        for (unsigned i = 0; i < TEXTP_BANKS; ++i)
            if (R.banks[i].busy && R.banks[i].doc == R.doc && R.banks[i].kind != 3)
                return;
        textp_layout p = {bank, columns()};
        bank_sent(bank, 2, p.columns);
        if (send_req(TEXTP_LAYOUT, R.doc, R.ticket, &p, sizeof p)) {
            R.banks[bank].busy = 0;
            R.err = E_FAIL;
        }
        R.want_layout = 0;
    }
}
static void view_changed(void)
{
    clamp_view();
    title_update();
    InvalidateRect(R.hwnd, NULL, FALSE);
}
static uint32_t column_at(uint32_t at)
{
    uint32_t n = colmap()[at];
    return n <= TEXTP_MAX_CHARS * 4 ? n : 0;
}
static uint32_t at_column(textp_row row, uint32_t target)
{
    uint32_t lo = row.start, hi = row.end;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (column_at(mid) <= target)
            lo = mid + 1;
        else
            hi = mid;
    }
    uint32_t at = lo > row.start ? lo - 1 : row.start;
    const wchar_t *s = content();
    if (at > row.start && s[at] >= 0xDC00 && s[at] <= 0xDFFF)
        --at;
    return at;
}
static uint32_t hit(int x, int y)
{
    if (!R.loaded || R.front < 0)
        return 0;
    int line = (y - head_h()) / R.line;
    if (y < head_h())
        line = 0;
    if (line < 0)
        line = 0;
    uint32_t index = R.top + (uint32_t)line;
    if (index >= R.info.rows)
        return R.info.chars;
    textp_row row = row_at(index);
    int pixels = x - gutter();
    if (pixels < 0)
        return row.start;
    uint32_t target = column_at(row.start) + R.left + (uint32_t)(pixels + R.cell / 2) / R.cell;
    if (target >= column_at(row.end))
        return row.end;
    return at_column(row, target);
}
static void reveal(uint32_t at)
{
    uint32_t row = row_for(at);
    if (row < R.top)
        R.top = row;
    else if (row >= R.top + page_rows())
        R.top = row - page_rows() + 1;
    if (!R.wrap) {
        uint32_t c = column_at(at), width = (R.cw - gutter()) / R.cell;
        if (c < R.left)
            R.left = c;
        else if (c >= R.left + width)
            R.left = c - width + 1;
    }
    view_changed();
}
static void selection(uint32_t a, uint32_t b)
{
    if (a > R.info.chars)
        a = R.info.chars;
    if (b > R.info.chars)
        b = R.info.chars;
    R.anchor = a;
    R.caret = b;
    InvalidateRect(R.hwnd, NULL, FALSE);
}
static void apply_location(void)
{
    if(!R.jump_doc||R.jump_doc!=R.doc||!R.loaded)return;
    uint32_t wanted=R.jump_line,column=R.jump_column;R.jump_doc=0;
    const wchar_t *source=content();uint32_t at=0,line=1;
    while(at<R.info.chars&&line<wanted)if(source[at++]=='\n')line++;
    uint32_t end=at;while(end<R.info.chars&&source[end]!='\n')end++;
    size_t length=wcslen(R.jump_expected);
    if(line!=wanted||!column||column-1>end-at||length>end-at-(column-1)||wcsncmp(source+at+column-1,R.jump_expected,length)){
        app_status(NULL,1,R.info.flags&TEXTP_PARTIAL?L"Search match is outside or differs from this limited preview":L"Search match no longer agrees with the file or preview encoding; search again");return;
    }
    at+=column-1;selection(at,at+(uint32_t)length);R.restore_at=at;R.top=row_for(at);R.left=0;reveal(at);view_changed();
}
int textview_goto_match(const wchar_t *path,uint32_t line,uint32_t column,const wchar_t *expected)
{
    if (binary_preview()) return 0;
    if(!R.path||!path||!fpe_path_equal(path,R.path)||!line||!column||!expected||!*expected||wcslen(expected)>=TEXTP_QUERY_MAX)return 0;
    if (R.markdown) markdown_mode(0);
    R.jump_doc=R.doc;R.jump_line=line;R.jump_column=column;wcscpy(R.jump_expected,expected);apply_location();return 1;
}
int textview_selected(void) { return R.loaded && R.anchor != R.caret; }
int textview_is_binary_preview(void) { return binary_preview(); }
int textview_key_scope(void) { return R.hwnd && (key_receiver == R.hwnd || GetFocus() == R.hwnd); }
static void fill(HDC dc, int x, int y, int w, int h, COLORREF c)
{
    RECT r = {x, y, x + w, y + h};
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}
static int match_at(uint32_t at, uint32_t *index)
{
    uint32_t lo = 0, hi = R.match_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (R.matches[mid] <= at)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo && at < R.matches[lo - 1] + R.query_chars) {
        *index = lo - 1;
        return 1;
    }
    return 0;
}
#include "textnav.inc"

static void paint(HDC dc)
{
    fill(dc, 0, 0, R.cw, R.ch, C_BG);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, app_font());
    SetTextColor(dc, C_TEXT);
    fill(dc, 0, 0, R.cw, head_h(), C_HEADER);
    wchar_t s[512];
    GetWindowTextW(R.hwnd, s, 512);
    RECT rect = {app_px(10), app_px(5), R.cw - app_px(40), app_px(28)};
    if (R.markdown && !binary_preview()) rect.right = R.cw - app_px(199);
    DrawTextW(dc, s, -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (R.markdown && !binary_preview()) {
        RECT a = {R.cw - app_px(192), app_px(5), R.cw - app_px(117), app_px(26)};
        RECT b = {R.cw - app_px(112), app_px(5), R.cw - app_px(37), app_px(26)};
        fill(dc, a.left, a.top, a.right-a.left, a.bottom-a.top, R.markdown_rendered ? C_ACCENT : C_BG);
        fill(dc, b.left, b.top, b.right-b.left, b.bottom-b.top, !R.markdown_rendered ? C_ACCENT : C_BG);
        SetTextColor(dc, R.markdown_rendered ? C_BG : C_TEXT); DrawTextW(dc, R.markdown == 1 ? L"Rendered" : R.markdown == 2 || R.markdown == 5 || R.markdown == 7 ? L"Structured" : R.markdown == 3 ? L"Table" : L"Notebook", -1, &a, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SetTextColor(dc, !R.markdown_rendered ? C_BG : C_TEXT); DrawTextW(dc, L"Source", -1, &b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (R.markdown == 6)
        wcscpy(s, R.err == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) ? L"Database preview is not yet available for WSL files." : FAILED(R.err) ? L"Database preview unavailable. Check that the database helper is installed." : L"Tables, views and rows are opened read-only.");
    else if (R.markdown == 8)
        wcscpy(s, R.err == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) ? L"Spreadsheet preview is not yet available for WSL files." : FAILED(R.err) ? L"Spreadsheet preview unavailable. Check that the spreadsheet helper is installed." : L"Read only. Formula results come from the workbook and may be stale.");
    else if (R.markdown == 9)
        wcscpy(s, R.err == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) ? L"Word preview is not yet available for WSL files." : FAILED(R.err) ? L"Word preview unavailable. Check that the Word helper is installed." : L"Read-only document preview.");
    else if (FAILED(R.err))
        wcscpy(s, R.info.flags & TEXTP_BINARY ? L"Binary data cannot be previewed as text."
                  : R.err == HRESULT_FROM_WIN32(ERROR_TIMEOUT)
                      ? L"Loading timed out. Select a different file to retry."
                      : L"The file could not be read.");
    else if (!R.loaded)
        wcscpy(s, L"Loading text...");
    else
        _snwprintf(s, 512, L"%.2f MB%ls%ls%ls | %ls | %ls", R.info.bytes / (1024.0 * 1024),
                   R.info.flags & TEXTP_PARTIAL ? L" (limited prefix)" : L"",
                   R.info.flags & TEXTP_GUESSED ? L" | Encoding guessed" : L"",
                   R.info.flags & TEXTP_REPLACED ? L" | Invalid characters replaced" : L"",
                   R.lines ? L"Line numbers" : L"No line numbers",
                   textview_selected() ? L"Text selected" : L"Read only");
    s[511] = 0;
    rect.right = R.cw - app_px(12);
    rect.top = app_px(29);
    rect.bottom = app_px(52);
    SetTextColor(dc, FAILED(R.err) ? C_ERROR : C_DIM);
    DrawTextW(dc, s, -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (R.find) {
        if (R.finding)
            wcscpy(s, L"Searching...");
        else if (!R.query_chars)
            wcscpy(s, L"Find in preview");
        else if (!R.match_count)
            wcscpy(s, L"No matches");
        else
            _snwprintf(s, 512, L"%u of %u%ls", R.match_index + 1, R.match_count, R.match_more ? L"+" : L"");
        rect = (RECT){R.cw - app_px(150), app_px(58), R.cw - app_px(8), app_px(80)};
        SetTextColor(dc, C_DIM);
        DrawTextW(dc, s, -1, &rect, DT_SINGLELINE | DT_RIGHT | DT_NOPREFIX);
    }
    if (!R.loaded || R.front < 0 || FAILED(R.err)){textnav_paint(dc);return;}
    const wchar_t *text = content();
    const uint8_t *style = styles();
    SelectObject(dc, R.font);
    static const COLORREF colors[] = {C_TEXT,
                                      RGB(130, 180, 255),
                                      RGB(190, 170, 245),
                                      RGB(205, 165, 110),
                                      RGB(115, 160, 120),
                                      RGB(100, 200, 210)};
    uint32_t a = R.anchor < R.caret ? R.anchor : R.caret, b = R.anchor > R.caret ? R.anchor : R.caret;
    SaveDC(dc);
    IntersectClipRect(dc, 0, head_h(), view_width(), R.ch);
    for (uint32_t k = R.top; k < R.info.rows && k < R.top + page_rows() + 1; ++k) {
        textp_row row = row_at(k);
        int y = head_h() + (int)(k - R.top) * R.line;
        if (R.lines && (k == 0 || row_at(k - 1).line != row.line)) {
            wchar_t num[24];
            _snwprintf(num, 24, L"%u", row.line);
            RECT nr = {0, y, gutter() - R.cell, y + R.line};
            SetTextColor(dc, C_DIM);
            DrawTextW(dc, num, -1, &nr, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
        }
        uint32_t base = column_at(row.start), target = base + R.left, i = at_column(row, target);
        unsigned glyphs = 0;
        while (i < row.end && glyphs < 2048) {
            uint32_t col = column_at(i), match = 0;
            int x = gutter() + ((int64_t)col - base - R.left) * R.cell;
            if (x >= view_width())
                break;
            int selected = i < b && i >= a, found = match_at(i, &match);
            unsigned color = style[i] < sizeof colors / sizeof *colors ? style[i] : 0;
            wchar_t run[256];
            int dx[256];
            unsigned len = 0;
            uint32_t width = 0;
            while (i < row.end && len < 254 && glyphs < 2048) {
                uint32_t units, w = text_cell(text, i, row.end, column_at(i), &units), next_match = 0;
                int sel = i < b && i + units > a, has = match_at(i, &next_match);
                unsigned c = style[i] < sizeof colors / sizeof *colors ? style[i] : 0;
                if (len && (sel != selected || has != found || (has && next_match != match) || c != color))
                    break;
                if (x + (int)width * R.cell >= R.cw)
                    break;
                run[len] = text[i] == '\t' ? L' ' : text[i];
                dx[len++] = (int)w * R.cell;
                if (units == 2) {
                    run[len] = text[i + 1];
                    dx[len++] = 0;
                }
                i += units;
                glyphs += units;
                width += w;
            }
            if (!len)
                break;
            if (selected || found)
                fill(dc, x, y, (int)(width ? width : 1) * R.cell, R.line,
                     selected                 ? C_SEL
                     : match == R.match_index ? RGB(110, 85, 25)
                                              : RGB(65, 60, 34));
            SetTextColor(dc, colors[color]);
            ExtTextOutW(dc, x, y, 0, NULL, run, len, dx);
        }
    }
    RestoreDC(dc, -1);
    textnav_paint(dc);
}
static void find_place(void)
{
    if (R.find)
        SetWindowPos(R.find, NULL, app_px(10), app_px(57),
                     R.cw > app_px(185) ? R.cw - app_px(175) : app_px(10), app_px(22),
                     SWP_NOZORDER | SWP_NOACTIVATE);
}
static void choose_match(int step)
{
    if (!R.match_count)
        return;
    if (step > 0)
        R.match_index = (R.match_index + 1) % R.match_count;
    else if (step < 0)
        R.match_index = (R.match_index + R.match_count - 1) % R.match_count;
    uint32_t at = R.matches[R.match_index];
    selection(at, at + R.query_chars);
    reveal(at);
}
static LRESULT CALLBACK find_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_KEYDOWN && w == VK_TAB) {
        app_focus_window(R.hwnd);
        return 0;
    }
    if (m == WM_KEYDOWN && (w == VK_ESCAPE || w == VK_RETURN)) {
        if (w == VK_ESCAPE)
            find_close();
        else
            choose_match(GetKeyState(VK_SHIFT) < 0 ? -1 : 1);
        return 0;
    }
    if (m == WM_CHAR && (w == '\r' || w == 27 || w == '\t'))
        return 0;
    return CallWindowProcW(R.find_orig, h, m, w, l);
}
static void find_open(void)
{
    if (!R.find) {
        R.find = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, R.hwnd,
                                 NULL, GetModuleHandleW(NULL), NULL);
        if (!R.find)
            return;
        SendMessageW(R.find, WM_SETFONT, (WPARAM)app_font(), FALSE);
        SendMessageW(R.find, EM_SETLIMITTEXT, TEXTP_QUERY_MAX - 1, 0);
        R.find_orig = (WNDPROC)SetWindowLongPtrW(R.find, GWLP_WNDPROC, (LONG_PTR)find_proc);
        find_place();
        if (R.loaded) {
            R.restore_at = row_at(R.top).start;
            R.want_layout = 1;
            request_work();
        }
    }
    app_focus_window(R.find);
    SendMessageW(R.find, EM_SETSEL, 0, -1);
    InvalidateRect(R.hwnd, NULL, FALSE);
}
static void find_close(void)
{
    if (!R.find)
        return;
    KillTimer(R.hwnd, TIMER_FIND);
    DestroyWindow(R.find);
    R.find = NULL;
    R.find_gen++;
    R.query_chars = R.match_count = R.finding = 0;
    R.query[0] = 0;
    app_focus_window(R.hwnd);
    if (R.loaded) {
        R.restore_at = row_at(R.top).start;
        R.want_layout = 1;
        request_work();
    }
    view_changed();
}
static void search_start(void)
{
    if (!R.find || !R.loaded)
        return;
    GetWindowTextW(R.find, R.query, TEXTP_QUERY_MAX);
    R.query_chars = (uint32_t)wcslen(R.query);
    R.match_count = 0;
    R.finding = !!R.query_chars;
    R.find_gen++;
    uint8_t payload[sizeof(textp_find) + TEXTP_QUERY_MAX * 2];
    textp_find p = {R.find_gen, R.query_chars};
    memcpy(payload, &p, sizeof p);
    memcpy(payload + sizeof p, R.query, (R.query_chars + 1) * 2);
    send_req(TEXTP_FIND, R.doc, 0, payload, sizeof p + (R.query_chars + 1) * 2);
    InvalidateRect(R.hwnd, NULL, FALSE);
}
static void clipboard_publish(void)
{
    if (!R.clipboard)
        return;
    if (GetClipboardSequenceNumber() != R.clipboard_sequence) {
        GlobalFree(R.clipboard);
        R.clipboard = NULL;
        KillTimer(R.hwnd, TIMER_CLIPBOARD);
        app_status(NULL, 1, L"Text copy cancelled because the clipboard changed");
        return;
    }
    if (OpenClipboard(R.hwnd)) {
        EmptyClipboard();
        int ok = SetClipboardData(CF_UNICODETEXT, R.clipboard) != NULL;
        CloseClipboard();
        if (!ok)
            GlobalFree(R.clipboard);
        R.clipboard = NULL;
        KillTimer(R.hwnd, TIMER_CLIPBOARD);
        app_status(NULL, !ok, ok ? L"Copied selected text" : L"Text could not be copied");
    } else if (GetTickCount64() >= R.clipboard_until) {
        GlobalFree(R.clipboard);
        R.clipboard = NULL;
        KillTimer(R.hwnd, TIMER_CLIPBOARD);
        app_status(NULL, 1, L"The clipboard is busy; try copying again");
    } else
        SetTimer(R.hwnd, TIMER_CLIPBOARD, 40, NULL);
}
static void copy_result(const textp_copied *p)
{
    if (p->bank >= TEXTP_BANKS || p->bytes < 2 || p->bytes > TEXTP_BANK_BYTES || (p->bytes & 1))
        return;
    const uint8_t *src = R.pixels + (size_t)p->bank * TEXTP_BANK_BYTES;
    if (*(const uint16_t *)(src + p->bytes - 2))
        return;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, p->bytes);
    if (!h) {
        app_status(NULL, 1, L"Not enough memory to copy text");
        return;
    }
    void *dst = GlobalLock(h);
    if (!dst) {
        GlobalFree(h);
        return;
    }
    memcpy(dst, src, p->bytes);
    GlobalUnlock(h);
    if (R.clipboard)
        GlobalFree(R.clipboard);
    R.clipboard = h;
    R.clipboard_until = GetTickCount64() + 1500;
    clipboard_publish();
}

void textview_action(int action)
{
    if (!R.hwnd || !R.path)
        return;
    if (binary_preview() && action != TEXTVIEW_OPEN) return;
    if (R.markdown && R.markdown_rendered && action != TEXTVIEW_OPEN) markdown_mode(0);
    if (action == TEXTVIEW_OPEN) {
        if(!fpe_wsl_requires_native(R.path))ShellExecuteW(app_window(),L"open",R.path,NULL,NULL,SW_SHOWNORMAL);
        return;
    }
    if (action == TEXTVIEW_FIND) {
        find_open();
        return;
    }
    if (action == TEXTVIEW_NEXT || action == TEXTVIEW_PREV) {
        if (!R.find)
            find_open();
        else
            choose_match(action == TEXTVIEW_NEXT ? 1 : -1);
        return;
    }
    if (action == TEXTVIEW_ENCODING) {
        uint32_t encoding = (R.encoding + 1) % (TEXT_1252 + 1);
        textview_configure(R.limit_mb, R.wrap, R.lines, encoding);
        return;
    }
    if (action == TEXTVIEW_WRAP || action == TEXTVIEW_LINES) {
        if (R.loaded)
            R.restore_at = row_at(R.top).start;
        if (action == TEXTVIEW_WRAP)
            R.wrap = !R.wrap;
        else
            R.lines = !R.lines;
        R.want_layout = 1;
        R.left = 0;
        request_work();
        view_changed();
        return;
    }
    if (!R.loaded)
        return;
    if (action == TEXTVIEW_SELECT_ALL) {
        selection(0, R.info.chars);
        view_changed();
        return;
    }
    if (action == TEXTVIEW_COPY && textview_selected() && !R.copying && !R.clipboard) {
        R.clipboard_sequence = GetClipboardSequenceNumber();
        R.copy_start = R.anchor < R.caret ? R.anchor : R.caret;
        R.copy_end = R.anchor > R.caret ? R.anchor : R.caret;
        R.copying = 2;
        app_status(NULL, 0, L"Copying selected text...");
        request_work();
    }
}
static void key(WPARAM vk, int sys)
{
    int ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if(!sys&&ctrl&&vk=='D'){if(R.markdown_rendered&&R.markdown_hwnd)PostMessageW(R.markdown_hwnd,WM_APP+(shift?91:90),0,0);else if(shift)textnav_menu(1);else textnav_toggle();return;}
    if (!sys && shift && R.loaded &&
        (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_HOME || vk == VK_END)) {
        uint32_t at = R.caret;
        const wchar_t *s = content();
        if (vk == VK_LEFT && at) {
            --at;
            if (at && s[at] >= 0xDC00 && s[at] <= 0xDFFF)
                --at;
        } else if (vk == VK_RIGHT && at < R.info.chars) {
            if (s[at] >= 0xD800 && s[at] <= 0xDBFF && at + 1 < R.info.chars)
                at += 2;
            else
                ++at;
        } else if (vk == VK_HOME)
            at = ctrl ? 0 : row_at(row_for(at)).start;
        else if (vk == VK_END)
            at = ctrl ? R.info.chars : row_at(row_for(at)).end;
        else if (vk == VK_UP || vk == VK_DOWN) {
            uint32_t current = row_for(at), next = current;
            if (vk == VK_UP && next)
                --next;
            if (vk == VK_DOWN && next + 1 < R.info.rows)
                ++next;
            textp_row from = row_at(current), to = row_at(next);
            uint32_t c = column_at(at) - column_at(from.start);
            at = at_column(to, column_at(to.start) + c);
            if (column_at(to.start) + c >= column_at(to.end))
                at = to.end;
        }
        R.caret = at;
        reveal(at);
        return;
    }
    if (!sys && ctrl && (vk == VK_HOME || vk == VK_END)) {
        R.anchor = R.caret = vk == VK_HOME ? 0 : R.info.chars;
        reveal(R.caret);
        return;
    }
    if (!sys && !ctrl) {
        int delta = 0;
        switch (vk) {
        case VK_ESCAPE:
            if (textview_selected()) {
                selection(R.caret, R.caret);
                view_changed();
            } else if (R.find)
                find_close();
            else
                app_preview_close();
            return;
        case VK_TAB:
            app_focus_cycle(shift ? -1 : 1);
            return;
        case VK_UP:
            delta = -1;
            break;
        case VK_DOWN:
            delta = 1;
            break;
        case VK_PRIOR:
            delta = -(int)page_rows();
            break;
        case VK_NEXT:
        case VK_SPACE:
            delta = (int)page_rows() * (shift ? -1 : 1);
            break;
        case VK_LEFT:
            if (R.left)
                R.left--;
            view_changed();
            return;
        case VK_RIGHT:
            R.left++;
            view_changed();
            return;
        case VK_HOME:
            R.top = R.left = 0;
            view_changed();
            return;
        case VK_END:
            R.top = R.info.rows;
            view_changed();
            return;
        }
        if (delta) {
            if (delta < 0)
                R.top = R.top < (uint32_t)-delta ? 0 : R.top + delta;
            else
                R.top += delta;
            view_changed();
            return;
        }
    }
    fn_ctx c;
    app_preview_ctx(&c, R.hwnd, FN_FROM_KEY);
    uint8_t mods =
        (ctrl ? FN_CTRL : 0) | (shift ? FN_SHIFT : 0) | (sys || GetKeyState(VK_MENU) < 0 ? FN_ALT : 0);
    fn_key((uint8_t)vk, mods, &c);
    app_ctx_free(&c);
}
static LRESULT CALLBACK reader_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if(textnav_input(h,m,w,l))return 0;
    switch (m) {
    case WM_CREATE:
        SetWindowTheme(h, L"DarkMode_Explorer", NULL);
        return 0;
    case WM_SIZE:
        R.cw = LOWORD(l);
        R.ch = HIWORD(l);
        markdown_place();
        find_place();
        if (R.loaded) {
            R.restore_at = row_at(R.top).start;
            R.want_layout = 1;
            request_work();
        }
        view_changed();
        return 0;
    case WM_MARKDOWN_READY: {
        DWORD pid = 0; HWND child = (HWND)w;
        GetWindowThreadProcessId(child, &pid);
        if (R.markdown_process && pid == R.markdown_pid && GetParent(child) == h) {
            R.markdown_hwnd = child; markdown_place();
        }
        return 0;
    }
    case WM_MARKDOWN_FAILED:
        { DWORD pid = 0; GetWindowThreadProcessId((HWND)w, &pid);
        if (R.markdown_process && pid == R.markdown_pid) {
            markdown_stop(); R.markdown_rendered = 0; if (binary_preview()) R.err = E_FAIL; view_changed();
        }
        return 0; }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        int64_t t = fpe_prof_begin();
        if (!R.bdc)
            R.bdc = CreateCompatibleDC(dc);
        if (!R.bmp || R.bw != R.cw || R.bh != R.ch) {
            if (R.bmp) {
                SelectObject(R.bdc, R.old_bmp);
                DeleteObject(R.bmp);
            }
            R.bmp = CreateCompatibleBitmap(dc, R.cw > 0 ? R.cw : 1, R.ch > 0 ? R.ch : 1);
            R.old_bmp = SelectObject(R.bdc, R.bmp);
            R.bw = R.cw;
            R.bh = R.ch;
        }
        paint(R.bdc);
        BitBlt(dc, 0, 0, R.cw, R.ch, R.bdc, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        FPE_PROF_END("text.paint", t);
        return 0;
    }
    case WM_TEXT_LAUNCHED:
        if ((uint32_t)w != R.launch_gen || !R.launcher) return 0;
        preview_launch_take(&R.launcher, &R.process, &R.job, &R.pid);
        if (!R.process) {
            helper_stop();
            R.gone = 1;
            if (!binary_preview()) R.err = E_FAIL;
            view_changed();
        }
        return 0;
    case WM_TEXT_GONE:
        if ((uint32_t)w != R.launch_gen) return 0;
        if (!R.gone) {
            helper_stop();
            R.loaded = 0;
            R.finding = R.copying = 0;
            R.gone = 1;
            if (!binary_preview()) R.err = E_FAIL;
            KillTimer(h, TIMER_WATCHDOG);
            view_changed();
        }
        return 0;
    case WM_TEXT_MSG: {
        msg *mm = (msg *)l;
        if ((uint32_t)w != R.launch_gen) { free(mm); return 0; }
        R.started = GetTickCount64();
        if (mm->head.type == TEXTP_READY) {
            textp_info info;
            memcpy(&info, mm->data, sizeof info);
            if (info.bank < TEXTP_BANKS && R.banks[info.bank].busy &&
                R.banks[info.bank].ticket == mm->head.ticket) {
                int kind = R.banks[info.bank].kind;
                R.banks[info.bank].busy = 0;
                if (mm->head.doc == R.doc) {
                    R.err = (HRESULT)info.err;
                    if (SUCCEEDED(R.err) && info.chars <= TEXTP_MAX_CHARS && info.rows &&
                        info.rows <= info.chars + 1 &&
                        textp_snapshot_bytes(info.chars, info.rows) <= TEXTP_BANK_BYTES) {
                        R.front = info.bank;
                        R.info = info;
                        R.loaded = 1;
                        R.top = row_for(R.restore_at);
                        apply_location();
                        if (kind == 1 && R.find)
                            search_start();
                    } else {
                        if (SUCCEEDED(R.err))
                            R.err = E_FAIL;
                        R.info = info;
                        R.loaded = 0;
                    }
                    view_changed();
                }
                request_work();
            }
        } else if (mm->head.type == TEXTP_COPIED) {
            textp_copied p;
            memcpy(&p, mm->data, sizeof p);
            if (p.bank < TEXTP_BANKS && R.banks[p.bank].busy && R.banks[p.bank].ticket == mm->head.ticket) {
                R.banks[p.bank].busy = 0;
                if (mm->head.doc == R.doc) {
                    R.copying = 0;
                    if (SUCCEEDED((HRESULT)p.err))
                        copy_result(&p);
                    else
                        app_status(NULL, 1, L"Text could not be copied");
                }
                request_work();
            }
        } else if (mm->head.type == TEXTP_FOUND && mm->head.doc == R.doc) {
            textp_found p;
            memcpy(&p, mm->data, sizeof p);
            if (p.gen == R.find_gen && p.chars == R.query_chars && p.count <= TEXTP_MATCH_MAX &&
                mm->head.size == sizeof p + p.count * 4) {
                R.finding = 0;
                if (SUCCEEDED((HRESULT)p.err)) {
                    uint32_t *hits = (uint32_t *)(mm->data + sizeof p);
                    int valid = 1;
                    for (uint32_t i = 0; i < p.count; ++i)
                        if (hits[i] > R.info.chars || p.chars > R.info.chars - hits[i] ||
                            (i && hits[i] <= hits[i - 1]))
                            valid = 0;
                    if (valid) {
                        R.match_count = p.count;
                        R.match_more = p.more;
                        memcpy(R.matches, hits, p.count * 4);
                        R.match_index = 0;
                        choose_match(0);
                    }
                }
                title_update();
                InvalidateRect(h, NULL, FALSE);
            }
        }
        free(mm);
        return 0;
    }
    case WM_COMMAND:
        if ((HWND)l == R.find && HIWORD(w) == EN_CHANGE) {
            R.find_gen++;
            R.finding = 1;
            R.match_count = 0;
            SetTimer(h, TIMER_FIND, 150, NULL);
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_CTLCOLOREDIT:
        if ((HWND)l == R.find) {
            HDC dc = (HDC)w;
            SetBkColor(dc, C_EDIT);
            SetTextColor(dc, C_TEXT);
            return (LRESULT)R.find_brush;
        }
        break;
    case WM_TIMER:
        if (w == TIMER_MARKDOWN) {
            DWORD exit_code = 0;
            if (R.markdown_process && ((!R.markdown_hwnd && ++R.markdown_wait > 30) || !GetExitCodeProcess(R.markdown_process, &exit_code) || exit_code != STILL_ACTIVE)) {
                markdown_stop(); R.markdown_rendered = 0; if (binary_preview()) R.err = E_FAIL; view_changed();
                app_status(NULL, 1, R.markdown == 6 ? L"Database preview unavailable" : R.markdown == 8 ? L"Spreadsheet preview unavailable" : R.markdown == 9 ? L"Word preview unavailable" : L"Enhanced preview unavailable; showing source");
            }
            return 0;
        }
        if (w == TIMER_CLIPBOARD) {
            clipboard_publish();
            return 0;
        }
        if (w == TIMER_FIND) {
            KillTimer(h, w);
            search_start();
            return 0;
        }
        if (w == TIMER_WATCHDOG) {
            int busy = !R.loaded || R.finding;
            for (unsigned i = 0; i < TEXTP_BANKS; ++i)
                busy |= R.banks[i].busy;
            if (busy && GetTickCount64() - R.started > 15000) {
                R.front = -1;
                R.loaded = 0;
                helper_stop();
                R.gone = 1;
                R.err = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                KillTimer(h, w);
                view_changed();
            } else if (!busy)
                R.started = GetTickCount64();
            return 0;
        }
        if (w == TIMER_EDGE && R.selecting) {
            if (R.mouse.y < head_h()) {
                if (R.top)
                    R.top--;
            } else if (R.mouse.y >= R.ch)
                R.top++;
            clamp_view();
            R.caret = hit(R.mouse.x, R.mouse.y);
            view_changed();
            return 0;
        }
        break;
    case WM_VSCROLL: {
        SCROLLINFO si = {sizeof si, SIF_TRACKPOS};
        GetScrollInfo(h, SB_VERT, &si);
        switch (LOWORD(w)) {
        case SB_LINEUP:
            if (R.top)
                R.top--;
            break;
        case SB_LINEDOWN:
            R.top++;
            break;
        case SB_PAGEUP:
            R.top = R.top > page_rows() ? R.top - page_rows() : 0;
            break;
        case SB_PAGEDOWN:
            R.top += page_rows();
            break;
        case SB_THUMBTRACK:
            R.top = si.nTrackPos;
            break;
        case SB_TOP:
            R.top = 0;
            break;
        case SB_BOTTOM:
            R.top = R.info.rows;
            break;
        }
        view_changed();
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int steps = (short)HIWORD(w) / 120;
        if (LOWORD(w) & MK_SHIFT) {
            int v = (int)R.left - steps * 4;
            R.left = v > 0 ? v : 0;
        } else {
            int v = (int)R.top - steps * 3;
            R.top = v > 0 ? v : 0;
        }
        view_changed();
        return 0;
    }
    case WM_MOUSEHWHEEL: {
        int v = (int)R.left + (short)HIWORD(w) / 120 * 4;
        R.left = v > 0 ? v : 0;
        view_changed();
        return 0;
    }
    case WM_LBUTTONDOWN:
        app_focus_window(h);
        if (R.markdown && !binary_preview() && GET_Y_LPARAM(l) < app_px(28) && GET_X_LPARAM(l) >= R.cw - app_px(192) && GET_X_LPARAM(l) < R.cw-app_px(32)) {
            markdown_mode(GET_X_LPARAM(l) < R.cw - app_px(114)); return 0;
        }
        if (R.loaded && GET_Y_LPARAM(l) >= head_h()) {
            uint32_t at = hit(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            if (!(w & MK_SHIFT))
                R.anchor = at;
            R.caret = at;
            R.selecting = 1;
            R.mouse = (POINT){GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            SetCapture(h);
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        if (R.loaded) {
            uint32_t at = hit(GET_X_LPARAM(l), GET_Y_LPARAM(l)), a = at, b = at;
            const wchar_t *s = content();
            while (a && (iswalnum(s[a - 1]) || s[a - 1] == '_'))
                --a;
            while (b < R.info.chars && (iswalnum(s[b]) || s[b] == '_'))
                ++b;
            selection(a, b);
            view_changed();
        }
        return 0;
    case WM_MOUSEMOVE:
        if (R.selecting) {
            R.mouse = (POINT){GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            R.caret = hit(R.mouse.x, R.mouse.y);
            if (R.mouse.y < head_h() || R.mouse.y >= R.ch)
                SetTimer(h, TIMER_EDGE, 40, NULL);
            else
                KillTimer(h, TIMER_EDGE);
            view_changed();
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        R.selecting = 0;
        KillTimer(h, TIMER_EDGE);
        if (m == WM_LBUTTONUP)
            ReleaseCapture();
        return 0;
    case WM_KEYDOWN:
        {HWND previous=key_receiver;key_receiver=h;key(w,0);key_receiver=previous;}
        return 0;
    case WM_SYSKEYDOWN:
        {HWND previous=key_receiver;key_receiver=h;key(w,1);key_receiver=previous;}
        return 0;
    case WM_CHAR:
    case WM_SYSCHAR:
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_CONTEXTMENU: {
        app_focus_window(h);
        fn_ctx c;
        app_preview_ctx(&c, R.hwnd, FN_FROM_MENU);
        if (!app_menu_context_current(&c)) { app_ctx_free(&c); return 0; }
        HMENU menu = CreatePopupMenu();
        fn_menu(menu, FN_MENU_TEXT, &c);
        POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (l == -1) {
            pt = (POINT){gutter(), head_h()};
            ClientToScreen(h, &pt);
        }
        UINT id = menu_track(menu, h, pt);
        DestroyMenu(menu);
        if (id && app_menu_context_current(&c))
            fn_menu_run(id, &c);
        app_ctx_free(&c);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}
int textview_start(HWND parent)
{
    if (R.hwnd)
        return 0;
    memset(&R, 0, sizeof R);
    R.front = -1;
    R.nav_file=docmarks_location(NULL,L"preview-bookmarks.fwb");
    R.limit_mb = 4;
    R.wrap = R.lines = 1;
    R.cell = 8;
    R.line = 18;
    R.find_brush = CreateSolidBrush(C_EDIT);
    WNDCLASSW cls = {0};
    cls.hInstance = GetModuleHandleW(NULL);
    cls.lpszClassName = L"OpenPilotText";
    cls.lpfnWndProc = reader_proc;
    cls.hCursor = LoadCursorW(NULL, IDC_IBEAM);
    cls.style = CS_DBLCLKS;
    RegisterClassW(&cls);
    R.font =
        CreateFontW(-app_px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    HDC dc = GetDC(parent);
    HGDIOBJ old = SelectObject(dc, R.font);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    R.cell = tm.tmAveCharWidth;
    R.line = tm.tmHeight + app_px(3);
    SelectObject(dc, old);
    ReleaseDC(parent, dc);
    R.hwnd = CreateWindowExW(0, cls.lpszClassName, L"Text preview", WS_CHILD | WS_CLIPSIBLINGS,
                             0, 0, 0, 0, parent, NULL, cls.hInstance, NULL);
    if (!R.hwnd || !R.font) {
        textview_stop();
        return -1;
    }
    return 0;
}
HWND textview_window(void) { return R.hwnd; }
void textview_clear(void)
{
    markdown_stop(); R.markdown = R.markdown_rendered = R.archived = 0;
    if (R.to)
        send_req(TEXTP_CLOSE, ++R.doc, 0, NULL, 0);
    else
        ++R.doc;
    if (R.clipboard) {
        GlobalFree(R.clipboard);
        R.clipboard = NULL;
    }
    if (R.hwnd) {
        KillTimer(R.hwnd, TIMER_CLIPBOARD);
        KillTimer(R.hwnd, TIMER_FIND);
        KillTimer(R.hwnd, TIMER_WATCHDOG);
        KillTimer(R.hwnd, TIMER_EDGE);
    }
    R.front = -1;
    R.loaded = R.want_open = R.want_layout = R.copying = R.selecting = 0;
    R.top = R.left = R.anchor = R.caret = R.restore_at = 0;
    R.jump_doc=R.jump_line=R.jump_column=0;R.jump_expected[0]=0;
    if(GetCapture()==R.hwnd)ReleaseCapture();
    R.match_count = R.finding = 0;R.nav_count=0;R.nav_loaded=0;R.nav_targets_count=0;memset(&R.rail,0,sizeof R.rail);
    R.find_gen++;
    memset(&R.info, 0, sizeof R.info);
    free(R.path);
    R.path = NULL;free(R.nav_identity);free(R.nav_key);R.nav_identity=R.nav_key=NULL;
    R.err = S_OK;
    if (R.hwnd)
        view_changed();
}
void textview_stop(void)
{
    textview_clear();
    helper_stop();
    if (R.hwnd)
        DestroyWindow(R.hwnd);
    if (R.find_brush)
        DeleteObject(R.find_brush);
    if (R.bdc) {
        SelectObject(R.bdc, R.old_bmp);
        if (R.bmp)
            DeleteObject(R.bmp);
        DeleteDC(R.bdc);
    }
    if (R.font)
        DeleteObject(R.font);
    free(R.nav_file);
    memset(&R, 0, sizeof R);
}
static void textview_open_impl(const wchar_t *path, uint64_t bytes, int64_t mtime, int archived,const wchar_t *identity)
{
    if (!R.hwnd || !path || wcslen(path) >= TEXTP_PATH_MAX)
        return;
    if (R.path && fpe_path_equal(R.path, path) && R.size == bytes && R.mtime == mtime && R.archived == archived && R.nav_identity && fpe_path_equal(R.nav_identity,identity?identity:archived?L"":path) && !R.gone)
        return;
    textview_clear();
    if (R.gone)
        helper_stop();
    R.path = _wcsdup(path);
    R.nav_identity=_wcsdup(identity?identity:archived?L"":path);R.nav_key=docmarks_identity(R.nav_identity,L"source");
    R.size = bytes;
    R.mtime = mtime;
    R.archived = archived;
    const wchar_t *extension = wcsrchr(path, L'.');
    R.markdown = extension && (!_wcsicmp(extension, L".md") || !_wcsicmp(extension, L".markdown") || !_wcsicmp(extension, L".mdown"));
    if (extension && (!_wcsicmp(extension, L".json") || !_wcsicmp(extension, L".jsonl") || !_wcsicmp(extension, L".ndjson"))) R.markdown = 2;
    if (extension && (!_wcsicmp(extension, L".csv") || !_wcsicmp(extension, L".tsv"))) R.markdown = 3;
    if (extension && !_wcsicmp(extension, L".ipynb")) R.markdown = 4;
    if (extension && (!_wcsicmp(extension, L".yaml") || !_wcsicmp(extension, L".yml") || !_wcsicmp(extension, L".toml") || !_wcsicmp(extension, L".ini"))) R.markdown = 5;
    if (extension && (!_wcsicmp(extension, L".db") || !_wcsicmp(extension, L".db3") || !_wcsicmp(extension, L".sqlite") || !_wcsicmp(extension, L".sqlite3") || !_wcsicmp(extension, L".sqlitedb"))) R.markdown = 6;
    if (extension && (!_wcsicmp(extension, L".xml") || !_wcsicmp(extension, L".xaml") || !_wcsicmp(extension, L".rss") || !_wcsicmp(extension, L".atom"))) R.markdown = 7;
    if (extension && (!_wcsicmp(extension, L".xlsx") || !_wcsicmp(extension, L".xlsm"))) R.markdown = 8;
    if (extension && (!_wcsicmp(extension, L".docx") || !_wcsicmp(extension, L".docm"))) R.markdown = 9;
    if(fpe_wsl_requires_native(path)){R.err=HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);view_changed();return;}
    if (binary_preview()) {
        if (R.find) find_close();
        if (R.path) markdown_mode(1); else R.err = E_OUTOFMEMORY;
        view_changed();
        return;
    }
    if (!R.path || helper_start()) {
        R.err = E_FAIL;
        view_changed();
        return;
    }
    if (R.markdown) markdown_mode(1);
    R.want_open = 1;
    R.started = GetTickCount64();
    SetTimer(R.hwnd, TIMER_WATCHDOG, 500, NULL);
    request_work();
    view_changed();
}
void textview_open(const wchar_t *path, uint64_t bytes, int64_t mtime) { textview_open_impl(path, bytes, mtime, 0,NULL); }
void textview_open_archive(const wchar_t *path, uint64_t bytes, int64_t mtime) { textview_open_impl(path, bytes, mtime, 1,NULL); }
void textview_configure(uint32_t limit, int wrap, int lines, uint32_t encoding)
{
    if (limit < 1)
        limit = 1;
    if (limit > 8)
        limit = 8;
    if (encoding > TEXT_1252)
        encoding = TEXT_AUTO;
    int reload = R.path && (limit != R.limit_mb || encoding != R.encoding);
    R.limit_mb = limit;
    R.encoding = encoding;
    R.wrap = !!wrap;
    R.lines = !!lines;
    if (reload) {
        wchar_t *path = _wcsdup(R.path), *identity=R.nav_identity?_wcsdup(R.nav_identity):NULL;
        uint64_t bytes = R.size;
        int64_t mtime = R.mtime;
        int archived = R.archived;
        if (path) {
            textview_clear();
            textview_open_impl(path, bytes, mtime, archived,identity);
            free(path);
        }
        free(identity);
    } else if (R.loaded) {
        R.restore_at = row_at(R.top).start;
        R.want_layout = 1;
        request_work();
        view_changed();
    }
}

void textview_open_archive_key(const wchar_t *path,uint64_t bytes,int64_t mtime,const wchar_t *identity){textview_open_impl(path,bytes,mtime,1,identity);}
void textview_set_bookmark_memory(const wchar_t *reference){free(R.nav_file);R.nav_file=docmarks_location(reference,L"preview-bookmarks.fwb");}
