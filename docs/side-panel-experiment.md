# Side panel experiment: native child window or shared pixels?

> Outcome: **pixels** was chosen and native mode was removed. The test app and the script used below
> (`tools/fppanel_test.c`, `tests/panel_experiment.ps1`) are in commit `8f7f8f7`.

The side panel shows a separate program (a *panel app*) at the right edge of the window: first the settings, later
tools such as a space analyzer. The app runs in its own process so a bug in it can't take the explorer down. There
are two ways to put its UI on screen:

- **native**: the app creates a real window inside the panel (a child of the explorer's window). Its controls,
  focus, text input, accessibility and drag and drop all work as usual. The catch is that Windows then joins the
  two programs' input handling.
- **pixels**: the app draws into shared memory, the explorer shows the picture and forwards mouse and keys. The two
  share nothing but memory.

Both are built (`app/sidepanel.c`, protocol in `app/panelproto.h`) and measured with a test app
(`tools/fppanel_test.c`: a text field, buttons, a list with a right-click menu, drag out, drop in, clipboard, a
5-second hang on F9 and a crash on F10). `tests/panel_experiment.ps1` runs everything below; the switch in the
panel's header flips the mode by hand.

## Results

Release build, 96 dpi, the app's area 415 × 667 px. Times in ms; "hung" means the panel app's UI thread was blocked for 5 s.

| | native | pixels |
|---|---|---|
| open the panel (explorer's UI thread) | 16 avg, 37 worst | 12 avg, 33 worst |
| ... of which the first focus of the new window (once) | 23 | 24 |
| app's first picture after opening | 71 | 41 |
| typed character to screen | direct | 1.6 avg, 2.3 worst |
| resize to the app's picture at the new size | direct | ~4.5 |
| explorer answers messages while the app is hung | 1.4 worst | 0.5 worst |
| key to a file pane while hung | 0.6 | 0.6 |
| **resize the window while hung** | **3,479 before the fix**, 24 after | 16 |
| **minimize and restore while hung** | **3,188** | 79 |
| move focus from the app to a pane while hung | 5 | 3 |
| **real key typed into the app, then a real click on a pane, while hung** (`-RealInput`) | **4,680** | 12 |
| close the panel while hung | up to 505 (until the app is ended) | 9 |
| **close the explorer while hung** | **4,718 before the fix**, 132 after | 142 |
| app crash: explorer still fine, no error report | yes, app gone in 9 ms | yes, 6 ms |
| app memory (working set / private) | 11.0 / 1.8 MB | 11.9 / 2.8 MB |
| explorer memory while open | +1.3 MB | +2.3 MB (the two shared pictures) |
| UI Automation (screen readers) sees | every control, with its name | one empty pane |

Bold rows are where a hung panel app froze the explorer for the rest of the hang.

## What native mode needed, and what it still has

A child window from another process shares one input queue with the explorer. Anything the explorer does that
touches that window waits for the app:

- **Resizing.** `SetWindowPos` on the app's window waited 3.5 s even with `SWP_ASYNCWINDOWPOS`: that flag only goes
  async between *different* input queues. Fixed by never touching the app's window: the explorer sends the new size
  over the pipe and the app resizes itself.
- **Closing the explorer.** `DestroyWindow` hides the window and moves focus away, and both waited for the app
  (4.7 s). Fixed by ending the app on `WM_CLOSE` (100 ms to quit, then it is ended).
- **Minimize and restore** still wait for the app (3.2 s). There is no clean fix: moving the app's window out first
  is itself a call that waits for it.
- **Real keystrokes and clicks** wait too. Posted messages bypass the input queue, so `-RealInput` uses real ones
  (it takes the foreground and the mouse for ~15 s, so it is opt-in): a key typed into the hung app sits at the head
  of the shared queue, and a click on the file list reached the explorer only when the app woke up, 4.7 s later.
  There is no fix for this short of separate queues, which is pixels mode.
- Not tested: a DPI change (Windows *sends* the DPI messages to child windows, so likely the same wait).

Each of these can only be found by hitting it, and any new panel app can bring a new one.

## What pixels mode costs

Nothing in the explorer waits for the app, in any of the cases above. The price is everything Windows would
otherwise do inside the app's window:

- **Accessibility.** Screen readers see one empty pane. The fix is the app sending a small list of elements (role,
  name, value, rectangle) and the explorer exposing them through UI Automation: one implementation in the explorer
  that serves every panel app.
- **Text input.** The app draws its own text field. IME composition works through the system's floating window,
  placed at the caret the app reports; it is not drawn inline.
- **Menus and drag and drop** work best when the explorer does them on the app's behalf (the app asks: "menu with
  these items here", "drag these files"; "files dropped at x, y" goes the other way). In the test app they run inside
  the app, which needs the foreground for menus and can't see the mouse button the explorer captured for drags.
- **Memory.** Two pictures of the panel's size in shared memory: w × h × 8 bytes, 2.1 MB here, four times that at
  200 % scaling.

## Found on the way

- **Layout costs 6–9 ms per call**, with or without the panel: most of it is Windows moving the panes and redrawing
  their scrollbars synchronously. It is paid on opening the panel, dragging splitters and resizing the window.
  Batching the moves (`DeferWindowPos`) and drawing the scrollbars ourselves would cut it. (Later: the moves are
  batched, unchanged windows and scroll bars are left alone, and a splitter drag makes 3–4 ms layouts; see
  `tests/resize.ps1`.)
- The **first focus** of a new window costs 17–59 ms once per run (text services attaching to it); later opens
  are ~1 ms. Same in both modes.

## Recommendation

**Pixels**, for every panel app. The point of running panel apps out of process is that they can't hurt the
explorer, and native mode breaks that in ways we can only find one at a time (four found here, two of them
unfixable: minimizing, and any real input while the app hangs).
Pixels mode's costs are known and land in one place, the explorer's side of the protocol:

1. the settings app (stage 2) draws its own controls, all simple: switches, choices, short text fields, key capture;
2. menus, drags and drops go through the protocol (explorer-side, shared by all panel apps);
3. accessibility through an element list, before the side panel is more than settings.
