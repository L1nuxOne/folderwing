# Isolated Windows UI acceptance

Run from ordinary Windows PowerShell 7 in the active console, after building:

```powershell
pwsh -NoProfile -File C:\Work\openpilot\tools\child-ui.ps1 -Profile Shell
pwsh -NoProfile -File C:\Work\openpilot\tools\child-ui.ps1 -Profile All
pwsh -NoProfile -File C:\Work\openpilot\tools\child-ui.ps1 -Action Status
pwsh -NoProfile -File C:\Work\openpilot\tools\child-ui.ps1 -Action Stop
```

The first Run starts an owned child session. Windows may authenticate it
automatically or open its normal sign-in dialog, depending on the parent logon
and Windows policy.
Later Run commands reuse that live authenticated session. `-Action Start` opens
and authenticates it without running tests. `-Action Stop` logs off the exact
owned child and restores the original child-session feature setting. Use
`-CloseAfterRun` for a run followed by session cleanup. Closing the view-only host
also ends the session. Defaults are 60 minutes idle and eight hours maximum;
`-IdleMinutes 5..240` and `-SessionHours 1..12` apply when starting a new session.
Status does not extend the idle period. A controller/session loss requires new
Windows authentication; no password or authentication token is stored by the tool.

Earlier parent logons required manual child sign-in. After enabling the child
feature before parent password sign-in and the user's independent Credential
Guard change, no-prompt diagnostic connections and the normal public launcher
successfully authenticated automatically. Fresh runs also reused the same live
session. This is observed behavior on this setup, not a universal guarantee or a
tool-managed authentication bypass. See [authentication diagnosis](CHILD_UI_AUTHENTICATION.md)
for the chronological evidence and limits. The tool stores no credentials and
does not change authentication/security policy.

`Smoke` (default) runs complete `tests/ui_smoke.ps1`, including native Explorer
menus and Properties. `Settings` runs all eight N08/N10/N09/N12 cases; `Shell`
runs the independent shell lane. `All` runs them serially with one worker.
`-Config debug` selects the debug build. No arbitrary script or command is
accepted. `-BuildOnly` compiles/verifies the launcher without enabling the feature
or creating a session. Local Windows SDK .NET Framework 4.8 x64 tools
(`TlbImp.exe`) and the installed Framework compiler are required; no downloads.

Each Run captures a fresh source/image manifest, resource posture and input key,
then creates its own private desktop, least-privilege triggerless task and receipt.
Only authentication is reused; test results and worker processes are not reused.
Source and release changes between runs are allowed. Changes to launcher control
files require Stop followed by a new session, so the executing controller and
compiled host/agent remain the verified version. Its scripts and narrow elevated
feature capsule are frozen in the content-addressed cache, including cleanup code.
A second Run is refused while acceptance is active; Status and Stop remain usable.
Explicit Stop cancels an active run as unqualified, then cleans up the owned child.
Infrastructure failures, residue or failed private-desktop restoration retire the
session. Completed assertion failures may retain it only after verified cleanup.

The launcher discovers the Windows SID and parent/child sessions. Reuse binds the
session token, controller and host PID/birth/image, cache identity, parent console,
child ID and original host acknowledgement. Unknown sessions, stale identities,
changed task definitions, replayed requests and escaped paths are refused.
A stale controller or blocked cleanup is never silently adopted. Inspect the
retained session evidence before recovery; the launcher does not delete ambiguous
objects or disable a feature that was originally enabled.

If the child-session feature is disabled, the frozen helper requests the normal
administrator prompt and uses `WTSEnableChildSessions`. Its original value is
restored when the session ends, rather than between runs. The ordinary-token RDP
ActiveX host connects only to localhost with `ConnectToChildSession` read back as
true. Clipboard, drive, printer, port and smart-card redirection are disabled.
Credential Guard may require the Windows account password in its normal dialog.
Enter it there; never send it in chat. Authentication/UAC/NLA/Defender/listener and
firewall policy are unchanged. WSL and Docker are not stopped or reconfigured.

For every run, an ordinary-token bootstrap creates a unique private desktop
inside the child, leaving startup programs on Default. It copies Default's existing
integrity label onto only that new desktop, verifies the label and unchanged DACL,
and restores Default after the worker exits. Low-integrity preview helper tokens
are unchanged. Worker admission verifies the exact private input/caller desktop.
`OPENPILOT_CHILD_UI_LEASE` opts into unchanged-size work-area placement for newly
owned test windows on the 1920x1080 child desktop; no focus request is added.
Ordinary desktop test placement, assertions and phase deadlines are unchanged.

`build/child-ui/<session-token>/` retains the bootstrap, frozen-cache identity,
host events, session acknowledgement, atomic observational state, requests and
final session cleanup receipt. The active pointer contains identities, not
credentials. Every run has its separate `build/child-ui/<run-token>/`: input hashes,
lease, exact task XML/instance, placement, raw output, results and write-once
`FINAL.json`. A retained-session run receipt references the later session cleanup
receipt; it does not claim that the host has already closed. Failures are not
retried. Existing optional resource absences, such as Everything IPC on the private
desktop, are counted and require separate coverage. These profiles do not qualify
physical-console input, host GPU/DPI conditions or performance.

