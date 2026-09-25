#!/usr/bin/env python3
"""CI helpers for the PGO experiment (.github/workflows/pgo-experiment.yml).

  train-clice  run an instrumented clice build's workload, merge a .profdata
  train-clang  build clice with an instrumented clang, merge a .profdata
  workloads    materialize the benchmark CDBs (abseil, llvm Sema)
  bench        run pipeline_benchmark of several builds interleaved, report
"""

import argparse
import json
import os
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

STAGES = ["preprocess_ms", "preprocess_tokens_ms", "parse_ms", "index_ms", "pch_build_ms", "parse_pch_ms"]


def run(args, **kwargs):
    print("+", " ".join(str(a) for a in args), flush=True)
    return subprocess.run([str(a) for a in args], **kwargs)


def merge_profiles(raw_dir: Path, out: Path) -> None:
    raws = sorted(raw_dir.glob("*.profraw"))
    if not raws:
        sys.exit(f"no .profraw under {raw_dir}")
    total = sum(r.stat().st_size for r in raws)
    print(f"merging {len(raws)} raw profiles ({total / 1048576:.0f} MB)")
    out.parent.mkdir(parents=True, exist_ok=True)
    # Tests kill some of their child processes mid-write; their truncated
    # profiles are skipped rather than failing the merge.
    run(["llvm-profdata", "merge", "--sparse", "--failure-mode=all", "-o", out, *raws], check=True)
    run(["llvm-profdata", "show", out], check=False)
    listing = run(["llvm-profdata", "show", "--all-functions", out], capture_output=True, text=True).stdout
    static = [line.strip() for line in listing.splitlines() if ";" in line][:3]
    print("static function names:", *static, sep="\n  ")


def train_clice(args) -> None:
    build = Path(args.build).resolve()
    raw_dir = build / "pgo-raw"
    shutil.rmtree(raw_dir, ignore_errors=True)
    raw_dir.mkdir(parents=True)
    env = dict(os.environ, LLVM_PROFILE_FILE=str(raw_dir / "%m-%p.profraw"))
    env["CLICE_TEST_DATA_DIR"] = str(Path("tests/data").resolve())

    start = time.time()
    # clice's own sources: the dependencies' largest files (simdjson, lmdb)
    # would otherwise crowd out the LLVM-heavy TUs.
    run([build / "bin/pipeline_benchmark", "--limit", args.limit, "--filter", args.filter,
         "--json", build / "pgo-train.json", build / "compile_commands.json"], env=env, check=True)
    print(f"pipeline_benchmark training: {time.time() - start:.0f} s", flush=True)
    if args.unit_tests:
        start = time.time()
        result = run([build / "bin/unit_tests"], env=env)
        print(f"unit_tests training: {time.time() - start:.0f} s (exit {result.returncode})", flush=True)
    merge_profiles(raw_dir, Path(args.out).resolve())


def train_clang(args) -> None:
    """The instrumented clang has replaced the environment's clang-23, so an
    ordinary configure + build of clice is the training run."""
    raw_dir = Path(args.raw_dir).resolve()
    shutil.rmtree(raw_dir, ignore_errors=True)
    raw_dir.mkdir(parents=True)
    env = dict(os.environ, LLVM_PROFILE_FILE=str(raw_dir / "clang-%m.profraw"), CCACHE_DISABLE="1")
    build = Path("build/clang-train")
    run(["cmake", "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
         "-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain.cmake", "-DCLICE_ENABLE_TEST=ON",
         "-DCMAKE_C_COMPILER_LAUNCHER=", "-DCMAKE_CXX_COMPILER_LAUNCHER="], env=env, check=True)
    start = time.time()
    result = run(["ninja", "-C", build, "-k", "0", *args.targets], env=env)
    print(f"clang training build: {time.time() - start:.0f} s (exit {result.returncode})", flush=True)
    merge_profiles(raw_dir, Path(args.out).resolve())


