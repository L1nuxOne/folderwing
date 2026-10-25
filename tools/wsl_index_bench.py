#!/usr/bin/env python3
"""Reproducible WSL filename-index experiment; never imported by OpenPilot.

Linux only, stdlib Python 3.10+, SQLite with FTS5 trigram support. Databases
live in private temporary directories on --scratch (use the Linux filesystem).
The native probe is optional but required for a native memory/deployment claim.
No filesystem names, user settings, running app, or installed distro is changed.
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import random
import resource
import select
import sqlite3
import stat
import struct
import subprocess
import sys
import tempfile
import time

LIMIT = 301  # 300 visible results plus evidence that the result is truncated.
CACHE_KIB = 16384
REPEATS = 7
SCHEMA = """
PRAGMA application_id=0x4f504942;
CREATE TABLE entries(id INTEGER PRIMARY KEY, path BLOB NOT NULL UNIQUE,
 name TEXT NOT NULL, size INTEGER NOT NULL, mtime INTEGER NOT NULL, kind INTEGER NOT NULL);
CREATE INDEX by_size ON entries(size);
CREATE INDEX by_mtime ON entries(mtime);
"""
TRIGGERS = """
CREATE TRIGGER entries_ai AFTER INSERT ON entries BEGIN
 INSERT INTO names(rowid,name) VALUES(new.id,new.name); END;
CREATE TRIGGER entries_ad AFTER DELETE ON entries BEGIN
 INSERT INTO names(names,rowid,name) VALUES('delete',old.id,old.name); END;
CREATE TRIGGER entries_au AFTER UPDATE OF name ON entries BEGIN
 INSERT INTO names(names,rowid,name) VALUES('delete',old.id,old.name);
 INSERT INTO names(rowid,name) VALUES(new.id,new.name); END;