Smoke WSL fixtures use fresh `mktemp` roots and owner markers, bounded native waits,
and guarded cleanup only after success. Failed fixtures remain. The companion
`tests/ui_smoke_wsl_contract.ps1` checks the real lifecycle and wrong-owner deletion
refusal without stopping WSL. `tests/child_session_contract.ps1` checks negative
session/process/request/task admission and write-once evidence without creating a
session or sending input. Its owning live gate is repeated Run commands in one
session, complete `-Profile All`, Status/concurrency checks and explicit Stop.

The 2026-10-21 reuse acceptance passed on cache
`8d17569b343b45dfe31b4a2861fc98a1dc854d4f449b60d981df86d26d4c853f`:
cold `All` (160 smoke checks, 40 Settings assertions in eight cases, 10 shell
checks), another `Shell` run (10 checks), concurrent Run refusal with no new
request, and cancellation of an admitted third run followed by verified cleanup.
There was one connection and one Windows sign-in across all three fresh runs.
The cancelled run is unqualified for UI assertions. All 42 admission contracts
and 32 native governance self-checks passed. The owned tasks, controller, host
and child were absent afterward; the feature returned to its original disabled
setting, and WSL/Docker remained running. Everything IPC was unavailable in the
private child; separate integration coverage is recorded below. This does not
qualify performance or idle/maximum-session expiry.

Evidence: `build/child_ui_acceptance/20261021-f1f38ca66a10/CLOSEOUT.json`, receipt
`2ee3b4c6e6d9f6a254c72e79e2b504838e06a9303e1bef0a2db9fe559a2d85fa`.
Its tested source snapshot precedes this documentation-only acceptance note;
launcher controls and release outputs were unchanged throughout validation.

The separate 2026-10-21 Everything acceptance passed against the existing
Everything 1.5.0.1409a process on the parent Default desktop, using its standard
IPC class. The complete release `app_tests.exe` passed 2,882 assertions with
the actual Everything query branch executed. Eighteen additional checks against
release `folderwing.exe` verified indexed Go to navigation, indexed folder search,
zero-match results retaining the indexed backend, case/type filters, syntax-error
recovery, file reveal and released function lifetimes. A separate owned app
configuration naming a missing instance verified recursive live fallback,
Unicode filename matching, zero results and nested-file reveal without changing
or stopping the real Everything process. These checks used acknowledged messages
to owned windows and no physical keyboard/mouse input.

Both owned release app processes and the native test process were absent after
the run; the original Everything IPC window remained. All 791 source files and
141 release files were unchanged during validation, native/Python inventories
were unchanged, and WSL/Docker remained running with the same WSL boot ID.
There were no skipped product checks or automatic retries. This qualifies the
installed instance separately; it does not make its IPC window visible from the
private child desktop or qualify other Everything versions or performance.
Evidence: `build/everything_acceptance/20261021-47a21af93829/CLOSEOUT.json`, receipt
`039a1e7312949059b193d8ece56f0b506d2362cc9e4fbfd4fa00f80c760d85a3`.
The tested source snapshot precedes this documentation-only acceptance update.

If the controller is forcibly killed, its host watches controller lifetime and
closes the owned child, but global feature restoration may need operator recovery.
After reviewing the lease, host result and exact task XML, an operator can invoke
only the frozen helper recorded in `cache_path` (normal administrator prompt):

```powershell
Start-Process pwsh -Verb RunAs -Wait -ArgumentList '-NoProfile -File "<cache_path>\child-ui-feature.ps1" -LeasePath "<session_root>\lease.json" -Action Restore'
```

Restoration refuses any existing child or changed parent console/account. Never
remove a task/session by name alone. Interrupted runs remain unqualified.

The 2026-10-21 active-worker controller-loss drill verified the host's automatic
owned-child logoff and stale-session refusal. The controller was terminated by
its exact PID/birth/session/image identity, without terminating descendants.
Its host logged off the owned child; the interrupted run acquired no passing
acceptance receipt. An identity-checked operator then removed only its captured
task XML/instance, invoked the frozen Restore helper, and retired only its exact
stale active pointer after independently confirming controller/host/child absence.
The feature returned to its original disabled state. A write-once `RECOVERY.json`
records recovery separately from normal `FINAL.json`; failed-run data remains.
This is manual operator recovery, not automatic full cleanup or resumable tests.

Evidence: `build/session_lifecycle/20261021-b8b76dddd4a2/CLOSEOUT.json`, receipt
`4254cac0387cebcd7feeb38573a35ba3db04994785fe33f17f4d1ee455ab450b`.
The later real five-minute idle and one-hour maximum-lifetime tests both passed
using the public limits and unchanged clocks. Status did not extend idle; the
maximum test used four hours of idle allowance and no test requests. Exact
receipt/process/session cleanup checks passed and preserved the originally
enabled feature. Evidence: `build/remaining_ui_acceptance/20261021-615adeb316a4/STATUS.json`.
See [consolidated hardening status](HARDENING_STATUS.md) for the current ledger.

The exact N11 changed-view/modal gate is a separate parent-desktop lane:

```powershell
pwsh -NoProfile -File C:\Work\openpilot\tests\gesture_modal.ps1 -Config release
```

Coordinate about one minute of keyboard/mouse idleness first. It uses owned
windows/files, actual modal menus and the registered OLE drop target; no physical
input is injected. It waits for actual copy completion before exact-byte checks.
The private child desktop rejected the normal elevated durability-broker launch,
so this lane requires the ordinary parent Default desktop and ordinary token.
No broker/durability checks are weakened. The fixed child Shell/All profiles are
unchanged. Source-side physical OLE mouse routing and performance are not qualified.
