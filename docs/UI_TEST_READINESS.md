# UI acceptance readiness

`tests/ui_readiness.ps1` is an opt-in helper for PDF transport, folder comparison,
text preview and thumbnails. Other acceptance scripts retain their existing
input and readiness contracts. It uses the existing acknowledged, modifier-isolated
input helper; navigation must acknowledge its request and commit without an
address error. Comparison windows must belong to the current test app PID.
Settings checks observe a new revision, rather than assuming a fixed delay means
the reload completed. The comparison settings wait retains its original 500 ms
budget, and opening plus completion share the existing seven-second deadline.

Only a failed assertion/deadline collects a readiness snapshot. JSON evidence
records owned addresses and request/loading/resolving/error state, settings
revision, comparison phase (absent/building/busy/finished), reader visibility and
titles, helper PID/birth identities, and clipboard ownership/sequence. Helper
history records processes actually observed by existing polling; it is not a
complete process-creation trace or a product transport generation counter. The
observer never reads clipboard contents or inspects destroyed edit controls.
An observer error is reported separately and cannot make an assertion pass.

Run `tests/ui_readiness_contract.ps1` for independent hidden-window checks of
negative states, owner filtering, settings acknowledgement, navigation-error
reporting and observation after destruction. Run `tests/ui_input_contract.ps1`
for the input helper's separate keyboard/modifier contracts. Helper changes also
require the four owning live acceptance scripts. No larger deadline or automatic
retry is permitted to replace these outcomes.

`tests/transfer_crash.ps1 -OtherRoot E:\ -Config release` copies and hash-verifies
`build/release/folderwing-durable.exe` beside its temporary native writers. It requires two
NTFS volumes and must use approved fixture volumes; do not point the test at the
WSL backing drive. Missed checkpoints report exit, terminal job/error and flush
counters. Successful fixtures are cleaned only after their owner token is
checked; failed fixtures and stages remain for review.

The clipboard selector in `tests/drag_tests.c` distinguishes unavailable reads
from verified data mismatch. Preservation includes the sequence captured before
publication. Only unavailable read access can wait (at most 1500 ms); a verified
mismatch or changed sequence fails immediately. Read polling neither pumps UI
messages nor republishes data. Independent held-read and mismatch controls stay
inside that selector. Its owning gate is the complete `tests/dragging.ps1` lane,
with its Windows helpers, desktop, private Linux root and unchanged dispatch guard.
