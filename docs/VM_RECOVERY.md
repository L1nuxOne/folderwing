# Isolated VM recovery harness

This harness prepares a disposable Hyper-V guest for eight native Linux
cross-filesystem overwrite-move interruption cases. Its default Windows action
prints a plan. It never shuts down WSL, changes host features/services, imports
an existing VM, formats/mounts disks or automatically removes recovery data.

Preparation and process/mock contract checks are not VM recovery results. A
case passes VM recovery only after a matching host-recorded abrupt power-off,
a different guest boot ID, matching lab/case/fixture identities and agreeing
host/guest content oracles. Physical host/controller/device power loss remains separate. The instructions
below describe the original Linux cohort; subsequent isolated cohorts expanded
Windows, tree, selected-file, relay/ACK and metadata coverage.

## Completed isolated recovery cohorts

These historical accepted runs total **104 scenarios and 136 abrupt endpoint
power-offs**. They ran without stopping WSL or Docker. They are evidence for
those frozen source snapshots and storage/guest configurations, not proof for
arbitrary future changes or physical device power failure.

| Cohort | Scenarios | Endpoint power-offs | Evidence directory |
|---|---:|---:|---|
| Linux single-file | 8 | 8 | `build/vm_recovery_run/20261019-guest` |
| Windows single-file NTFS | 8 | 8 | `build/vm_windows_recovery/20261019` |
| Windows tree and small-file batch | 12 | 12 | `build/vm_windows_recovery/20261019-tree` |
| Windows merge and selected files | 12 | 12 | `build/vm_windows_recovery/20261019-merge` |
| Linux trees | 16 | 16 | `build/vm_linux_tree/20261019` |
| Cross-endpoint relay and ACK | 15 | 30 | `build/deferred_retry/20261020` |
| Windows selected coordinator with Linux endpoints | 8 | 16 | `build/vm_selected_linux/20261020` |
| Linux ACL/xattr recovery | 9 | 18 | `build/vm_metadata_linux/20261020` |
| Windows NTFS named streams (N03) | 8 | 8 | `build/guest_hardening/20261021-n03-acl-986f2cca3a` |
| Linux recent nested merges (N04) | 8 | 8 | `build/guest_hardening/20261021-n04-a5cf3cc5c74f` |

Each directory retains `REPORT.md` and its content-addressed `CLOSEOUT.json`.
The N03/N04 follow-up used fresh cases and the current frozen writers. It
verified default, empty and Unicode NTFS streams; destination DACL entries and
inheritance protection; recent existing Linux merge ancestry; and source
removal before/after the required directory fences. Linux native and streamed
cases used separate ext4 volumes in the same guest; natural journal commits
remain possible. These are not independent-endpoint interruption results.

The Windows fixture persists its own boot-volume config and executable before
arming a case and verifies their hashes after reboot. Clean Windows shutdowns
and boots occur after the first recovery observation and before the next fresh
case; recovery policy is unchanged. DACL comparison follows the existing native
`same_dacl` contract: exact ACL bytes, presence/nullness and inheritance
protection, excluding `AUTO_INHERITED` bookkeeping. Native and managed negative
controls and a separate native comparison qualify that oracle.

Earlier failed fixture cohorts and partial data remain under
`build/guest_hardening/`; their ten checkpoint power-offs are excluded from the
accepted totals above. The aggregate `REPORT-20261021.md` and
`CLOSEOUT-20261021.json` record their diagnoses and verified restoration. No
production source or published release image changed during this follow-up.

The old `build/vm_relay_ack/20261019` preparation remains a resource-deferred
attempt, with zero cases; it must not be mistaken for the later accepted relay
run under `build/deferred_retry/20261020`. Initial setup and serial-transport
problems, additional read-only observations and adapter repairs remain recorded
in the relevant reports. No automatic transfer resume, ACK replay, staging
cleanup or all-or-nothing tree rollback is implemented. Existing staging remains
for review. The next source audit receives these scoped evidence summaries;
no new live interruption run is implied by a functional acceptance pass.

## Available backend and preparation artifacts

The 2026-10-19 read-only inventory found Hyper-V and its management module
installed, VMMS running and elevated VM access working, with zero existing VMs.
Ubuntu and docker-desktop remained running throughout the queries. No QEMU,
VirtualBox or VMware executable was found on PATH or the checked standard
installation paths; WSL has no `/dev/kvm`. C: has room for an isolated lab;
D: is nearly full. No host restart/feature enablement is needed to use the
already-installed Hyper-V backend. That was the preparation-time inventory; the later owned guests and accepted runs are recorded above.

The prepared guest bundle and build receipts live under
`build/vm_recovery/20261019/`. The bundle includes a private static x86-64 Linux
fixture, the guest driver, exact source snapshot, hashes and payload contract.
The fixture wraps only the actual writer linked into this test executable; no
test switch is added to `fpwsl` or any published app image. Python 3.10+ and
util-linux `findmnt` are required in the guest; a guest compiler is unnecessary
when using the prepared static fixture.

