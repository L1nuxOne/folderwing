# Native test-impact selection

`tests/native-impact.json` is the standing native selector registry. It records
each selector's primary claim, owner and additive source dependencies, plus the
executable's compile inputs and resource/fixture requirements. It replaces the
need to reconstruct current native mappings from historical `build/` reports.
Those reports remain evidence of their own snapshots.

From the repository root, with Python 3.10 or newer:

```sh
python3 tools/native-impact.py audit
python3 tools/native-impact.py self-check
python3 tools/native-impact.py inventory
python3 tools/native-impact.py select --changed app/fn.c app/fn.h
```

The commands also run under native Windows Python (`python` instead of
`python3`). They do not require WSL, Windows elevation, a desktop or a native
rebuild. Auditing/selection reads the source tree and registry; the self-check
creates and removes only its own temporary fixtures. Neither command launches
product executables, changes Windows settings or publishes binaries.

Run the audit and self-check after changing native tests, their bindings,
headers or build configuration, and before using an impact plan. Selection
always performs the full audit first and exits 2 on an invalid plan; success is
exit 0. `select` prints a plan, **not a test execution receipt**.

## Selection and execution

`--changed` accepts exact, canonical repository-relative paths, using `/` on
both platforms. All paths must resolve inside the checkout. Multiple paths
union their owning selectors. A directory binding covers its descendants,
including newly added fixtures. Unknown/unbound changes fail instead of
producing an empty successful selection. Deleted paths require reconciling the
registry first; they are not silently ignored.

The plan includes both directly affected selectors and the complete selected
executable units. The current binaries have no uniform per-selector CLI, so a
request selecting one test must run its whole executable. It preserves fixture
requirements such as two distinct Windows volumes, actual WSL distros, sibling
helpers, an interactive desktop or bundled dependencies. Missing resources are
failures to execute the required gate; they are not implicit skips. Source
bindings inherited from earlier work remain union-only, including broad shared
engine/app dependencies. Plans favor conservative coverage over minimal counts.

The coordinator ancestry fixture (`tests/ancestry_tests.c`) is not a default
`build.ps1` target. Its module record contains an explicit Zig fixture build
recipe, executed from the repository root, plus its two owned-root arguments.
Do not assume that a historical installed test executable was rebuilt by
`build.ps1`. Other records name their actual build target and fixture route.
Execution guidance supplies requirements; fixture paths must be freshly owned,
never invented from or pointed at user data.

## Maintaining identities and bindings

The static collector discovers every direct `tests/*.c` translation unit. It
finds `static` functions named `test_*`, requires references outside their own
bodies, and seals the executable entry point. It masks comments and literals.
API replacement callbacks are recorded as helpers; an assertion-bearing
callback cannot be silently excluded. When assertions live directly in an
otherwise ungrouped executable, its entry point is the selector. In particular,
the OLE warmup test is `tests/dropwarm_tests.c::wmain`; `test_unole` is a cleanup
callback, not a standalone test. The registry retains this historical identity
correction explicitly.

This is a static identity/reference check, not a C compiler or proof that every
conditional path executes. It does not recursively collect included production
files or `tests/fixtures/` as independent native selectors. Native runtime
results must still come from the corresponding executable and its owning gate.
Python tests and PowerShell acceptance scripts retain their separate inventories
and gates; this native plan cannot replace those mandatory lanes.

For an addition/removal/rename, reconcile the exact inventory identities and
retire stale rows. Every active selector needs one nonempty primary owner and
one established primary classification; mixed modules use exact rows. The
historical primary labels are retained, including their original spelling.
No added test may be left in a legacy/catch-all debt bucket.

Every row includes its source module, declared compiled inputs, transitive local
quoted/default-macro includes, `build.ps1` and this registry/tool. Add semantic
dependencies that includes cannot express: helper protocols, fixtures,
configuration, generation rules and acceptance contracts. Dependencies can only
widen an existing owner's impact selection. The audit rejects duplicate JSON
keys, duplicate bindings, missing files, stale/unmapped selectors and module
drift. The unbound, unclassified, skip, xfail, quarantine and automatic-retry
budgets are zero.

`build_sha256` and the engine/external compile-glob inventories deliberately
require review when build configuration changes. Update those seals only after
checking every affected executable's `compile_inputs` and dependency closure.
The audit resolves local includes again, so a newly included header must be
bound before the registry can pass. External headers/libraries and macro-driven
builds require explicit review; this tool does not infer arbitrary compiler or
PowerShell semantics.

The private `self-check` cases exercise malformed inventories, missing owners
and bindings, callback identity, build/glob drift, path escape, duplicate keys
and additive selection. They are governance audit checks, not additional native
product test selectors. A governance-only mapping update requires this audit
lane; runtime product/test changes still require their normal selected,
acceptance and broader gates.
