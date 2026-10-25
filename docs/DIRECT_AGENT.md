# Direct agent in OpenPilot

Choose **Direct agent** on the action dock (or in More when space is narrow), or
use the existing **Direct agent** command in the palette. The optional native
`direct-client.exe` lives beside `folderwing.exe`. It uses native Win32 controls and
the same process/pixel panel contract as other helpers; no Electron runtime is
loaded into OpenPilot. Normal browsing starts no Direct client or runtime.

The client supports retained threads, composing/streaming, explicit Stop,
questions/approvals and reviewed selected-file references. Selecting files adds
references to a draft; Send is explicit. The independently owned Direct runtime
continues when the panel closes. Disabling `direct.show` retires the presentation
helper, leaving runtime work alone. Direct is included in the default dock item
list as `direct`; existing explicit item lists can enable it through Visible
buttons. Its selected state follows the visible panel.

The native client/runtime sources are maintained separately at
`~/work/LexLattice/codex-review-shell-direct`, under `native/direct-client/` and
`src/main/direct/runtime-host/`. OpenPilot builds do not build that external
project; its accepted `direct-client.exe` is installed as an optional asset.

## Initial owner installation

The 2026-10-24 installation binds one dedicated Windows runtime to
`C:\Work\openpilot`, project `openpilot`, using the previously tested
`gpt-6.1-sol` model with low reasoning effort. It reads the existing WSL Codex
sign-in cache without copying or refreshing credentials. Model authentication is
not a guarantee of a successful later provider turn. Login expiry/revocation must
be resolved through the account's ordinary sign-in mechanism.

Runtime configuration, logs and lifecycle launchers are installed under
`%LOCALAPPDATA%\OpenPilot\Direct`:

- **Start Direct Runtime.cmd** starts the independent host or reuses the matching
  ready host. It returns after readiness; the host has detached stdio/lifetime.
- **Status Direct Runtime.cmd** reports readiness without sending a model turn.
- **Stop Direct Runtime.cmd** gracefully shuts down that configured host and its
  active tasks. It uses authenticated profile discovery rather than killing
  arbitrary Node processes.
- `direct-runtime.json` contains paths/project/model choices, not credentials.
- `profile/` contains the runtime's private discovery and durable thread data.
  The runtime profile identity pins project/model/auth-path; a different binding
  requires a different profile rather than rewriting an active identity.

The native client's `%LOCALAPPDATA%\DirectLean\client.json` remembers only the
profile folder, allowing the panel to connect automatically. The host must be
started separately after a Windows restart; it is not a login/background service.
Once started, reopening the panel reattaches without submitting a turn. A new
conversation starts read-only and existing owner decision controls remain in use.
This initial host is scoped to OpenPilot; references elsewhere do not silently
change its project or grant access to those paths.

`tools/direct-runtime.ps1` and `tools/direct-runtime-start.mjs` are the reusable
launcher sources. Installed configuration paths belong to this owner's machine;
other machines must supply their own dedicated profile/project/auth paths.

## Validation scope

The focused dock acceptance exercises all placement/overflow/DPI behavior, the
real native client's panel READY handshake, selected highlighting, source-context
preservation and presentation retirement/disabled dispatch. Its client uses an
owned empty LOCALAPPDATA directory and connects to no personal runtime.

Installation separately checks runtime readiness, idempotent Start, graceful Stop
and restart with an empty new profile. No provider request or live file operation
is submitted to validate setup. Earlier native-client/offline/live-provider
acceptance remains in the Direct project's own documented evidence; it is not a
fresh all-feature or live-account acceptance claim for this installation.