def workloads(args) -> None:
    root = Path(args.dir).resolve()
    root.mkdir(parents=True, exist_ok=True)

    abseil = root / "abseil"
    if args.only == "llvm":
        pass
    elif not abseil.exists():
        run(["git", "clone", "--depth", "1", "--branch", "20250814.1",
             "https://github.com/abseil/abseil-cpp.git", abseil], check=True)
    compilers = [f"-DCMAKE_C_COMPILER={args.cc}", f"-DCMAKE_CXX_COMPILER={args.cxx}"]
    if args.flags:
        compilers += [f"-DCMAKE_C_FLAGS={args.flags}", f"-DCMAKE_CXX_FLAGS={args.flags}"]
    if args.only != "llvm":
        run(["cmake", "-S", abseil, "-B", abseil / "build", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
             "-DCMAKE_CXX_STANDARD=20", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", "-DABSL_BUILD_TESTING=OFF",
             *compilers], check=True)

    if args.skip_llvm or args.only == "abseil":
        return
    llvm = root / "llvm"
    if not llvm.exists():
        run(["git", "clone", "--depth", "1", "--branch", "llvmorg-23.1.1",
             "https://github.com/llvm/llvm-project.git", llvm], check=True)
    run(["cmake", "-S", llvm / "llvm", "-B", llvm / "build", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", "-DLLVM_ENABLE_PROJECTS=clang",
         "-DLLVM_TARGETS_TO_BUILD=X86", *compilers],
        check=True)
    # Sema's TUs include tablegen output: build every tablegen target so no
    # TU stops at a missing generated header.
    listing = run(["ninja", "-C", llvm / "build", "-t", "targets", "all"],
                  check=True, capture_output=True, text=True).stdout
    names = {line.split(":")[0] for line in listing.splitlines()}
    gens = sorted(n for n in names
                  if "/" not in n and not n.startswith("install-")
                  and n.endswith(("TableGen", "_gen", "tablegen-targets")))
    print(f"tablegen targets: {len(gens)}")
    run(["ninja", "-C", llvm / "build", *gens], check=True)


def bench(args) -> None:
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    variants = dict(v.split("=", 1) for v in args.variant)
    variant_env: dict[str, dict[str, str]] = {}
    for spec in args.env:
        name, assignment = spec.split("=", 1)
        key, value = assignment.split("=", 1)
        variant_env.setdefault(name, {})[key] = value
    loads = []
    for spec in args.workload:
        # CDB may be a Windows path with a drive colon.
        name, rest = spec.split(":", 1)
        cdb, flt, limit = rest.rsplit(":", 2)
        loads.append((name, Path(cdb).resolve(), flt, limit))

    # results[variant][workload] = list of per-round {stage: total ms}
    results = {v: {w[0]: [] for w in loads} for v in variants}
    walls = {v: {w[0]: [] for w in loads} for v in variants}
    errors = {v: {} for v in variants}
    names = list(variants)
    for round_index in range(args.rounds):
        order = names[round_index % len(names):] + names[:round_index % len(names)]
        for variant in order:
            exe = Path(variants[variant]) / "bin/pipeline_benchmark"
            if not exe.exists():
                exe = exe.with_suffix(".exe")
            for name, cdb, flt, limit in loads:
                report = out / f"{variant}-{name}-{round_index}.json"
                cmd = [exe, "--runs", "1", "--limit", limit, "--json", report]
                if flt:
                    cmd += ["--filter", flt]
                start = time.time()
                result = run(cmd + [cdb], stdout=subprocess.DEVNULL,
                             env=dict(os.environ, **variant_env.get(variant, {})))
                if result.returncode != 0:
                    print(f"warning: {variant} {name} exited {result.returncode}", flush=True)
                walls[variant][name].append(time.time() - start)
                files = json.loads(report.read_text())["files"]
                totals = {s: sum(f[s] for f in files if f.get(s, -1) >= 0) for s in STAGES}
                results[variant][name].append(totals)
                errors[variant][name] = sum(1 for f in files if f.get("error"))
                print(f"round {round_index} {variant} {name}: wall {walls[variant][name][-1]:.1f} s "
                      f"parse {totals['parse_ms'] / 1000:.1f} s", flush=True)

    base = names[0]
    lines = [f"Rounds: {args.rounds}; per stage: median over rounds of the summed per-file time (s); "
             f"ratio = time / {base} (lower is faster).", ""]
    summary = {}
    for name, _, flt, limit in loads:
        lines += [f"### {name} (limit {limit}{', filter ' + flt if flt else ''})", ""]
        header = "| variant | wall | " + " | ".join(s.removesuffix("_ms") for s in STAGES) + " | errors |"
        lines += [header, "|" + "---|" * (len(STAGES) + 3)]
        base_wall = statistics.median(walls[base][name])
        base_stage = {s: statistics.median(r[s] for r in results[base][name]) for s in STAGES}
        for variant in names:
            wall = statistics.median(walls[variant][name])
            stage = {s: statistics.median(r[s] for r in results[variant][name]) for s in STAGES}
            cells = [f"{wall:.1f} ({wall / base_wall:.3f})"]
            for s in STAGES:
                ratio = stage[s] / base_stage[s] if base_stage[s] else float("nan")
                cells.append(f"{stage[s] / 1000:.2f} ({ratio:.3f})")
            lines.append(f"| {variant} | " + " | ".join(cells) + f" | {errors[variant][name]} |")
            summary.setdefault(variant, {})[name] = {"wall": walls[variant][name], "stages": results[variant][name]}
        lines.append("")
    text = "\n".join(lines)
    print(text)
    (out / "summary.md").write_text(text)
    (out / "summary.json").write_text(json.dumps(summary, indent=1))
    if "GITHUB_STEP_SUMMARY" in os.environ:
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as f:
            f.write("## pipeline_benchmark\n\n" + text + "\n")