## Host plan and later VM creation

In PowerShell 7, the default action is safe to inspect without elevation:

```powershell
./tools/vm-recovery.ps1 -Action Plan
```

Actual VM actions require an elevated PowerShell session. Creation accepts a
**clean, standalone generation-2 Linux VHDX template with a usable local login**.
It copies that template; it does not attach or boot the original. Do not use a
WSL distro disk as the template.

```powershell
./tools/vm-recovery.ps1 -Action Create -BaseDisk C:\path\to\clean-linux.vhdx
```

This creates a new GUID-named directory under `build/vm-labs`, a copied boot
VHDX, two blank 2-GiB dynamic data VHDX files and a new VM. The VM has one CPU,
1 GiB fixed memory, no network adapters, disabled automatic checkpoints and a
unique COM1 named pipe. Secure Boot is disabled for this disposable guest only.
Creation leaves the VM **off** and returns its `lease.json` path. Failed
provisioning is retained for review; there is no automatic rollback/deletion.

All subsequent operations validate the exact VM ID/name/ownership note, private
disk set, absence of differencing parents, serial pipe and absence of network
adapters. Lease/disk paths must remain inside the generated lab directory and
have no reparse ancestors. Existing VMs cannot be adopted by name. A lab-wide
exclusive lock serializes case creation, start, interruption and collection.

Start only that lab:

```powershell
./tools/vm-recovery.ps1 -Action Start -Lease C:\path\to\lab\lease.json
```

## Guest data volumes and bundle

Use the new VM's console to prepare its **two newly created blank data disks**
as ext4 and mount them at `/mnt/op-source` and `/mnt/op-destination`. Identify
those disks from the owned VM's disk/controller configuration before formatting;
the harness deliberately supplies no automatic formatting command. Configure
their filesystem UUID mounts to return at the same paths after a reboot. Keep
the copied boot disk separate. Normal guest mounts perform filesystem recovery;
the observer then reads data without changing or cleaning transfer objects.

Transfer/extract the bundle onto the guest's boot disk, for example at
`/opt/openpilot-vm-recovery`, using offline installation media or a supported
Hyper-V guest-file-copy route. Host folders and user disks are not test volumes.
Guest OS installation/media transfer is provisioning work, not performed by
the harness-preparation turn.

Before executing its fixture, verify the archive and each file against the
bundle manifest/build receipt. `manifest.json` binds the supplied static fixture
and observed sources; the owning build receipt records the compilation. Source
hashes alone do not prove a supplied binary was built from those sources.

In the guest, as root, provision markers only after both data mounts are ready:

```sh
python3 /opt/openpilot-vm-recovery/guest.py provision \
  --source-volume /mnt/op-source --destination-volume /mnt/op-destination \
  --lab-token LAB_TOKEN_FROM_HOST_LEASE
```

Provision requires two distinct mounted ext4 filesystems with UUIDs and no
entries except optional `lost+found`. It writes exclusive ownership markers and
flushes them. Existing markers/data are never overwritten. The guest driver
rejects WSL and non-Hyper-V environments before provisioning/arming. Subsequent
commands verify lab tokens, filesystem UUIDs and exact per-case paths.
All guest CLI actions also take an exclusive per-lab lock under `/run`; a second
case cannot flush or mutate the data volumes while an armed case is paused.

## One checkpoint per fresh case

Read the payload/fixture hashes from the host copy of the bundle manifest, then
create a fresh host case. The example uses the point after full publication
barriers and before any source removal:

```powershell
$bundle = Get-Content C:\path\to\bundle-manifest.json -Raw | ConvertFrom-Json
./tools/vm-recovery.ps1 -Action NewCase -Lease C:\path\to\lab\lease.json `
  -Phase after-publication-sync -FixtureSha256 $bundle.fixture_sha256 `
  -NewSha256 $bundle.payload.new_sha256 -OldSha256 $bundle.payload.old_sha256
```

The returned case ID is a new 32-character UUID. Prepare that case in the guest
using its ID and lab token:

```sh
python3 /opt/openpilot-vm-recovery/guest.py prepare \
  --source-volume /mnt/op-source --destination-volume /mnt/op-destination \
  --lab-token LAB_TOKEN --case-id CASE_ID --phase after-publication-sync \
  --fixture /opt/openpilot-vm-recovery/fixture
```

This creates fresh case directories, known source/old-destination bytes and a
case manifest, then flushes baseline files and directories. Source and target
paths are derived from owned roots; caller-supplied replacement paths are not
accepted. Reusing a case, changing a mount/fixture/payload or substituting a
symlink fails instead of overwriting evidence.

Start the host waiter **before** arming the guest:

```powershell
./tools/vm-recovery.ps1 -Action Interrupt -Lease C:\path\to\lab\lease.json `
  -Case C:\path\to\lab\cases\CASE_ID.json -AllowInterrupt -TimeoutSeconds 120
