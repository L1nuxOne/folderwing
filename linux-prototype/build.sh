#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
out="$PWD/build/linux-prototype"
mkdir -p "$out"
stage="$(mktemp -d "$out/.build-XXXXXX")"
trap 'rm -rf -- "$stage"' EXIT
compiler="${CC:-cc}"
pkg-config --atleast-version=4.8 gtk4
pkg-config --exists sqlite3
# Native build of the SAME full helper sources used by build.ps1. System SQLite
# is a prototype build dependency; this is not a public redistribution package.
helper_sources=(linux/fpwsl.c linux/fppath.c linux/fppermissions.c linux/fpsizes.c
 linux/fpspace.c linux/fpduplicate.c linux/fpgit.c linux/fpcontent.c
 app/contentmatch.c app/textutil.c linux/fpsearch.c linux/fpindex.c linux/fphash.c
 linux/fpguard.c linux/fprename.c linux/fpjobs.c linux/fptransfer.c linux/fpshell.c)
read -r -a sqlite_flags <<< "$(pkg-config --cflags --libs sqlite3)"
"$compiler" -std=c11 -O2 -g -Isrc -Wall -Wextra -Wno-unused-parameter \
 -Wno-missing-field-initializers -Wno-misleading-indentation \
 "${helper_sources[@]}" "${sqlite_flags[@]}" -lm -lpthread -o "$stage/folderwing-linux-helper"
read -r -a gtk_flags <<< "$(pkg-config --cflags --libs gtk4 gio-unix-2.0)"
"$compiler" -std=c11 -O2 -g -Wall -Wextra -Werror \
 linux-prototype/model.c linux-prototype/operations.c linux-prototype/desktop.c linux-prototype/clipboard.c linux-prototype/browser.c "${gtk_flags[@]}" -o "$stage/folderwing-linux"
cp docs/branding/folderwing-icon.svg "$stage/folderwing.svg"
if [[ "${1:-}" == --tests ]]; then
 "$compiler" -std=c11 -O2 -g -Wall -Wextra -Werror \
 tests/linux_browser_tests.c linux-prototype/model.c linux-prototype/operations.c linux-prototype/desktop.c linux-prototype/clipboard.c "${gtk_flags[@]}" -o "$stage/folderwing-linux-browser-tests"
 "$compiler" -std=c11 -O2 -g -Isrc -Wno-unused-parameter -Wno-missing-field-initializers \
  "${helper_sources[@]}" tests/fixtures/linux_ops_faults.c "${sqlite_flags[@]}" -lm -lpthread -Wl,--wrap=fsync -o "$stage/folderwing-linux-helper-faults"
fi
for file in "$stage"/*; do mv -f -- "$file" "$out/$(basename -- "$file")"; done
printf 'Built %s\n' "$out/folderwing-linux"