def clang_bench(args) -> None:
    """Time a from-scratch build of clice's `clice` target with each clang.
    Each variant's binary replaces the environment's clang-23 in turn; the
    environment's own clang is the variant named "conda"."""
    prefix = Path(os.environ["CONDA_PREFIX"])
    target = prefix / "bin/clang-23"
    original = Path(args.out).resolve() / "clang-23.conda"
    original.parent.mkdir(parents=True, exist_ok=True)
    if not original.exists():
        shutil.copy2(target, original)
    variants = {"conda": original}
    variants.update({k: Path(v).resolve() for k, v in (x.split("=", 1) for x in args.variant)})
    env = dict(os.environ, CCACHE_DISABLE="1")
    times = {name: [] for name in variants}
    names = list(variants)
    for round_index in range(args.rounds):
        order = names[round_index % len(names):] + names[:round_index % len(names)]
        for name in order:
            target.unlink()
            shutil.copy2(variants[name], target)
            target.chmod(0o755)
            build = Path(f"build/clang-bench-{name}-{round_index}").resolve()
            shutil.rmtree(build, ignore_errors=True)
            if args.project:
                # Another project than the one the profile was trained on.
                configure = ["cmake", "-S", args.project, "-B", build, "-G", "Ninja",
                             "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_STANDARD=20",
                             "-DABSL_BUILD_TESTING=OFF",
                             "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++"]
                targets = []
            else:
                configure = ["cmake", "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                             "-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain.cmake", "-DCMAKE_C_COMPILER_LAUNCHER=",
                             "-DCMAKE_CXX_COMPILER_LAUNCHER="]
                targets = ["clice"]
            run(configure, env=env, check=True, stdout=subprocess.DEVNULL)
            start = time.time()
            run(["ninja", "-C", build, *targets], env=env, check=True, stdout=subprocess.DEVNULL)
            times[name].append(time.time() - start)
            print(f"round {round_index} {name}: {times[name][-1]:.1f} s", flush=True)
            shutil.rmtree(build, ignore_errors=True)
    base = statistics.median(times[names[0]])
    what = f"`ninja` of {args.project} (Release, library)" if args.project else "`ninja clice` (RelWithDebInfo)"
    lines = [f"{what} from scratch, no ccache; median of {args.rounds} rounds.", "",
             "| clang | seconds | ratio |", "|---|---|---|"]
    for name in names:
        t = statistics.median(times[name])
        lines.append(f"| {name} | {t:.1f} ({', '.join(f'{x:.0f}' for x in times[name])}) | {t / base:.3f} |")
    text = "\n".join(lines)
    print(text)
    (Path(args.out) / "summary.md").write_text(text)
    if "GITHUB_STEP_SUMMARY" in os.environ:
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as f:
            f.write("## clang build speed\n\n" + text + "\n")
    target.unlink()
    shutil.copy2(original, target)