```

Then, in the guest console:

```sh
python3 /opt/openpilot-vm-recovery/guest.py arm \
  --manifest /mnt/op-source/CASE_ID/case.json \
  --fixture /opt/openpilot-vm-recovery/fixture
```

The actual writer stops at the selected checkpoint. Its supervisor confirms
SIGSTOP, then sends a bounded `OPVM1` JSON event through `/dev/ttyS0` with the
lab/case/boot/fixture identities and observed barrier flags. Unexpected
completion, errors, EOF, malformed frames or timeouts do not authorize power-off.
Resuming a checkpointed fixture makes it exit without continuing the transfer.

Only after matching the event and revalidating VM custody does the host issue
`Stop-VM -TurnOff` for the exact owned VM object. It confirms Off and writes the
immutable interruption record **after** power-off, avoiding a host evidence
flush before the interruption. A duplicate case interruption is rejected.
Timeouts/errors preserve case data; no guest or host recovery cleanup is run.

## Reboot and independently verify

Start the same owned VM with `-Action Start`. After its data filesystems are
mounted, start the host collector before requesting the guest observation:

```powershell
./tools/vm-recovery.ps1 -Action Collect -Lease C:\path\to\lab\lease.json `
  -Case C:\path\to\lab\cases\CASE_ID.json -TimeoutSeconds 120
```

```sh
python3 /opt/openpilot-vm-recovery/guest.py verify \
  --manifest /mnt/op-source/CASE_ID/case.json
```

The guest reads exact file hashes with no-follow/no-atime handles and lists
staging candidate names without descending into, deleting or publishing them.
It labels its result as an observation. The host requires the prior matching
power-off record, validates a new guest boot ID, rechecks VM custody and computes
the source/destination oracle independently. A false guest pass label cannot
override mismatching hashes. Results are created exclusively; prior evidence is
never overwritten. `Collect` returns exit 1 for a verified content failure.

| Checkpoint | Source after recovery | Destination after recovery |
| --- | --- | --- |
| `partial-write` | Complete source | Original destination |
| `before-payload-sync` | Complete source | Original destination |
| `after-payload-sync` | Complete source | Original destination |
| `before-destination-sync` | Complete source | Complete original **or** complete new destination |
| `after-publication-sync` | Complete source | Complete new destination |
| `before-source-unlink` | Complete source | Complete new destination |
| `after-source-unlink` | Complete source **or** absent | Complete new destination |
| `after-source-sync` | Absent | Complete new destination |

These are VM-recovery oracles for a pre-flushed baseline on the selected ext4
configuration, assuming truthful successful flushes. Unflushed namespace changes
are not required to survive. The publication point counts both destination and
staging directory fences. There is no automatic resume, ACK replay, rollback or
staging deletion. Preserve the VM/disks and evidence for review on any failure.

## Rebuild and harness checks

Build the private fixture into a fresh output directory, for example with Zig:

```sh
zig cc -target x86_64-linux-musl -static -std=c11 -O2 -pthread -Isrc \
  linux/fpjobs.c tests/fixtures/vm_recovery.c \
  -Wl,--wrap=fsync -Wl,--wrap=write -Wl,--wrap=unlinkat \
  -o build/NEW_EVIDENCE_DIRECTORY/vm-recovery-fixture
python3 tools/vm-recovery-bundle.py \
  --fixture build/NEW_EVIDENCE_DIRECTORY/vm-recovery-fixture \
  --output build/NEW_EVIDENCE_DIRECTORY/openpilot-vm-recovery.tar.gz
```

The bundler rejects dynamic-interpreter/wrong-platform fixtures and existing
output files. It creates a deterministic tarball with exact hashes and fixed
safe archive paths. It does not execute the fixture or launch any VM.

Run the complete local harness contracts:

```sh
python3 -m unittest discover -s tests -p test_vm_recovery.py -v
```

```powershell
pwsh -NoProfile -File tests/vm_recovery_contract.ps1
```

Python cases cover the recovery oracles, guest custody, payload/manifest/bundle
integrity and all eight actual-writer stops on private temporary filesystems.
Those stops use process signals and provide checkpoint wiring evidence only.
PowerShell cases mock every VM mutation and validate ownership/serial/replay/
oracle guards using private files. They never interrupt a real VM. Exact
classifications, source-impact edges and resource requirements are recorded in
`tests/vm-recovery-manifest.json`; the native inventory remains unchanged.

Hyper-V command contracts: [Stop-VM](https://learn.microsoft.com/en-us/powershell/module/hyper-v/stop-vm),
[Set-VMComPort](https://learn.microsoft.com/en-us/powershell/module/hyper-v/set-vmcomport),
[Add-VMHardDiskDrive](https://learn.microsoft.com/en-us/powershell/module/hyper-v/add-vmharddiskdrive).