"""
# Decision thresholds are exploratory, declared before the measurements. These
# are not changes to any product/test performance budget.
CRITERIA = {
    "selective_warm_p95_ms": 50, "selective_first_connection_ms": 100,
    "million_row_storage_build_s": 60, "million_row_db_bytes": 300 * 1024**2,
    "mixed_10000_transaction_s": 2, "native_query_and_rebuild_peak_rss_kib": 96 * 1024,
    "correctness_mismatches": 0,
}


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def evaluate(case):
    """Keep failed measurements as evidence; do not silently change thresholds."""
    if case["corpus"] or case["rows"] != 1000000:
        return {"million_row_decision_case": False}
    native = case["native"]
    selective = [q for q in case["trigram"].values() if q["selective"]]
    first = [native[key]["first_query_ms"] for key in
             ("trigram:rare_substring", "trigram:missing_long") if key in native]
    measurements = {
        "selective_warm_p95_ms": max(q["p95_ms"] for q in selective),
        "selective_first_connection_ms": max(first) if first else None,
        "million_row_storage_build_s": case["phases_s"]["storage_build_total"],
        "million_row_db_bytes": case["disk_bytes"]["indexed_checkpointed"]["database"],
        "mixed_10000_transaction_s": case["updates"]["transaction_s"],
        "native_query_and_rebuild_peak_rss_kib": max(v["peak_process_rss_kib"] for v in native.values()) if native else None,
        "correctness_mismatches": case["correctness"]["mismatches"],
    }
    checks = {name: {"actual": actual, "maximum": CRITERIA[name],
                     "passed": actual is not None and actual <= CRITERIA[name]}
              for name, actual in measurements.items()}
    return {"million_row_decision_case": True, "checks": checks,
            "passed": all(value["passed"] for value in checks.values())}


def text_name(raw):
    # ASCII folding only; never use this experiment as a Unicode case contract.
    # Backslash escapes here are synthetic search text; original path bytes are
    # stored separately. Production must reuse the app's matcher and encoding.
    return raw.lower().decode("utf-8", "backslashreplace")


EDGE_NAMES = [
    b"a:b.txt", b"a\\b.txt", b"percent%literal.txt", b"under_score.txt",
    b"bracket[name].txt", b'say "hello".txt', b"line\nbreak.txt",
    b"raw-\xff-notes.txt", b"raw-\xfe-notes.txt", b"raw-\\xff-notes.txt",
    "Ωmega.txt".encode(), "ωmega.txt".encode(), "café.txt".encode(),
    b"cafe.txt", b"Case.txt", b"case.txt", b"[?]*.txt", b"notes..txt",
]


def synthetic_rows(count):
    """Deterministic, bounded-memory mixed development/document filename set."""
    for i in range(count):
        if i < len(EDGE_NAMES):
            name = EDGE_NAMES[i]
            folder = b"edge/"
        else:
            number = f"{i:08x}"
            family = i % 10
            name = (
                f"report-{number}-2026.pdf" if family == 0 else
                f"engine_{number}_config.json" if family == 1 else
                f"IMG_{number}_holiday.jpg" if family == 2 else
                f"readme-{number}.md" if family == 3 else
                f"archive-{number}.tar.gz" if family == 4 else
                f"component-{number}.cpp" if family == 5 else
                f"test_{number}_operations.py" if family == 6 else
                f"budget-{number}-draft.xlsx" if family == 7 else
                f"notes-{number}.txt" if family == 8 else
                f"manual-{number}.html"
            ).encode()
            if i % 9973 == 0 or i == count - 1:
                name = b"cobalt_needle-" + name
            if i % 1009 == 0:
                name = "café-".encode() + name
            folder = f"projects/{'alpha' if i % 2 else 'beta'}/module-{i % 211}/".encode()
        path = folder + name
        yield (i + 1, path, text_name(name), (i * 7919) % (256 * 1024**2),
               1700000000 + i % (365 * 86400), 1 if i % 37 == 0 else 0)


def filesystem_rows(root, limit, state):
    """Read-only corpus sample. Links/special entries are recorded, never followed.

    This measures enumeration separately from storage. It is a benchmark corpus
    reader, not the production descriptor-relative, mount-aware scanner.
    """
    root = os.fsencode(root)
    root_device = os.stat(root, follow_symlinks=False).st_dev
    if not stat.S_ISDIR(os.stat(root, follow_symlinks=False).st_mode):
        raise ValueError("corpus root must be a real directory")
    stack = [b""]
    count = 0
    enumeration_s = 0.0
    while stack:
        relative = stack.pop()
        start = time.perf_counter()
        try:
            with os.scandir(os.path.join(root, relative)) as directory:
                items = sorted(directory, key=lambda e: e.name)
        except OSError:
            state["enumeration_errors"] += 1
            enumeration_s += time.perf_counter() - start
            continue
        enumeration_s += time.perf_counter() - start
        for item in items:
            start = time.perf_counter()
            try:
                meta = item.stat(follow_symlinks=False)
            except OSError:
                state["enumeration_errors"] += 1
                enumeration_s += time.perf_counter() - start
                continue
            path = os.path.join(relative, item.name)
            is_dir = stat.S_ISDIR(meta.st_mode)
            if is_dir and meta.st_dev == root_device:
                stack.append(path)
            kind = 1 if is_dir else 0 if stat.S_ISREG(meta.st_mode) else 2
            enumeration_s += time.perf_counter() - start
            count += 1
            yield (count, path, text_name(item.name), max(0, meta.st_size) if kind == 0 else 0,
                   int(meta.st_mtime), kind)
            if count == limit:
                state["corpus_limit_reached"] = True
                state["enumeration_s"] = enumeration_s
                return
    state["enumeration_s"] = enumeration_s


def literal_glob(text):
    # Do not use LIKE ... ESCAPE: FTS5 cannot accelerate that form.
    return "*" + "".join({"*": "[*]", "?": "[?]", "[": "[[]"}.get(c, c) for c in text) + "*"


def queries():
    specs = [
        ("rare_substring", "cobalt_needle", True),
        ("missing_long", "zzq-no-such-name", True),
        ("common_substring", "report", False),
        ("one_character", "e", False),
        ("two_characters_missing", "qz", False),
        ("literal_underscore", "under_score", True),
        ("literal_percent", "percent%literal", True),
        ("literal_brackets", "bracket[name]", True),
        ("literal_quote", 'say "hello"', True),
        ("literal_colon", "a:b", True),
        ("literal_backslash", "a\\b", True),
        ("literal_wildcards", "[?]*", False),
        ("accent_preserved", "café", False),
        ("raw_byte_search_text", "raw-\\xff", False),
    ]
    result = [{"id": name, "pattern": literal_glob(term), "selective": selective}
              for name, term, selective in specs]
    result += [
        {"id": "wildcard", "pattern": "report*2026*.pdf", "selective": False},
        {"id": "metadata", "pattern": "*engine*", "min_size": 200 * 1024**2,
         "min_time": 1700050000, "kind": 0, "selective": False},
        {"id": "subtree", "pattern": "*cobalt_needle*", "prefix": "projects/alpha/", "selective": True},
    ]
    return result


def statement(q, indexed, bounded=True):
    columns = "e.id,e.path,e.size,e.mtime,e.kind"
    table = "names n JOIN entries e ON e.id=n.rowid" if indexed else "entries e"
    conditions = [f"{'n' if indexed else 'e'}.name GLOB ?"]
    params = [q["pattern"]]
    for key, column in [("min_size", "size"), ("min_time", "mtime"), ("kind", "kind")]:
        if key in q:
            conditions.append(f"e.{column} {'=' if key == 'kind' else '>='} ?")
            params.append(q[key])
    if "prefix" in q:
        prefix = q["prefix"].encode()
        conditions += ["e.path >= ?", "e.path < ?"]
        params += [prefix, prefix[:-1] + bytes([prefix[-1] + 1])]
    return (f"SELECT {columns} FROM {table} WHERE " + " AND ".join(conditions) +
            (f" LIMIT {LIMIT}" if bounded else "")), params


def connect(path):
    c = sqlite3.connect(path)
    c.executescript(f"PRAGMA cache_size=-{CACHE_KIB}; PRAGMA mmap_size=0; PRAGMA temp_store=FILE;"
                    "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;")
    return c


def measured_queries(c, indexed):
    result = {}
    all_queries = queries()
    for q in all_queries:
        result[q["id"]] = dict(q, samples_ms=[], returned_rows=0,
                               plan=list(c.execute("EXPLAIN QUERY PLAN " + statement(q, indexed)[0],
                                                   statement(q, indexed)[1])))
    rng = random.Random(20261016)
    for repetition in range(REPEATS + 1):
        rng.shuffle(all_queries)
        for q in all_queries:
            start = time.perf_counter_ns()
            rows = c.execute(*statement(q, indexed)).fetchall()
            elapsed = (time.perf_counter_ns() - start) / 1e6
            value = result[q["id"]]
            value["returned_rows"] = len(rows)
            if repetition:
                value["samples_ms"].append(elapsed)
    for value in result.values():
        ordered = sorted(value["samples_ms"])
        value["median_ms"] = ordered[len(ordered) // 2]
        value["p95_ms"] = ordered[math.ceil(0.95 * len(ordered)) - 1]
    return result


def fingerprint(rows):
    # Order independent, streaming multiset digest, also validates path bytes.
    count, total, xor = 0, 0, 0
    for row in rows:
        rowid, path, size, mtime, kind = row
        if not isinstance(path, bytes):
            raise AssertionError("raw path was not returned as BLOB")
        h = int.from_bytes(hashlib.sha256(struct.pack("<QqqB", rowid, size, mtime, kind) + path).digest(), "big")
        count += 1
        total = (total + h) % (1 << 256)
        xor ^= h
    return {"count": count, "sum_sha256": f"{total:064x}", "xor_sha256": f"{xor:064x}"}


def verify(c):
    result = {}
    for q in queries():
        a = fingerprint(c.execute(*statement(q, False, False)))
        b = fingerprint(c.execute(*statement(q, True, False)))
        if a != b:
            raise AssertionError(f"index/scan mismatch: {q['id']}: {a} != {b}")
        result[q["id"]] = a
    if c.execute("PRAGMA integrity_check").fetchone() != ("ok",):
        raise AssertionError("SQLite integrity check failed")
    c.execute("INSERT INTO names(names,rank) VALUES('integrity-check',1)")
    c.commit()
    return result


def edge_oracle(c):
    # Independent Python glob and byte identity checks on fixed known records.
    edges = list(synthetic_rows(len(EDGE_NAMES)))
    for row in edges:
        got = c.execute("SELECT id,path,name,size,mtime,kind FROM entries WHERE id=?", (row[0],)).fetchone()
        if got != row:
            raise AssertionError("edge corpus byte/metadata round trip failed")
    result = {}
    for q in queries():
        if any(key in q for key in ("min_size", "min_time", "kind", "prefix")):
            continue
        expected = {row[0] for row in edges if fnmatch.fnmatchcase(row[2], q["pattern"])}
        sql, params = statement(q, True, False)
        got = {row[0] for row in c.execute(sql + " AND e.id<=?", params + [len(edges)])}
        if expected != got:
            raise AssertionError(f"independent glob oracle failed: {q['id']}: {expected} != {got}")
        result[q["id"]] = sorted(got)
    return result


def mixed_updates(c, count):
    changes = min(10000, count - len(EDGE_NAMES))
    stride = max(1, (count - len(EDGE_NAMES)) // changes)
    selected = [len(EDGE_NAMES) + 1 + i * stride for i in range(changes)]
    start = time.perf_counter()
    c.execute("BEGIN IMMEDIATE")
    for i, rowid in enumerate(selected):
        if i % 3 == 0:
            c.execute("UPDATE entries SET name='changed-'||name, path=CAST('renamed/'||id||'/changed-'||name AS BLOB),"
                      "size=size+1,mtime=mtime+1 WHERE id=?", (rowid,))
        elif i % 3 == 1:
            c.execute("DELETE FROM entries WHERE id=?", (rowid,))
        else:
            c.execute("INSERT INTO entries VALUES(?,?,?,?,?,?)",
                      (count + i + 1, f"new/fresh-{i}.txt".encode(), f"fresh-{i}.txt", i, 1800000000, 0))
    c.commit()
    elapsed = time.perf_counter() - start
    expected = (changes + 2) // 3, (changes + 1) // 3, changes // 3
    actual = (c.execute("SELECT count(*) FROM entries WHERE name GLOB 'changed-*'").fetchone()[0],
              sum(c.execute("SELECT count(*) FROM entries WHERE id=?", (rowid,)).fetchone()[0] == 0
                  for i, rowid in enumerate(selected) if i % 3 == 1),
              c.execute("SELECT count(*) FROM entries WHERE id>?", (count,)).fetchone()[0])
    if actual != expected:
        raise AssertionError(f"mixed update visibility failed: {actual} != {expected}")
    return {"operations": changes, "rename_and_metadata": expected[0], "delete": expected[1],
            "insert": expected[2], "transaction_s": elapsed}


def crash_worker(path):
    c = connect(path)
    c.execute("PRAGMA cache_size=-64")
    c.execute("BEGIN IMMEDIATE")
    c.execute("UPDATE entries SET name='uncommitted-'||name WHERE id<=5000")
    print("READY", flush=True)
    sys.stdin.read(1)  # Parent kills only this owned subprocess, before commit.


def crash_recovery(path):
    c = connect(path)
    before = fingerprint(c.execute("SELECT id,path,size,mtime,kind FROM entries"))
    first_name = c.execute("SELECT name FROM entries ORDER BY id LIMIT 1").fetchone()[0]
    c.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    c.close()
    child = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "--crash", str(path)],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        if not select.select([child.stdout], [], [], 15)[0] or child.stdout.readline() != b"READY\n":
            raise AssertionError("crash fixture did not become ready")
        wal_bytes = Path(str(path) + "-wal").stat().st_size
        if not wal_bytes:
            raise AssertionError("crash fixture did not spill uncommitted WAL pages")
    finally:
        child.kill()
        child.communicate(timeout=10)
    c = connect(path)
    after = fingerprint(c.execute("SELECT id,path,size,mtime,kind FROM entries"))
    if before != after or c.execute("SELECT name FROM entries ORDER BY id LIMIT 1").fetchone()[0] != first_name:
        raise AssertionError("uncommitted crash changed committed rows")
    if c.execute("SELECT count(*) FROM entries WHERE name GLOB 'uncommitted-*'").fetchone()[0]:
        raise AssertionError("uncommitted names survived crash")
    verify(c)
    c.close()
    return {"uncommitted_wal_bytes": wal_bytes, "committed_rows_preserved": True,
            "trigram_matches_full_scan_after_recovery": True}


def disk_bytes(path):
    return {suffix or "database": Path(str(path) + suffix).stat().st_size
            for suffix in ("", "-wal", "-shm") if Path(str(path) + suffix).exists()}


def worker(args):
    if sys.platform != "linux":
        raise RuntimeError("run this benchmark inside Linux/WSL")
    state = {"enumeration_errors": 0, "corpus_limit_reached": False, "enumeration_s": 0}
    with tempfile.TemporaryDirectory(prefix="openpilot-index-", dir=args.scratch) as tmp:
        path = Path(tmp) / "index.db"
        c = connect(path)
        c.executescript(SCHEMA)
        data = (filesystem_rows(args.corpus, args.corpus_limit, state) if args.corpus else synthetic_rows(args.rows))
        start = time.perf_counter()
        digest = hashlib.sha256()
        count = 0
        c.execute("BEGIN")
        for row in data:
            c.execute("INSERT INTO entries VALUES(?,?,?,?,?,?)", row)
            digest.update(canonical([row[0], row[1].hex(), *row[2:]]))
            count += 1
        c.commit()
        import_s = time.perf_counter() - start
        if count <= len(EDGE_NAMES):
            raise ValueError("corpus too small")
        plain_disk = disk_bytes(path)
        plain_queries = measured_queries(c, False)
        start = time.perf_counter()
        c.execute("CREATE VIRTUAL TABLE names USING fts5(name,content='entries',content_rowid='id',"
                  f"tokenize='trigram case_sensitive 1',detail='{args.detail}',columnsize=0)")
        c.execute("INSERT INTO names(names) VALUES('rebuild')")
        c.execute("INSERT INTO names(names) VALUES('optimize')")
        c.commit()
        c.executescript(TRIGGERS)
        index_build_s = time.perf_counter() - start
        indexed_disk_before_checkpoint = disk_bytes(path)
        c.execute("PRAGMA wal_checkpoint(TRUNCATE)")
        indexed_disk = disk_bytes(path)
        indexed_queries = measured_queries(c, True)
        correctness_before = verify(c)
        edges = edge_oracle(c) if not args.corpus else None
        native = {}
        c.close()
        if args.native_probe:
            for backend in ("scan", "trigram"):
                for q in ("rare_substring", "missing_long", "common_substring", "two_characters_missing"):
                    pattern = next(x["pattern"] for x in queries() if x["id"] == q)
                    process_start = time.perf_counter_ns()
                    proc = subprocess.run([args.native_probe, str(path), backend, pattern],
                                          check=True, capture_output=True, text=True, timeout=30)
                    native[backend + ":" + q] = json.loads(proc.stdout)
                    native[backend + ":" + q]["process_wall_ms"] = (time.perf_counter_ns() - process_start) / 1e6
            proc = subprocess.run([args.native_probe, str(path), "rebuild", "_"],
                                  check=True, capture_output=True, text=True, timeout=180)
            native["rebuild"] = json.loads(proc.stdout)
            expected_source = sqlite3.connect(":memory:").execute("SELECT sqlite_source_id()").fetchone()[0]
            if any(value["sqlite_source_id"] != expected_source for value in native.values()):
                raise AssertionError("native probe and Python must use the same SQLite source revision")
        c = connect(path)
        updates = mixed_updates(c, count)
        update_disk = disk_bytes(path)
        correctness_after = verify(c)
        c.close()
        recovery = crash_recovery(path)
        c = connect(path)
        c.execute("PRAGMA wal_checkpoint(TRUNCATE)")
        final_disk = disk_bytes(path)
        c.close()
        output = {
            "rows": count, "detail": args.detail, "corpus": args.corpus,
            "corpus_digest": digest.hexdigest(), "enumeration": state,
            "python": sys.version, "sqlite_version": sqlite3.sqlite_version,
            "python_sqlite_compile_options": [row[0] for row in sqlite3.connect(":memory:").execute("PRAGMA compile_options")],
            "sqlite_source_id": sqlite3.connect(":memory:").execute("SELECT sqlite_source_id()").fetchone()[0],
            "phases_s": {"generate_or_enumerate_and_import": import_s,
                         "trigram_build_and_optimize": index_build_s,
                         "storage_build_total": import_s - state["enumeration_s"] + index_build_s},
            "disk_bytes": {"plain": plain_disk, "indexed_before_checkpoint": indexed_disk_before_checkpoint,
                           "indexed_checkpointed": indexed_disk, "after_updates": update_disk, "final": final_disk},
            "scan": plain_queries, "trigram": indexed_queries, "native": native,
            "correctness": {"before": correctness_before, "after": correctness_after,
                            "independent_edge_oracle": edges, "mismatches": 0},
            "updates": updates, "crash_recovery": recovery,
            "python_worker_peak_rss_kib": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
            "temporary_database_removed_at_exit": True,
        }
        print(json.dumps(output))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="new JSON result file (refuses overwrite)")
    parser.add_argument("--scratch", default="/tmp", help="Linux filesystem directory for private databases")
    parser.add_argument("--sizes", nargs="+", type=int, default=[100000, 1000000])
    parser.add_argument("--details", nargs="+", choices=["none", "full"], default=["none", "full"])
    parser.add_argument("--corpus", help="optional read-only real filesystem corpus, no link following")
    parser.add_argument("--corpus-limit", type=int, default=300000)
    parser.add_argument("--native-probe", type=lambda s: str(Path(s).resolve()))
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--rows", type=int, default=100000, help=argparse.SUPPRESS)
    parser.add_argument("--detail", choices=["none", "full"], default="none", help=argparse.SUPPRESS)
    parser.add_argument("--crash", help=argparse.SUPPRESS)
    args = parser.parse_args()
    os.umask(0o077)
    if args.crash:
        crash_worker(args.crash)
        return
    if args.worker:
        worker(args)
        return
    if args.output is None or args.output.exists():
        parser.error("provide --output pointing to a new JSON file")
    if min(args.sizes) < 1000 or args.corpus_limit < 1000:
        parser.error("dataset sizes must be at least 1000")
    output = {"schema": "openpilot-wsl-index-benchmark/v1", "criteria": CRITERIA,
              "profile": {"workers": 1, "query_repeats": REPEATS, "cache_kib": CACHE_KIB,
                          "mmap": False, "journal": "WAL", "synchronous": "FULL", "result_limit": LIMIT,
                          "cache_posture": "warm OS cache; new native process/connection per pattern; no cache dropping",
                          "native_rss_method": "/proc/self/status VmHWM; inherited ru_maxrss reported only as diagnostic",
                          "scratch": str(Path(args.scratch).resolve()), "uname": platform.uname()._asdict(),
                          "scratch_mount": json.loads(subprocess.run(
                              ["findmnt", "--json", "--target", args.scratch, "--output", "TARGET,FSTYPE,OPTIONS"],
                              check=True, capture_output=True, text=True).stdout),
                          "cpu_count": os.cpu_count(), "python_sqlite_version": sqlite3.sqlite_version,
                          "driver_sha256": sha(__file__),
                          "native_probe_sha256": sha(args.native_probe) if args.native_probe else None},
              "cases": []}
    jobs = [(size, detail, None) for size in args.sizes for detail in args.details]
    if args.corpus:
        jobs += [(0, "none", str(Path(args.corpus).resolve()))]
    start = time.perf_counter()
    for size, detail, corpus in jobs:
        print(f"Benchmarking {corpus or str(size) + ' synthetic filenames'}, detail={detail}", flush=True)
        command = [sys.executable, str(Path(__file__).resolve()), "--worker", "--rows", str(size),
                   "--detail", detail, "--scratch", args.scratch, "--corpus-limit", str(args.corpus_limit)]
        if args.native_probe:
            command += ["--native-probe", args.native_probe]
        if corpus:
            command += ["--corpus", corpus]
        case_start = time.perf_counter()
        proc = subprocess.run(command, check=True, text=True, capture_output=True, timeout=1200)
        case = json.loads(proc.stdout)
        case["wall_s"] = time.perf_counter() - case_start
        case["criteria_evaluation"] = evaluate(case)
        output["cases"].append(case)
        print(f"  {case['rows']} rows; {case['disk_bytes']['indexed_checkpointed']['database']/1024**2:.1f} MiB; "
              f"build {case['phases_s']['storage_build_total']:.2f}s; updates {case['updates']['transaction_s']:.3f}s; "
              f"correctness mismatches {case['correctness']['mismatches']}", flush=True)
    output["wall_s"] = time.perf_counter() - start
    output["result_id"] = hashlib.sha256(canonical(output)).hexdigest()
    # O_EXCL preserves prior evidence even if another process chose the path.
    with args.output.open("x") as f:
        json.dump(output, f, indent=2)
        f.write("\n")
    print(f"Saved {args.output.resolve()}")


if __name__ == "__main__":
    main()