def match_report(args) -> None:
    """Recompile TUs of a configured profile-using LLVM build with the PGO
    diagnostics on, once per profile, and record every function the profile
    has no data for or no longer matches (hash mismatch), comdat/weak ones
    (header templates and inlines, silent by default) included, with the
    count the mismatch discards."""
    import re
    import shlex
    pattern = re.compile(r"(function control flow change detected \(hash mismatch\)|no profile data available for function)"
                         r" (\S+) Hash = \d+ up to (\d+) count discarded")
    # A frontend profile reports per TU only: "of N functions, M have mismatched data" / "... no data".
    fe_pattern = re.compile(r"profile data may be (out of date|incomplete): of (\d+) functions?, (\d+) (?:has|have)")
    build = Path(args.build).resolve()
    entries = json.loads((build / "compile_commands.json").read_text())
    profiles = dict(p.split("=", 1) for p in args.profile)
    remaps = dict(r.split("=", 1) for r in args.remap)
    result = {"target": args.target, "profiles": {}}
    for pname, ppath in profiles.items():
        ppath = str(Path(ppath).resolve())
        remap = [f"-fprofile-remapping-file={Path(remaps[pname]).resolve()}"] if pname in remaps else []
        records = []
        for want in args.files:
            entry = next((e for e in entries if e["file"].replace("\\", "/").endswith(want)), None)
            if entry is None:
                print(f"{want}: not in the CDB")
                continue
            argv = entry.get("arguments") or shlex.split(entry["command"])
            argv = [f"-fprofile-instr-use={ppath}" if a.startswith("-fprofile-instr-use=") else a
                    for a in argv if a != "-w"]
            # LLVM's precompiled headers are not built here (no full build);
            # the -include of the PCH's source header that follows stays, so
            # the TU sees the same declarations textually.
            kept = []
            i = 0
            while i < len(argv):
                if argv[i] == "-Xclang" and i + 3 < len(argv) and argv[i + 1] == "-include-pch":
                    i += 4
                    continue
                kept.append(argv[i])
                i += 1
            argv = kept
            if "-o" in argv:
                argv[argv.index("-o") + 1] = os.devnull
            argv = [a for a in argv if not a.startswith("-fprofile-remapping-file=")] + remap
            argv += ["-Wno-everything", "-Wbackend-plugin", "-Wprofile-instr-out-of-date", "-Wprofile-instr-missing",
                     "-mllvm", "-pgo-warn-missing-function", "-mllvm", "-no-pgo-warn-mismatch-comdat-weak=false"]
            out = subprocess.run(argv, cwd=entry["directory"], capture_output=True, text=True)
            if out.returncode != 0:
                print(f"{want}: exit {out.returncode}\n{out.stderr[-1500:]}")
            for m in pattern.finditer(out.stderr):
                kind = "mismatch" if "hash mismatch" in m.group(1) else "missing"
                records.append({"tu": want, "kind": kind, "name": m.group(2), "count": int(m.group(3))})
            for m in fe_pattern.finditer(out.stderr):
                kind = "fe-mismatch" if m.group(1) == "out of date" else "fe-missing"
                records.append({"tu": want, "kind": kind, "functions": int(m.group(2)), "count": int(m.group(3))})
        result["profiles"][pname] = records
        mism = [r for r in records if r["kind"] == "mismatch"]
        fe = [r for r in records if r["kind"] == "fe-mismatch"]
        fe_missing = [r for r in records if r["kind"] == "fe-missing"]
        print(f"{args.target} {pname}: IR {len(mism)} mismatched ({sum(r['count'] for r in mism)} counts); "
              f"FE {sum(r['count'] for r in fe)} of {sum(r['functions'] for r in fe)} functions mismatched, "
              f"{sum(r['count'] for r in fe_missing)} of {sum(r['functions'] for r in fe_missing)} without data")
    Path(args.json).write_text(json.dumps(result))


def category(demangled: str, name: str) -> str:
    if demangled.startswith(("std::", "void std::", "bool std::")) or "std::__1::" in demangled.split("(")[0]:
        return "libc++"
    head = demangled.split("(")[0]
    for prefix, label in (("clang::", "clang"), ("llvm::", "llvm")):
        if prefix in head:
            return label
    if ";" in name:
        path = name.split(";")[0]
        return "clang" if path.startswith("clang/") else "llvm" if path.startswith("llvm/") else "other"
    return "other"


