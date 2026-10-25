/*
 * panelproto.h - what the side panel (app/sidepanel.c) and a panel app say
 * to each other over the app's stdin/stdout.
 *
 * A panel app is a separate program shown in the side panel. It draws into
 * two shared bitmaps (hello.section: top-down 32-bit, w * h each, buffer b
 * at b * w * h * 4) and the explorer shows them and forwards input. Nothing
 * is shared but memory, so a hung or crashed app can't stall the explorer
 * (docs/side-panel-experiment.md measured why it isn't a child window).
 * The app draws into the buffer the host isn't showing, sends FRAME naming
 * it, and writes to the other one only after FRAME_ACK.
 * Every message is a header and `size` payload bytes.
 */
#ifndef PANELPROTO_H
#define PANELPROTO_H

#include <stdint.h>

#define PANELP_MAGIC   0x4C4E5050u /* "PPNL" */
#define PANELP_VERSION 8u
#define PANELP_MAX     (4u * 1024 * 1024)

enum {
    /* host -> app */
    PANELP_HELLO = 1,  /* panelp_hello */
    PANELP_RESIZE,     /* panelp_size: a new section comes with every size */
    PANELP_INPUT,      /* panelp_input */
    PANELP_FOCUS,      /* uint32_t: 1 got focus, 0 lost it */
    PANELP_FRAME_ACK,  /* the host shows the latest frame; the other buffer is free */
    PANELP_PING,       /* uint32_t sequence; the app answers PONG from its UI thread */
    PANELP_QUIT,
    PANELP_SETTINGS,   /* settings app: UTF-8 text, "rev" and "file" lines, then fn_describe (app/fn.h) */
    PANELP_SAVED,      /* settings app: the answer to a SET, "req\tstatus\tmessage";
                        * status ok | invalid | conflict | failed. A SETTINGS with what now applies comes first. */
    PANELP_SHOW,       /* uint32_t: 1 shown, 0 hidden. A hidden app keeps running; its frames are taken
                        * at once and not shown, so it needn't draw until shown again. Apps start shown. */
    /* terminal app (fpterm): UTF-8 text fields; paths use pathwire.h */
    PANELP_TERM_OPEN,   /* a new session: dir16, shell (auto|pwsh|powershell|cmd), cmd, title */
    PANELP_TERM_PANE,   /* active pane's folder (exact pathwire): an idle shell goes there */
    PANELP_TERM_CONFIG, /* font, size (pt), scrollback (lines), history (sessions), follow (on|off: shells follow the pane) */
    PANELP_OP_RESET,    /* operations: discard old snapshots (host immediately replays all records) */
    PANELP_OP_UPDATE,   /* ops_head then four UTF-16 strings, app/operations.h */
    PANELP_OP_SELECT,   /* uint64_t token: show the newest/requested job; repeated selection preserves input */
    PANELP_RELAY_CONTEXT, /* request-id, count, flags; UTF-16 dir then count paths, all terminated */
    PANELP_RELAY_BROWSED, /* request-id then UTF-16 chosen program (empty: cancelled) */
    PANELP_RELAY_OPEN,    /* uint32_t 1: open a new custom-command form */
    /* app -> host */
    PANELP_READY = 100, /* panelp_ready */
    PANELP_FRAME,       /* panelp_frame */
    PANELP_CURSOR,      /* uint32_t: an IDC_* resource number */
    PANELP_CARET,       /* panelp_rect: where text is typed, for the IME window */
    PANELP_PONG,        /* uint32_t sequence */
    PANELP_CLOSE,       /* the app asks the host to close the panel */
    PANELP_RUN,         /* a function id (UTF-8, no terminator) to run; the host decides whether it may */
    /* settings app: change settings together, UTF-8: "req\trev" then a line per change,
     * "section\tkey\tset|reset\tvalue". rev is the SETTINGS it saw: if the file changed
     * since, nothing is written and the answer is conflict. All of them are written, or none. */
    PANELP_SET,
    PANELP_CAPTURE,     /* uint32_t 1: every key comes as INPUT, system keys (WM_SYSKEYDOWN) and the
                         * panel's own included, until 0 or the panel loses focus (recording a key) */
    PANELP_OP_ACTION,   /* ops_action: Start(token, policy), Cancel(token), Clear completed */
    PANELP_RELAY_SAVE,  /* relay_draft, app/relayedit.h; reply PANELP_SAVED, snapshot first */
    PANELP_RELAY_GET_CONTEXT, /* uint32_t request-id: current pane folder and selected paths */
    PANELP_RELAY_BROWSE, /* uint32_t request-id: native program picker, answer BROWSED */
    PANELP_TERM_CWD,    /* terminal app: shell changed folder (exact pathwire) */
};

typedef struct panelp_head {
    uint32_t type, size;
} panelp_head;

typedef struct panelp_hello {
    uint32_t magic, version;
    uint32_t dpi;
    uint32_t w, h;
    uint32_t seq;     /* the section's number, for panelp_frame.seq */
    uint64_t section; /* file mapping handle, already in the app's handle table */
    uint32_t host_pid;
    uint32_t pad;
} panelp_hello;

typedef struct panelp_size {
    uint32_t w, h, dpi;
    uint32_t seq;     /* the new section's number */
    uint64_t section; /* the new mapping, in the app's handle table; the old one goes */
} panelp_size;

typedef struct panelp_input {
    uint32_t msg;    /* WM_* */
    uint32_t wparam; /* as the host got it (wheel: delta in the high word) */
    int32_t  x, y;   /* client coordinates in the app's area (mouse) */
    int32_t  sx, sy; /* screen coordinates (mouse) */
    uint32_t lparam; /* keys: as the host got it */
    uint32_t mods;   /* PANELP_M_* */
} panelp_input;

#define PANELP_M_CTRL  0x1u
#define PANELP_M_SHIFT 0x2u
#define PANELP_M_ALT   0x4u

typedef struct panelp_ready {
    uint32_t pid, pad;
    wchar_t  title[64];
} panelp_ready;

typedef struct panelp_rect {
    int32_t x, y, w, h;
} panelp_rect;

typedef struct panelp_frame {
    uint32_t buffer; /* 0 or 1 */
    uint32_t seq;    /* the section it was drawn in: frames of an older one are dropped */
    panelp_rect dirty;
} panelp_frame;

#endif
