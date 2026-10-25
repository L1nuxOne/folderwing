#!/usr/bin/env python3
"""Audit native test identities and print conservative source-impact plans.

This is a static inventory/selection tool, not a native test executor. It reads
the repository's direct tests/*.c translation units, never generated build data.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import re
import sys
import tempfile
import time
from pathlib import Path, PurePosixPath

SCHEMA = "openpilot-native-impact/v1"
REGISTRY = "tests/native-impact.json"
TOOL = "tools/native-impact.py"


class ImpactError(ValueError):
    pass


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def file_hash(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ImpactError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def load(path):
    return json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=unique_object)


def require(condition, message):
    if not condition:
        raise ImpactError(message)


def repo_path(repo, name):
    require(isinstance(name, str) and name, "empty/non-string source path")
    p = PurePosixPath(name)
    require(not p.is_absolute() and ".." not in p.parts and str(p) == name
            and "\\" not in name and ":" not in name, f"noncanonical repository path: {name}")
    path = repo / name
    require(path.resolve().is_relative_to(repo.resolve()), f"path escapes repository: {name}")
    require(path.exists(), f"missing source: {name}")
    return path


def mask_c(text):
    # Keep offsets/newlines, so comments and string/character literals cannot
    # manufacture test declarations, brace depth or entry-point references.
    token = r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\[\s\S]|[^"\\])*"|\'(?:\\[\s\S]|[^\'\\])*\''
    return re.sub(token, lambda m: re.sub(r"[^\n]", " ", m[0]), text)


def body_end(masked, opening):
    depth = 1
    for pos in range(opening + 1, len(masked)):
        depth += (masked[pos] == "{") - (masked[pos] == "}")
        if not depth:
            return pos + 1
    raise ImpactError("unclosed C function body")


def collect_module(text):
    masked = mask_c(text)
    entry = list(re.finditer(r"\bint\s+(?:WINAPI\s+)?(wmain|main|wWinMain)\s*\([^;{}]*\)\s*\{", masked))
    require(len(entry) == 1, "expected exactly one native entry point")
    functions = list(re.finditer(r"\bstatic\s+(?:\w+\s+|\*\s*)*?(test_\w+)\s*\([^;{}]*\)\s*\{", masked))
    names = [m[1] for m in functions]
    require(len(names) == len(set(names)), "duplicate native test definition")
    aliases = set(re.findall(r"^\s*#\s*define\s+\w+\s+(test_\w+)\s*$", masked, re.M))
    selectors, helpers = [], {}
    for match in functions:
        name = match[1]
        end = body_end(masked, match.end() - 1)
        if name in aliases:
            require(not re.search(r"\b(?:CHECK|VERIFY)\s*\(", masked[match.end():end]),
                    f"asserting function cannot be excluded as an API callback: {name}")
            helpers[name] = "API replacement callback; assertions are owned by the executable entry point"
            continue
        outside = masked[:match.start()] + " " * (end - match.start()) + masked[end:]
        # Supports direct calls and function-pointer case arrays. This checks
        # registration/reference, not C preprocessor reachability or execution.
        require(re.search(r"\b" + re.escape(name) + r"\b", outside), f"unreferenced native test: {name}")
        selectors.append(name)
    if not selectors:
        start = entry[0].end()
        end = body_end(masked, start - 1)
        require(re.search(r"\b(?:CHECK|VERIFY)\s*\(", masked[start:end]),
                "entry point has no recognized assertions")
        selectors = [entry[0][1]]
    return {"selectors": sorted(selectors), "helpers": helpers, "entrypoint": entry[0][1]}


def collect(repo):
    return {p.relative_to(repo).as_posix(): collect_module(p.read_text(encoding="utf-8"))
            for p in sorted((repo / "tests").glob("*.c"))}


def includes(repo, seeds):
    """Resolve repository-local quoted includes, including default macro includes.

    System/third-party headers remain externally declared compile inputs. Local
    include lookup matches this build's current directory and include/ search.
    """
    seen, pending = set(), list(seeds)
    while pending:
        name = pending.pop()
        if name in seen:
            continue
        path = repo_path(repo, name)
        seen.add(name)
        if path.suffix not in (".c", ".h", ".cpp"):
            continue
        text = path.read_text(encoding="utf-8")
        # Remove comments without interpreting comment markers inside literals.
        token = r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\[\s\S]|[^"\\])*"|\'(?:\\[\s\S]|[^\'\\])*\''
        text = re.sub(token, lambda m: re.sub(r"[^\n]", " ", m[0]) if m[0].startswith(('/*', '//')) else m[0], text)
        literals = re.findall(r'^\s*#\s*include\s+"([^"\n]+)"', text, re.M)
        macros = dict(re.findall(r'^\s*#\s*define\s+(\w+)\s+"([^"\n]+)"', text, re.M))
        literals += [macros[m] for m in re.findall(r'^\s*#\s*include\s+(\w+)\s*$', text, re.M) if m in macros]
        for literal in literals:
            found = next((p.resolve() for p in (path.parent / literal, repo / "include" / literal)
                          if p.is_file()), None)
            if found:
                require(found.is_relative_to(repo.resolve()), f"local include escapes repository: {name}: {literal}")
                pending.append(found.relative_to(repo.resolve()).as_posix())
    return seen


def audit(repo, registry):
    require(registry.get("schema") == SCHEMA, "unknown native-impact schema")
    inventory = collect(repo)
    modules = registry.get("modules", {})
    require(set(modules) == set(inventory), "native module inventory differs; reconcile additions/deletions")
    require(file_hash(repo / "build.ps1") == registry.get("build_sha256"),
            "build.ps1 changed; review native executable/compile-input mappings")
    require(sorted(p.relative_to(repo).as_posix() for p in (repo / "src").glob("*.c"))
            == registry.get("engine_sources"), "engine source glob changed; review compile-input mappings")
    for pattern, expected in registry.get("compile_globs", {}).items():
        require(not pattern.startswith('/') and '..' not in PurePosixPath(pattern).parts,
                "noncanonical compile glob")
        require(sorted(p.relative_to(repo).as_posix() for p in repo.glob(pattern)) == expected,
                f"compile glob changed; review mappings: {pattern}")
    actual = {m + "::" + n for m, item in inventory.items() for n in item["selectors"]}
    rows = registry.get("selectors", {})
    require(set(rows) == actual,
            f"selector inventory differs: missing={sorted(actual - rows.keys())}; stale={sorted(rows.keys() - actual)}")
    buckets = registry.get("primary_buckets", [])
    require(len(buckets) == len(set(buckets)) and buckets, "invalid primary taxonomy")
    require(registry.get("budgets") == {"unbound": 0, "unclassified": 0, "skips": 0,
                                       "xfail": 0, "quarantine": 0, "automatic_retries": 0},
            "native governance budgets must remain zero")
    build = (repo / "build.ps1").read_text()
    validated_sources = set()
    for module, item in modules.items():
        require(item.get("inventory") == inventory[module], f"sealed inventory changed: {module}")
        require(item.get("execution") and item.get("resources"), f"missing execution/fixture contract: {module}")
        executable = item.get("executable")
        require(isinstance(executable, str) and executable and '/' not in executable and '\\' not in executable
                and (executable.endswith(".exe") or item.get("platform") == "linux"),
                f"invalid native executable: {module}")
        built = re.search(r"Invoke-Zig\s+'" + re.escape(item["executable"]) + "'", build)
        recipe = item.get("build_recipe")
        require(built or isinstance(recipe, dict), f"native executable has no build target or fixture recipe: {module}")
        compile_inputs = item.get("compile_inputs", [])
        require(module in compile_inputs and len(compile_inputs) == len(set(compile_inputs)), f"invalid compile inputs: {module}")
        if not built:
            require(recipe.get("compiler") and isinstance(recipe.get("arguments"), list)
                    and set(compile_inputs) <= set(recipe["arguments"]), f"incomplete fixture build recipe: {module}")
        required = includes(repo, compile_inputs) | {"build.ps1", TOOL, REGISTRY}
        for name in inventory[module]["selectors"]:
            selector = module + "::" + name
            row = rows[selector]
            require(row.get("primary") in buckets, f"unclassified selector: {selector}")
            require(isinstance(row.get("owner"), str) and row["owner"].strip(), f"missing primary owner: {selector}")
            sources = row.get("sources", [])
            require(isinstance(sources, list) and sources and len(sources) == len(set(sources)), f"empty/duplicate bindings: {selector}")
            for source in sources:
                if source not in validated_sources:
                    repo_path(repo, source)
                    validated_sources.add(source)
            require(required <= set(sources), f"missing compile/include/governance bindings: {selector}: {sorted(required - set(sources))}")
    return {"schema": "openpilot-native-impact-audit/v1", "outcome": "passed",
            "native_modules": len(inventory), "native_selectors": len(actual),
            "inventory_digest": digest(inventory), "bindings_digest": digest(registry),
            "unbound": 0, "unclassified": 0, "skips": 0, "xfail": 0, "quarantine": 0, "automatic_retries": 0}


def select(repo, registry, changed):
    result = audit(repo, registry)
    changed = sorted(set(changed))
    require(changed, "supply at least one --changed repository path")
    matches = {}
    for source in changed:
        repo_path(repo, source)
        # A directory binding also owns its descendants, including new fixtures.
        owners = sorted(node for node, row in registry["selectors"].items()
                        if any(source == bound or source.startswith(bound + "/")
                               for bound in row["sources"]))
        require(owners, f"source has no native-impact binding: {source}")
        matches[source] = owners
    direct = sorted({node for owners in matches.values() for node in owners})
    modules = sorted({node.split("::")[0] for node in direct})
    expanded = sorted(node for node in registry["selectors"] if node.split("::")[0] in modules)
    result.update(schema="openpilot-native-impact-plan/v1", changed=changed,
                  direct_selectors=direct, selected_nodeids=expanded, selection_trace=matches,
                  execution_units={module: registry["modules"][module] for module in modules},
                  execution_policy="Whole executable: native binaries have no uniform per-selector CLI. Fixture requirements must be satisfied; missing resources are failures, not skips.",
                  executed=False)
    return result


def self_check():
    """Private audit fixtures; these are not additional product test selectors."""
    results = []
    with tempfile.TemporaryDirectory(prefix="openpilot-native-impact-") as temp:
        repo = Path(temp)
        for name, text in {"tests/a.c": "static void test_a(void){CHECK(1);}\nint wmain(void){test_a();return 0;}",
                           "build.ps1": "Invoke-Zig 'a.exe'", "src/x.c": "/* linked source */",
                           TOOL: "# tool", REGISTRY: "{}"}.items():
            p = repo / name; p.parent.mkdir(parents=True, exist_ok=True); p.write_text(text)
        model = {"schema": SCHEMA, "build_sha256": file_hash(repo / "build.ps1"),
                 "engine_sources": ["src/x.c"], "primary_buckets": ["app contract"],
                 "budgets": {"unbound": 0, "unclassified": 0, "skips": 0, "xfail": 0, "quarantine": 0, "automatic_retries": 0},
                 "modules": {"tests/a.c": {"inventory": collect(repo)["tests/a.c"], "executable": "a.exe", "execution": "whole executable", "resources": "private fixture", "compile_inputs": ["tests/a.c", "src/x.c"]}},
                 "selectors": {"tests/a.c::test_a": {"primary": "app contract", "owner": "A", "sources": ["tests/a.c", "src/x.c", "build.ps1", TOOL, REGISTRY]}}}
        def check(name, action, error=None):
            started = time.perf_counter()
            try:
                action()
            except ImpactError as exc:
                require(error is not None and error in str(exc), f"unexpected audit error in {name}: {exc}")
            else:
                require(error is None, f"audit failed to reject {name}")
            results.append({"case": name, "outcome": "passed", "call_ms": (time.perf_counter() - started) * 1000})
        def mutate(action):
            model_copy = copy.deepcopy(model); action(model_copy); return audit(repo, model_copy)
        check("valid_complete_mapping", lambda: audit(repo, model))
        check("linux_fixture_build_route", lambda: mutate(lambda d: d["modules"]["tests/a.c"].update(
            executable="a-linux", platform="linux", build_recipe={"compiler":"cc", "arguments":["tests/a.c", "src/x.c"]})))
        check("extensionless_requires_linux_platform", lambda: mutate(lambda d: d["modules"]["tests/a.c"].update(executable="a-linux")), "invalid native executable")
        check("linux_executable_path_rejected", lambda: mutate(lambda d: d["modules"]["tests/a.c"].update(executable="../a-linux", platform="linux")), "invalid native executable")
        check("missing_selector", lambda: mutate(lambda d: d["selectors"].clear()), "missing=")
        check("stale_selector", lambda: mutate(lambda d: d["selectors"].update({"tests/a.c::test_old": d["selectors"]["tests/a.c::test_a"]})), "stale=")
        check("missing_owner", lambda: mutate(lambda d: d["selectors"]["tests/a.c::test_a"].update(owner="")), "owner")
        check("unknown_primary", lambda: mutate(lambda d: d["selectors"]["tests/a.c::test_a"].update(primary="unknown")), "unclassified")
        check("missing_source", lambda: mutate(lambda d: d["selectors"]["tests/a.c::test_a"]["sources"].append("src/gone.c")), "missing source")
        check("duplicate_source", lambda: mutate(lambda d: d["selectors"]["tests/a.c::test_a"]["sources"].append("src/x.c")), "duplicate bindings")
        check("missing_compile_binding", lambda: mutate(lambda d: d["selectors"]["tests/a.c::test_a"]["sources"].remove("src/x.c")), "missing compile/include")
        check("raised_skip_budget", lambda: mutate(lambda d: d["budgets"].update(skips=1)), "budgets")
        check("duplicate_json_owner", lambda: unique_object([("owner", "A"), ("owner", "B")]), "duplicate JSON")
        check("duplicate_test_definition", lambda: collect_module("static void test_a(void){} static void test_a(void){} int main(void){test_a();}"), "duplicate native")
        check("unreferenced_test", lambda: collect_module("static void test_lost(void){CHECK(1);} int main(void){return 0;}"), "unreferenced")
        check("no_entrypoint", lambda: collect_module("static void test_a(void){CHECK(1);}"), "entry point")
        check("comments_and_literals_ignored", lambda: require(collect_module('/* static void test_fake(void){} */ static void test_a(void){CHECK("test_fake()");} int main(void){test_a();}')["selectors"] == ["test_a"], "comment/string manufactured a selector"))
        check("api_callback_not_a_test", lambda: require(collect_module("static void test_unole(void){stop();}\n#define OleUninitialize test_unole\nint wmain(void){CHECK(1);}")["selectors"] == ["wmain"], "callback counted as test"))
        check("asserting_callback_exclusion_rejected", lambda: collect_module("static void test_unole(void){CHECK(1);}\n#define OleUninitialize test_unole\nint wmain(void){CHECK(1);}"), "asserting function")
        check("unknown_source_rejected", lambda: select(repo, model, ["build.ps1", "tests"]), "no native-impact")
        check("traversal_path_rejected", lambda: repo_path(repo, "../outside"), "noncanonical")
        check("absolute_path_rejected", lambda: repo_path(repo, "/outside"), "noncanonical")
        check("selection_is_union_only", lambda: require(set(select(repo, model, ["src/x.c"])["selected_nodeids"]) <= set(select(repo, model, ["src/x.c", "build.ps1"])["selected_nodeids"]), "selection narrowed"))
        check("selection_never_claims_execution", lambda: require(select(repo, model, ["src/x.c"])["executed"] is False, "plan claimed execution"))
        check("nonvoid_test_identity", lambda: require(collect_module("static int test_b(void){CHECK(1);return 0;} int main(void){test_b();}")["selectors"] == ["test_b"], "nonvoid test lost"))
        check("literal_is_not_registration", lambda: collect_module('static void test_lost(void){CHECK(1);} int main(void){puts("test_lost");return 0;}'), "unreferenced")
        check("missing_build_route", lambda: mutate(lambda d: d['modules']['tests/a.c'].update(executable='gone.exe')), "no build target")
        check("incomplete_fixture_recipe", lambda: mutate(lambda d: d['modules']['tests/a.c'].update(executable='gone.exe', build_recipe={'compiler':'zig','arguments':[]})), "incomplete fixture")
        # Separate semantic inputs select separate cases; the actual execution
        # unit still expands to both cases, and unions cannot narrow either.
        original = (repo / 'tests/a.c').read_text()
        (repo / 'tests/a.c').write_text('static void test_a(void){CHECK(1);} static void test_b(void){CHECK(1);} int wmain(void){test_a();test_b();return 0;}')
        for name in ('one.md', 'two.md'):
            (repo / name).write_text('semantic input')
        pair = copy.deepcopy(model)
        pair['modules']['tests/a.c']['inventory'] = collect(repo)['tests/a.c']
        pair['selectors']['tests/a.c::test_b'] = copy.deepcopy(pair['selectors']['tests/a.c::test_a'])
        pair['selectors']['tests/a.c::test_a']['sources'].append('one.md')
        pair['selectors']['tests/a.c::test_b']['sources'].append('two.md')
        check('whole_executable_expansion', lambda: require(select(repo,pair,['one.md'])['direct_selectors']==['tests/a.c::test_a'] and select(repo,pair,['one.md'])['selected_nodeids']==['tests/a.c::test_a','tests/a.c::test_b'], 'execution unit narrowed'))
        check('disjoint_dependency_union', lambda: require(set(select(repo,pair,['one.md','two.md'])['direct_selectors'])=={'tests/a.c::test_a','tests/a.c::test_b'}, 'owning edge lost'))
        pair['selectors']['tests/a.c::test_a']['sources'].append('tests')
        (repo/'tests/new-fixture.bin').write_bytes(b'fixture')
        check('directory_fixture_binding', lambda: require(select(repo,pair,['tests/new-fixture.bin'])['direct_selectors']==['tests/a.c::test_a'], 'fixture child not selected'))
        (repo / 'tests/a.c').write_text(original)
        (repo / "src/new.c").write_text("/* added to wildcard compile */")
        check("engine_glob_drift", lambda: audit(repo, model), "engine source glob changed")
        (repo / "src/new.c").unlink()
        (repo / "tests/new.c").write_text("int main(void){CHECK(1);}")
        check("new_native_module", lambda: audit(repo, model), "module inventory differs")
        (repo / "tests/new.c").unlink()
        (repo / "build.ps1").write_text("Invoke-Zig 'different.exe'")
        check("build_drift", lambda: audit(repo, model), "build.ps1 changed")
        (repo / "build.ps1").write_text("Invoke-Zig 'a.exe'")
        (repo / "src/header.h").write_text("/* semantic shared contract */")
        (repo / "src/x.c").write_text('#include "header.h"\n')
        check("new_header_binding_required", lambda: audit(repo, model), "missing compile/include")
    return {"schema": "openpilot-native-impact-self-check/v1", "outcome": "passed", "checks": len(results), "cases": results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("inventory", "audit", "select", "self-check"))
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--changed", nargs="+")
    args = parser.parse_args()
    try:
        repo = args.repo.resolve()
        if args.command == "self-check":
            result = self_check()
        elif args.command == "inventory":
            result = {"schema": "openpilot-native-inventory/v1", "modules": collect(repo)}
        else:
            registry = load(repo / REGISTRY)
            result = select(repo, registry, args.changed or []) if args.command == "select" else audit(repo, registry)
        print(json.dumps(result, indent=2))
        return 0
    except (ImpactError, OSError, ValueError, TypeError, KeyError) as exc:
        print(json.dumps({"outcome": "error", "error": str(exc)}), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