def match_analyze(args) -> None:
    names = set()
    data = []
    for path in sorted(Path(args.dir).rglob("*.json")):
        d = json.loads(path.read_text())
        data.append(d)
        for records in d["profiles"].values():
            names.update(r["name"].split(";")[-1] for r in records if "name" in r)
    ordered = sorted(names)
    demangled = run(["llvm-cxxfilt"], input="\n".join(ordered), capture_output=True, text=True).stdout.splitlines()
    dm = dict(zip(ordered, demangled))
    lines = ["Discarded counts: the profile's execution counts of the functions whose CFG no longer matches",
             "(every function of the TUs, header templates and inlines included). The category columns split",
             "each row's discarded counts by where the function comes from.", "",
             "| target | profile | mismatched fns | discarded counts | clang | llvm | libc++ | other |",
             "|---|---|---|---|---|---|---|---|"]
    hottest = {}
    for d in sorted(data, key=lambda d: d["target"]):
        for pname, records in d["profiles"].items():
            mism = [r for r in records if r["kind"] == "mismatch"]
            total = sum(r["count"] for r in mism)
            by = {"clang": 0, "llvm": 0, "libc++": 0, "other": 0}
            for r in mism:
                by[category(dm.get(r["name"].split(";")[-1], r["name"]), r["name"])] += r["count"]
            share = lambda c: f"{100 * c / total:.0f}%" if total else "-"
            lines.append(f"| {d['target']} | {pname} | {len(mism)} | {total:.3g} | " +
                         " | ".join(share(by[k]) for k in ("clang", "llvm", "libc++", "other")) + " |")
            hottest[(d["target"], pname)] = sorted(mism, key=lambda r: -r["count"])[:8]
    lines += ["", "Frontend-PGO profiles (clang reports per TU, no per-function counts):", "",
              "| target | profile | functions | mismatched | without data |", "|---|---|---|---|---|"]
    for d in sorted(data, key=lambda d: d["target"]):
        for pname, records in d["profiles"].items():
            fe_m = [r for r in records if r["kind"] == "fe-mismatch"]
            fe_n = [r for r in records if r["kind"] == "fe-missing"]
            if not fe_m and not fe_n:
                continue
            total = max(sum(r["functions"] for r in fe_m), sum(r["functions"] for r in fe_n))
            lines.append(f"| {d['target']} | {pname} | {total} | {sum(r['count'] for r in fe_m)} | "
                         f"{sum(r['count'] for r in fe_n)} |")
    lines.append("")
    for (target, pname), recs in hottest.items():
        if not recs or pname != "x86":
            continue
        lines += [f"### hottest mismatches: {target}, x86 profile", ""]
        for r in recs:
            name = dm.get(r["name"].split(";")[-1], r["name"])
            lines.append(f"- {r['count']:.3g}  `{name[:140]}`  ({r['tu']})")
        lines.append("")
    text = "\n".join(lines)
    print(text)
    if "GITHUB_STEP_SUMMARY" in os.environ:
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as f:
            f.write(text + "\n")


def tablegen(args) -> None:
    build = Path(args.build)
    listing = run(["ninja", "-C", build, "-t", "targets", "all"], check=True, capture_output=True, text=True).stdout
    targets = {line.split(":")[0] for line in listing.splitlines()}
    gens = sorted(n for n in targets if "/" not in n and not n.startswith("install-")
                  and n.endswith(("TableGen", "_gen", "tablegen-targets")))
    run(["ninja", "-C", build, *gens], check=True)


