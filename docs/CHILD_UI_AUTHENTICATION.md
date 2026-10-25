# Child-session automatic sign-in investigation — 2026-10-21

Latest result: **automatic child-session sign-in succeeded in both no-prompt
probes after the user disabled Credential Guard and restarted Windows**. The
agent made no security-policy or production-launcher change. Earlier results
below are retained chronologically; see the final comparison for the current
observed setup.

Microsoft documents automatic child-session logon, with exceptions including a
parent logon that occurred before child sessions were enabled. That prerequisite
needs checking before treating repeated manual passwords as intrinsic to this
method. [Child Sessions](https://learn.microsoft.com/en-us/windows/win32/termserv/child-sessions)

## Observations

- The ordinary parent process is in console session 1 on Default. Supported
  metadata for its own logon reports interactive logon type 2, NTLM and
  `2026-10-14T04:31:44.5366975Z` (07:31 in Bucharest).
- All captured child-feature enable acknowledgements are from October 21. The
  launcher enables the feature on demand and restores its original state after
  cleanup. Its observed original state was disabled.
- Credential Guard is running; Windows reports security service 1 active. The
  computer is not domain-joined. Installed `mstscax.dll` is 10.0.26100.8875.
- The production host already enables CredSSP, sets and reads back loopback
  child mode, and uses the ordinary Windows account. It supplies no password.

The October 14 feature state was not captured. The recorded enable chronology
fits the documented timing exception but does not prove the entire cause.
Likewise, the last-used credential-provider registry entry is account-wide and
does not prove the provider used for this particular parent logon.

Credential Guard restricts CredSSP's use of saved/signed-in credentials while
permitting supplied credentials. It has no per-application exemption. That is
an additional documented constraint, not proof that it alone caused these
particular failures. [Credential Guard considerations](https://learn.microsoft.com/en-us/windows/security/identity-protection/credential-guard/considerations-known-issues)

## Bounded connection probes

Two private diagnostic copies of the host were compiled with the existing
local compiler/type library. They left CredSSP enabled and verified child-mode
readback. `AllowPromptingForCredentials=false` was set and read back before
Connect, so no user password interaction was requested. This setting suppresses
prompts; it does not bypass authentication.
[Microsoft property documentation](https://learn.microsoft.com/en-us/windows/win32/termserv/imsrdpclientnonscriptable5-allowpromptingforcredentials)

| Probe | Result |
|---|---|
| Current explicit username/domain | Disconnected with code 2055 before child creation; no automatic logon or host-ready acknowledgement. |
| Default child identity, without explicit username/domain assignment | Same result. |

Each probe completed in under five seconds including capability enable/restore.
Neither supplied credentials, permitted a credential prompt, disabled CredSSP,
changed NLA/delegation policy or changed Credential Guard. Omitting the explicit
identity settings did not enable SSO. The numeric disconnect code does not
isolate enable timing from Credential Guard or another Windows-build behavior.

The exact hosts exited, WTS positively reported no child, the capability returned
to disabled and no active launcher pointer remained. Probe receipts, input/image
hashes and all artifacts were independently verified. Production source, test
source and release files stayed unchanged; this was connection diagnosis rather
than product acceptance. WSL/Docker stayed running with the same WSL boot ID.

## Next distinguishing test

At a future planned parent Windows sign-in, have child sessions enabled **before**
that sign-in, authenticate normally with the account password, then repeat the
no-prompt loopback probes with Credential Guard still enabled. This test has not
been performed; it may still encounter Credential Guard or build-specific limits.

The existing parent session was not logged off, locked or restarted for this
investigation. The feature was not left persistently enabled. Persistent enable
would be an explicit setup choice distinct from the launcher's existing temporary
enable/restore behavior. A successful future experiment should precede any claim
that changing that default fixes unattended startup.

No credential extraction, password storage, token duplication/impersonation,
authentication-provider replacement or second-account setup was attempted.

Evidence: `build/child_sso_diagnosis/20261021-2f545783fabf/CLOSEOUT.json`, receipt
`25773fd40018bb1168cd0c0239634805501f889cb4bebcf372f24205cdf73d94`.
The tested snapshot precedes this documentation-only update. Existing UI reuse,
controller recovery and pending expiry qualifications retain their prior scopes.

## Pre-reboot preparation — October 21, 19:50 Bucharest

At the user's planned restart, the supported API was called to leave child
sessions enabled before the next Windows sign-in. Independent readback confirmed
enabled, no child, and the API's stored Terminal Server `fDenyChildConnections`
value changed from 1 to 0. No startup task, credential storage, security-policy
change or automatic restart was introduced. Persistence and automatic sign-in
after reboot remain untested. This supersedes the earlier disabled end state
only for this explicit follow-up experiment.

Exact preparation, read-only postboot observer and optional guarded rollback:
`build/child_sso_reboot/20261021-570ccdc86e4c/HANDOFF.md`.
Preparation receipt:
`80ee803ccc4e29f801d833353431cab2cb68ba6d7c39b2187c47ad7263754085`.

## Postboot result — October 21, 20:04 Bucharest

The user restarted Windows. Read-only observation, before any launcher or
feature-enable helper, confirmed boot at 19:59:32 and the child feature still
enabled (`fDenyChildConnections=0`), with no child present. The new parent logon
was 19:59:43, interactive type 2, NTLM. The last-used account provider was
PasswordProvider; this account-wide value does not independently establish
password versus PIN for the exact current logon. Credential Guard remained
running (service 1).

Fresh copies of both no-prompt probes (explicit current identity and default
identity) each disconnected with code 2055 before child creation, as before
reboot. CredSSP and child mode stayed enabled; prompting stayed disabled. No
postboot feature-enable call, supplied credentials, policy change or automatic
retry occurred. This establishes persistence through this reboot and continued
SSO failure in the observed setup. It does not isolate Credential Guard or
another Windows/authentication constraint as the cause.

Both exact probe hosts exited, child absence and no active pointer were verified,
and the intentionally enabled feature was preserved for this experiment. All
793 source files and 141 release files matched the precheck baseline
before this documentation update; WSL stayed on the same boot during the check.
No product acceptance or performance suites were rerun for connection diagnosis.

Evidence: `build/child_sso_postboot/20261021-c232b6ce2899/CLOSEOUT.json`, receipt
`758d0da233c4d34a24b2ec971249f744bdd176cac15bd76dc4914af3f1be401b`. Prior session reuse acceptance and pending
expiry/N11 qualifications are unchanged.

## User clarification: automatic parent Windows sign-in

The user confirmed that Windows signs them in automatically after restarting;
no password or PIN is entered. The exact automatic-sign-in mechanism has not
been inspected. The reboot probes therefore exercised an automatically signed-in
parent, and the proposed manual account-password logon comparison is still
untested. This does not establish automatic sign-in as the cause of error 2055.
The observed feature persistence, failures, cleanup and Credential Guard state
remain unchanged. No automatic-logon settings or stored secrets were inspected
or changed. A future manual-password comparison requires a user-chosen parent
sign-in; no sign-out or restart was initiated.

Immutable clarification: `build/child_sso_postboot/20261021-c232b6ce2899/LOGON-CLARIFICATION.json`,
receipt `bc15932c90a911b320a2473742225b8e19c4399361a41b80bde6838c70a151f2`.

## Manual-password parent sign-in result — October 21, 20:14 Bucharest

The user confirmed signing out and signing back in by entering the Windows
account password. Supported own-logon metadata independently confirms a new
interactive type 2 / NTLM logon at 20:12:21, console session 2, replacing the
previous session 1. The feature was still enabled before the probes; no enable
or restore API was called. Credential Guard remained running (service 1).

Both fresh no-prompt probes again disconnected with code 2055 before child
creation. Explicit current identity and default identity both failed with
CredSSP and child mode enabled. Thus the manual-password comparison is now
complete: pre-enabling, rebooting and then a fresh manual account-password
logon did not resolve SSO in the observed setup. Automatic parent sign-in is
not established as the cause; neither is Credential Guard as the sole cause.
No further restart or sign-out is needed to repeat this same comparison.

A bounded read-only event query found no events attributed to the exact probe
host PIDs in enabled RDPClient/NTLM Operational logs; LSA Operational was
disabled. This does not exclude server-side events or other PID attribution.
The first event query had a timestamp-conversion error; preserved v2 evidence
uses the correct UTC window. No logging policy was changed.

Independent verification confirms both exact hosts exited, no child or active
launcher pointer remains, and the intentionally enabled feature was preserved.
All 793 source files and 141 release files matched the precheck snapshot
before this documentation update. WSL boot identity stayed unchanged during
these checks. No product/UI acceptance or performance gates were rerun.

Evidence: `build/child_sso_password_logon/20261021-1c5df34819de/CLOSEOUT.json`,
receipt `1c28f3707f97ca8aba7e9a0d1acfb57a030831d699d3ca9d8e8d9f34936bf812`.
This supersedes the previously pending manual-password comparison, not the
existing UI acceptance or remaining expiry/N11 qualifications.

## Credential Guard comparison — October 21, 20:31 Bucharest

The user reported disabling Credential Guard and requested the same comparison.
Read-only observation confirmed a new Windows boot at 20:28:07, parent logon at
20:28:20, the child feature still enabled, and Credential Guard absent from the
running security-service list (previously [1], now [2]). VBS remained reported
running. The RDP binary version/hash and diagnostic host source were unchanged.
The agent did not change security, credential, delegation or feature settings.

**Both fresh no-prompt probes succeeded**, with explicit current identity and
default identity. Each produced login-complete and host-ready without supplied
credentials or a permitted credential prompt. CredSSP and child mode remained
enabled. Both exact owned children were logged off cleanly after these bounded
connection tests; the hosts exited, no active pointer remained, and the feature
was preserved as enabled.

This strongly supports Credential Guard as the relevant blocker in this setup.
It is a before/after observation with an intervening user reboot and changes in
reported security-service sets, not an isolated on/off reversal proving a sole
cause. No password storage or client workaround was added. All 793 source
files and 141 release files matched the baseline before this docs update;
WSL boot identity stayed unchanged during these checks. Full product/UI and
performance suites were not rerun for connection diagnosis.

Evidence: `build/child_sso_guard_comparison/20261021-94452caf51a5/CLOSEOUT.json`,
receipt `c4a556d98dc04829ab9d098d0f9e040bbd7a0d1c2f76864bc642d416ee3e9c56`.
Normal reusable-launcher validation, if performed, is recorded separately in
this evidence directory. Existing expiry/N11 qualifications are unchanged.
