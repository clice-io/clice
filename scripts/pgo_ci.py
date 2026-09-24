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
    if not abseil.exists():
        run(["git", "clone", "--depth", "1", "--branch", "20250814.1",
             "https://github.com/abseil/abseil-cpp.git", abseil], check=True)
    compilers = [f"-DCMAKE_C_COMPILER={args.cc}", f"-DCMAKE_CXX_COMPILER={args.cxx}"]
    if args.flags:
        compilers += [f"-DCMAKE_C_FLAGS={args.flags}", f"-DCMAKE_CXX_FLAGS={args.flags}"]
    run(["cmake", "-S", abseil, "-B", abseil / "build", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         "-DCMAKE_CXX_STANDARD=20", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", "-DABSL_BUILD_TESTING=OFF",
         *compilers], check=True)

    if args.skip_llvm:
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
            build = Path(f"build/clang-bench-{name}-{round_index}")
            shutil.rmtree(build, ignore_errors=True)
            run(["cmake", "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                 "-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain.cmake", "-DCMAKE_C_COMPILER_LAUNCHER=",
                 "-DCMAKE_CXX_COMPILER_LAUNCHER="], env=env, check=True, stdout=subprocess.DEVNULL)
            start = time.time()
            run(["ninja", "-C", build, "clice"], env=env, check=True, stdout=subprocess.DEVNULL)
            times[name].append(time.time() - start)
            print(f"round {round_index} {name}: {times[name][-1]:.1f} s", flush=True)
            shutil.rmtree(build, ignore_errors=True)
    base = statistics.median(times[names[0]])
    lines = [f"`ninja clice` from scratch, RelWithDebInfo, no ccache; median of {args.rounds} rounds.", "",
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
    """Recompile a few TUs of a profile-using LLVM build with the profile
    diagnostics on and count the functions the profile misses or no longer
    matches (hash mismatch)."""
    import shlex
    build = Path(args.build).resolve()
    entries = json.loads((build / "compile_commands.json").read_text())
    lines = ["| TU | no profile data | hash mismatch |", "|---|---|---|"]
    totals = [0, 0]
    for want in args.files:
        entry = next((e for e in entries if e["file"].replace("\\", "/").endswith(want)), None)
        if entry is None:
            lines.append(f"| {want} | not in CDB | |")
            continue
        argv = entry.get("arguments") or shlex.split(entry["command"])
        argv = [a for a in argv if a != "-w"]
        if "-o" in argv:
            argv[argv.index("-o") + 1] = os.devnull
        argv += ["-Wno-everything", "-Wbackend-plugin", "-mllvm", "-pgo-warn-missing-function"]
        result = subprocess.run(argv, cwd=entry["directory"], capture_output=True, text=True)
        missing = result.stderr.count("No profile data available for function")
        mismatch = result.stderr.count("hash mismatch")
        totals[0] += missing
        totals[1] += mismatch
        lines.append(f"| {want} | {missing} | {mismatch} |")
        if result.returncode != 0:
            print(result.stderr[-2000:])
    lines.append(f"| total | {totals[0]} | {totals[1]} |")
    text = "\n".join(lines)
    print(text)
    if "GITHUB_STEP_SUMMARY" in os.environ:
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as f:
            f.write("## Profile match\n\n" + text + "\n")


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
    p.set_defaults(func=workloads)

    p = sub.add_parser("bench")
    p.add_argument("--variant", action="append", required=True, help="NAME=DIR (DIR/bin/pipeline_benchmark)")
    p.add_argument("--workload", action="append", required=True, help="NAME:CDB:FILTER:LIMIT")
    p.add_argument("--rounds", type=int, default=3)
    p.add_argument("--out", required=True)
    p.add_argument("--env", action="append", default=[], help="NAME=KEY=VALUE for one variant's runs")
    p.set_defaults(func=bench)

    p = sub.add_parser("match-report")
    p.add_argument("--build", required=True, help="LLVM build directory (compile_commands.json)")
    p.add_argument("--files", nargs="+", required=True)
    p.set_defaults(func=match_report)

    p = sub.add_parser("clang-bench")
    p.add_argument("--variant", action="append", default=[], help="NAME=PATH of a clang-23 binary")
    p.add_argument("--rounds", type=int, default=2)
    p.add_argument("--out", required=True)
    p.set_defaults(func=clang_bench)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