def bench_aggregate(args) -> None:
    """Median over parallel jobs of each variant's ratio to the base. Every
    job ran all variants on one machine, so a ratio never mixes machines."""
    ratios: dict[str, dict[str, dict[str, list[float]]]] = {}
    order: list[str] = []
    jobs = 0
    for path in sorted(Path(args.dir).rglob("summary.json")):
        jobs += 1
        summary = json.loads(path.read_text())
        base = summary[args.base]
        for variant, loads in summary.items():
            if variant not in order:
                order.append(variant)
            for load, data in loads.items():
                if load not in base:
                    continue
                cell = ratios.setdefault(load, {}).setdefault(variant, {})
                cell.setdefault("wall", []).append(
                    statistics.median(data["wall"]) / statistics.median(base[load]["wall"]))
                for stage in STAGES:
                    b = statistics.median(r[stage] for r in base[load]["stages"])
                    v = statistics.median(r[stage] for r in data["stages"])
                    if b:
                        cell.setdefault(stage, []).append(v / b)
    lines = [f"Median over {jobs} parallel jobs of time / {args.base} (each job one machine; lower is faster;"
             " spread in brackets).", ""]
    for load, cells in ratios.items():
        n = len(next(iter(cells.values()))["wall"])
        lines += [f"### {load} ({n} jobs)", "",
                  "| variant | wall | " + " | ".join(s.removesuffix("_ms") for s in STAGES) + " |",
                  "|" + "---|" * (len(STAGES) + 2)]
        for variant in order:
            cell = cells.get(variant)
            if not cell:
                continue
            fmt = lambda xs: f"{statistics.median(xs):.3f} ({min(xs):.2f}–{max(xs):.2f})" if xs else "-"
            lines.append(f"| {variant} | {fmt(cell['wall'])} | " + " | ".join(fmt(cell.get(s, [])) for s in STAGES) + " |")
        lines.append("")
    text = "\n".join(lines)
    print(text)
    if "GITHUB_STEP_SUMMARY" in os.environ:
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as f:
            f.write(text + "\n")


def main() -> None:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("train-clice")
    p.add_argument("--build", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--limit", default="30")
    p.add_argument("--filter", default="clice/src/")
    p.add_argument("--unit-tests", action="store_true")
    p.set_defaults(func=train_clice)

    p = sub.add_parser("train-clang")
    p.add_argument("--raw-dir", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--targets", nargs="+", default=["clice"])
    p.set_defaults(func=train_clang)

    p = sub.add_parser("workloads")
    p.add_argument("--dir", required=True)
    p.add_argument("--skip-llvm", action="store_true")
    p.add_argument("--cc", default="clang")
    p.add_argument("--cxx", default="clang++")
    p.add_argument("--flags", default="", help="CMAKE_C_FLAGS / CMAKE_CXX_FLAGS of the workloads")
    p.add_argument("--only", choices=["abseil", "llvm"], help="Materialize one workload only")
    p.set_defaults(func=workloads)

    p = sub.add_parser("bench")
    p.add_argument("--variant", action="append", required=True, help="NAME=DIR (DIR/bin/pipeline_benchmark)")
    p.add_argument("--workload", action="append", required=True, help="NAME:CDB:FILTER:LIMIT")
    p.add_argument("--rounds", type=int, default=3)
    p.add_argument("--out", required=True)
    p.add_argument("--env", action="append", default=[], help="NAME=KEY=VALUE for one variant's runs")
    p.set_defaults(func=bench)

    p = sub.add_parser("bench-aggregate")
    p.add_argument("--dir", required=True)
    p.add_argument("--base", required=True)
    p.set_defaults(func=bench_aggregate)

    p = sub.add_parser("match-report")
    p.add_argument("--build", required=True, help="LLVM build directory (compile_commands.json)")
    p.add_argument("--files", nargs="+", required=True)
    p.add_argument("--profile", action="append", required=True, help="NAME=PATH")
    p.add_argument("--remap", action="append", default=[], help="NAME=FILE: a remapping file for profile NAME")
    p.add_argument("--target", default="")
    p.add_argument("--json", default="match.json")
    p.set_defaults(func=match_report)

    p = sub.add_parser("match-analyze")
    p.add_argument("--dir", required=True)
    p.set_defaults(func=match_analyze)

    p = sub.add_parser("tablegen")
    p.add_argument("--build", required=True)
    p.set_defaults(func=tablegen)

    p = sub.add_parser("clang-bench")
    p.add_argument("--variant", action="append", default=[], help="NAME=PATH of a clang-23 binary")
    p.add_argument("--project", help="Build this CMake project instead of clice")
    p.add_argument("--rounds", type=int, default=2)
    p.add_argument("--out", required=True)
    p.set_defaults(func=clang_bench)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
