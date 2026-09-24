"""Noise study harness: run several clice binaries and a reference clang
interleaved on the same machine over a frozen corpus, record every sample.

usage: perf_exp.py --corpus DIR --ref CLANG --clice LABEL=PATH ... --rounds N --out JSON
"""

import argparse
import json
import os
import platform
import random
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

TARGET = "--target=x86_64-unknown-linux-gnu"


def host_info():
    info = {"system": platform.system(), "machine": platform.machine(), "cpus": os.cpu_count()}
    try:
        if info["system"] == "Linux":
            for line in open("/proc/cpuinfo"):
                if line.startswith("model name"):
                    info["cpu"] = line.split(":", 1)[1].strip()
                    break
        elif info["system"] == "Darwin":
            info["cpu"] = subprocess.check_output(
                ["sysctl", "-n", "machdep.cpu.brand_string"], text=True
            ).strip()
        else:
            info["cpu"] = subprocess.check_output(
                ["powershell", "-NoProfile", "-Command", "(Get-CimInstance Win32_Processor).Name"],
                text=True,
            ).strip()
    except Exception as e:  # noqa: BLE001
        info["cpu_error"] = str(e)
    return info


def perf_available():
    if platform.system() != "Linux" or not shutil.which("perf"):
        return False
    r = subprocess.run(
        ["perf", "stat", "-x,", "-e", "instructions:u", "--", "true"],
        capture_output=True,
        text=True,
    )
    return r.returncode == 0 and "not supported" not in r.stderr and "<not" not in r.stderr


def run_measured(cmd, cwd, use_perf):
    """Wall seconds and (optionally) user-space instruction count."""
    instr = None
    if use_perf:
        out = Path(cwd) / "perf.out"
        cmd = ["perf", "stat", "-x,", "-e", "instructions:u", "-o", str(out), "--"] + cmd
    t0 = time.perf_counter()
    r = subprocess.run(cmd, cwd=cwd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    wall = time.perf_counter() - t0
    if r.returncode != 0:
        raise RuntimeError(f"{cmd} failed: {r.stderr[-2000:]}")
    if use_perf:
        for line in out.read_text().splitlines():
            parts = line.split(",")
            if len(parts) > 2 and parts[2].startswith("instructions") and parts[0].isdigit():
                instr = int(parts[0])
    return wall, instr


def make_workspace(root, corpus, manifest, label):
    ws = root / f"ws_{label}"
    if ws.exists():
        shutil.rmtree(ws)
    ws.mkdir(parents=True)
    db = []
    for name, std in manifest.items():
        shutil.copy(corpus / name, ws / name)
        cc = "clang" if name.endswith(".c") else "clang++"
        db.append(
            {
                "directory": ws.as_posix(),
                "file": (ws / name).as_posix(),
                "arguments": [cc, TARGET, std, "-w", "-c", name],
            }
        )
    (ws / "compile_commands.json").write_text(json.dumps(db, indent=1))
    return ws


BUILD_RE = re.compile(r"\[perf:build\] kind=turun file=(\S+) .*?compile_ms=(\d+) index_ms=(\d+)")


def run_clice(binary, ws, use_perf):
    shutil.rmtree(ws / ".clice", ignore_errors=True)
    wall, instr = run_measured(
        [binary, "index", "--workers", "1", "--workspace", ws.as_posix()], ws, use_perf
    )
    per_file = {}
    for log in (ws / ".clice" / "logs").rglob("*.log"):
        for m in BUILD_RE.finditer(log.read_text(errors="replace")):
            per_file[Path(m.group(1)).name] = {
                "compile_ms": int(m.group(2)),
                "index_ms": int(m.group(3)),
            }
    return {"wall": wall, "instr": instr, "files": per_file}


def run_ref(ref, corpus, manifest, use_perf):
    per_file = {}
    for name, std in manifest.items():
        wall, instr = run_measured([ref, TARGET, std, "-w", "-fsyntax-only", name], corpus, use_perf)
        per_file[name] = {"wall": wall, "instr": instr}
    return {"files": per_file}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--ref", required=True)
    ap.add_argument("--clice", action="append", required=True)
    ap.add_argument("--rounds", type=int, default=7)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    corpus = Path(args.corpus).resolve()
    manifest = json.loads((corpus / "manifest.json").read_text())
    root = Path("perf-work").resolve()
    root.mkdir(exist_ok=True)
    clices = dict(item.split("=", 1) for item in args.clice)
    workspaces = {label: make_workspace(root, corpus, manifest, label) for label in clices}
    use_perf = perf_available()

    result = {"host": host_info(), "perf": use_perf, "rounds": []}
    print(json.dumps(result["host"]), "perf:", use_perf, flush=True)

    # Warm-up: page in every binary and the corpus once, unrecorded.
    run_ref(args.ref, corpus, manifest, False)
    for label, binary in clices.items():
        run_clice(binary, workspaces[label], False)

    for i in range(args.rounds):
        order = ["ref"] + list(clices)
        random.shuffle(order)
        samples = {}
        for label in order:
            if label == "ref":
                samples[label] = run_ref(args.ref, corpus, manifest, use_perf)
            else:
                samples[label] = run_clice(clices[label], workspaces[label], use_perf)
        result["rounds"].append({"order": order, "samples": samples})
        brief = {
            k: round(v["wall"], 2) if "wall" in v else round(sum(f["wall"] for f in v["files"].values()), 2)
            for k, v in samples.items()
        }
        print(f"round {i}: {brief}", flush=True)

    Path(args.out).write_text(json.dumps(result, indent=1))


if __name__ == "__main__":
    sys.exit(main())
