#!/usr/bin/env python3
"""
LLVM release pipeline utilities.

Actions:
  discover:  Iteratively probe which static libs can be removed by deleting
             and rebuilding, then write the result to a manifest JSON.
  apply:     Read a manifest and replace listed libs with empty archives.
  repackage: Download all LLVM build artifacts, apply pruning, and repackage
             them in parallel.
"""

import argparse
import concurrent.futures
import fnmatch
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable, List, Optional

ARTIFACTS = [
    "arm64-linux-gnu-releasedbg-lto.tar.xz",
    "arm64-linux-gnu-releasedbg.tar.xz",
    "arm64-windows-msvc-releasedbg-lto.tar.xz",
    "arm64-windows-msvc-releasedbg.tar.xz",
    "arm64-macos-clang-debug-asan.tar.xz",
    "arm64-macos-clang-releasedbg-lto.tar.xz",
    "arm64-macos-clang-releasedbg.tar.xz",
    "x64-linux-gnu-debug-asan.tar.xz",
    "x64-linux-gnu-releasedbg-lto.tar.xz",
    "x64-linux-gnu-releasedbg.tar.xz",
    "x64-macos-clang-releasedbg-lto.tar.xz",
    "x64-macos-clang-releasedbg.tar.xz",
    "x64-windows-msvc-releasedbg-lto.tar.xz",
    "x64-windows-msvc-releasedbg.tar.xz",
]

ARCHIVE_MAGIC = b"!<arch>\n"


def _is_shared_lib(path: Path) -> bool:
    return ".so" in path.suffixes or ".dylib" in path.suffixes


def _replace_with_empty_archive(path: Path) -> None:
    if _is_shared_lib(path):
        path.write_bytes(b"")
    else:
        path.write_bytes(ARCHIVE_MAGIC)


def _remove_binaries(build_dir: Path) -> None:
    bin_dir = build_dir / "bin"
    if not bin_dir.is_dir():
        return
    for f in bin_dir.iterdir():
        if f.is_file() and f.suffix in {"", ".exe"}:
            f.unlink()


def _run_build(build_dir: Path) -> bool:
    try:
        subprocess.run(
            ["cmake", "--build", str(build_dir)],
            check=True,
            capture_output=True,
            text=True,
        )
        return True
    except subprocess.CalledProcessError as exc:
        combined = (exc.stdout or "") + (exc.stderr or "")
        if combined:
            print("Build output (last lines):")
            for line in combined.splitlines()[-50:]:
                print(line)
        return False


def _manifest_for(artifact: str, manifests_dir: Path) -> Optional[Path]:
    manifest_dirs = {
        "linux": "prune-manifest-ubuntu-24.04",
        "macos": "prune-manifest-macos-15",
        "windows": "prune-manifest-windows-2025",
    }
    for platform, dirname in manifest_dirs.items():
        if platform in artifact:
            return manifests_dir / dirname / "pruned-libs.json"
    return None


def _candidate_files(
    install_dir: Path, skip_patterns: Optional[List[str]] = None
) -> Iterable[Path]:
    if not install_dir.is_dir():
        raise FileNotFoundError(f"lib dir not found: {install_dir}")
    for path in sorted(install_dir.iterdir()):
        if not path.is_file():
            continue
        if path.suffix.lower() not in {".a", ".lib"}:
            print(f"Skipping non-static file: {path.name}")
            continue
        if skip_patterns and any(fnmatch.fnmatch(path.name, p) for p in skip_patterns):
            print(f"Skipping (never-prune): {path.name}")
            continue
        yield path


def _nullify_shared_libs(install_dir: Path) -> List[dict]:
    nullified: List[dict] = []
    for path in sorted(install_dir.iterdir()):
        if not path.is_file() or not _is_shared_lib(path):
            continue
        size = path.stat().st_size
        if size > 0:
            print(f"Nullifying shared lib: {path.name} ({size} bytes)")
            path.write_bytes(b"")
            nullified.append({"name": path.name, "size": size})
    return nullified


def _try_delete(path: Path, build_dir: Path) -> Optional[int]:
    size = path.stat().st_size
    backup = path.with_suffix(path.suffix + ".bak")
    print(f"Testing deletion: {path}")
    shutil.move(path, backup)
    _remove_binaries(build_dir)
    if _run_build(build_dir):
        backup.unlink(missing_ok=True)
        print(f"Safe to delete: {path.name} ({size} bytes)")
        return size
    shutil.move(backup, path)
    print(f"Required; restored: {path.name}")
    return None


def discover(
    install_dir: Path,
    build_dir: Path,
    skip_patterns: Optional[List[str]] = None,
) -> List[dict]:
    nullified = _nullify_shared_libs(install_dir)
    deletable: List[dict] = []
    for path in _candidate_files(install_dir, skip_patterns):
        size = _try_delete(path, build_dir)
        if size is not None:
            deletable.append({"name": path.name, "size": size})
    return nullified + deletable


def apply_manifest(manifest: Path, install_dir: Path) -> None:
    if not manifest.is_file():
        raise FileNotFoundError(f"Manifest not found: {manifest}")
    data = json.loads(manifest.read_text())
    removed = data.get("removed", [])
    if not isinstance(removed, list):
        raise ValueError("Manifest missing 'removed' list")
    for entry in removed:
        name = entry["name"] if isinstance(entry, dict) else entry
        target = install_dir / name
        if target.exists():
            _replace_with_empty_archive(target)
        else:
            print(f"Already absent: {target}")


def build_metadata_entry(path: Path) -> dict:
    name = path.name.lower()
    if "windows" in name:
        platform = "windows"
    elif "linux" in name:
        platform = "linux"
    elif "macos" in name:
        platform = "macos"
    else:
        platform = "unknown"

    if name.startswith("arm64-"):
        arch = "arm64"
    elif name.startswith("x64-") or name.startswith("x86_64-"):
        arch = "x64"
    else:
        arch = "unknown"

    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)

    return {
        "filename": path.name,
        "sha256": digest.hexdigest(),
        "lto": "-lto" in name,
        "asan": "-asan" in name,
        "platform": platform,
        "arch": arch,
        "build_type": "Debug" if "debug" in name else "RelWithDebInfo",
    }


def _compress_tar_xz(
    source_dir: Path,
    output_path: Path,
    xz_level: str,
    label: str,
) -> int:
    print(f"[{label}] Compressing (xz {xz_level})...", flush=True)
    with output_path.open("wb") as out:
        tar = subprocess.Popen(
            ["tar", "-C", str(source_dir), "-cf", "-", "."],
            stdout=subprocess.PIPE,
        )
        xz = subprocess.Popen(
            ["xz", "-T0", xz_level, "-c"],
            stdin=tar.stdout,
            stdout=out,
        )
        tar.stdout.close()
        xz.communicate()
        tar.wait()
        if tar.returncode != 0 or xz.returncode != 0:
            raise RuntimeError(
                f"tar/xz failed (tar={tar.returncode}, xz={xz.returncode})"
            )
    return output_path.stat().st_size


def _process_artifact(
    artifact: str,
    source_run_id: str,
    manifests_dir: Path,
    output_dir: Path,
    version: Optional[str] = None,
) -> None:
    workdir = Path(tempfile.mkdtemp(prefix="repackage-"))
    try:
        dl_dir = workdir / "dl"
        content_dir = workdir / "content"

        print(f"[{artifact}] Downloading...", flush=True)
        subprocess.run(
            ["gh", "run", "download", source_run_id, "-n", artifact, "-D", str(dl_dir)],
            check=True,
        )

        print(f"[{artifact}] Extracting...", flush=True)
        content_dir.mkdir()
        subprocess.run(
            [
                "tar",
                "-xf",
                str(dl_dir / artifact),
                "-C",
                str(content_dir),
            ],
            check=True,
        )
        shutil.rmtree(dl_dir)

        if "debug" in artifact or "asan" in artifact:
            print(f"[{artifact}] Debug/ASAN — skipping prune", flush=True)
        else:
            manifest = _manifest_for(artifact, manifests_dir)
            if not manifest:
                raise RuntimeError(f"No manifest mapping for artifact: {artifact}")
            if not manifest.is_file():
                raise FileNotFoundError(f"Prune manifest missing: {manifest}")
            print(f"[{artifact}] Pruning...", flush=True)
            apply_manifest(manifest, content_dir / "lib")

        output_path = output_dir / artifact
        file_size = _compress_tar_xz(content_dir, output_path, "-9e", artifact)

        print(f"[{artifact}] Done ({file_size / 1048576:.1f} MB)", flush=True)

        if version:
            meta = build_metadata_entry(output_path)
            meta_path = output_dir / f"{artifact}.meta.json"
            meta_path.write_text(json.dumps(meta, indent=2))
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


def repackage(
    source_run_id: str,
    manifests_dir: Path,
    output_dir: Path,
    max_parallel: int = 3,
    artifacts: Optional[List[str]] = None,
    version: Optional[str] = None,
) -> None:
    targets = artifacts if artifacts else ARTIFACTS
    output_dir.mkdir(parents=True, exist_ok=True)

    failed: List[str] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=max_parallel) as pool:
        futures = {
            pool.submit(
                _process_artifact, a, source_run_id, manifests_dir, output_dir, version
            ): a
            for a in targets
        }
        for future in concurrent.futures.as_completed(futures):
            artifact = futures[future]
            try:
                future.result()
            except Exception as exc:
                print(f"[{artifact}] FAILED: {exc}", flush=True)
                failed.append(artifact)

    if failed:
        print(f"\nFailed artifacts ({len(failed)}):")
        for name in failed:
            print(f"  {name}")
        sys.exit(1)

    print(f"\nAll {len(targets)} artifacts repackaged:")
    for path in sorted(output_dir.iterdir()):
        if path.is_file() and path.suffix != ".json":
            print(f"  {path.stat().st_size / 1048576:>8.1f} MB  {path.name}")


def _wait_and_download_manifest(
    run_id: str,
    artifact: str,
    download_dir: Path,
    manifest_name: str,
    max_attempts: int,
    sleep_seconds: int,
) -> Path:
    download_dir.mkdir(parents=True, exist_ok=True)
    for attempt in range(1, max_attempts + 1):
        print(
            f"[{attempt}/{max_attempts}] Downloading manifest "
            f"(run={run_id}, artifact={artifact})"
        )
        result = subprocess.run(
            [
                "gh",
                "run",
                "download",
                str(run_id),
                "--pattern",
                artifact,
                "--dir",
                str(download_dir),
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode == 0:
            for path in download_dir.rglob(manifest_name):
                if path.is_file():
                    print(f"Found manifest at {path}")
                    return path
            print("Download succeeded but manifest not found; retrying...")
        else:
            print(f"gh run download failed: {result.stderr.strip()}")
        if attempt < max_attempts:
            time.sleep(sleep_seconds)
    raise RuntimeError("Manifest could not be downloaded within the allotted attempts")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="LLVM release pipeline utilities.")
    sub = parser.add_subparsers(dest="action", required=True)

    dp = sub.add_parser("discover", help="Probe which static libs can be removed")
    dp.add_argument("--install-dir", type=Path, required=True)
    dp.add_argument("--build-dir", type=Path, required=True)
    dp.add_argument("--manifest", type=Path, required=True)
    dp.add_argument("--skip-pattern", action="append", default=[])

    ap = sub.add_parser("apply", help="Replace listed libs with empty archives")
    ap.add_argument("--manifest", type=Path, required=True)
    ap.add_argument("--install-dir", type=Path, required=True)
    ap.add_argument("--gh-run-id", type=str)
    ap.add_argument("--gh-artifact", type=str, default="llvm-pruned-libs")
    ap.add_argument("--gh-download-dir", type=Path, default=Path("artifacts"))
    ap.add_argument("--max-attempts", type=int, default=30)
    ap.add_argument("--sleep-seconds", type=int, default=60)

    rp = sub.add_parser("repackage", help="Download, prune, and repackage artifacts")
    rp.add_argument("--source-run-id", type=str, required=True)
    rp.add_argument("--manifests-dir", type=Path, required=True)
    rp.add_argument("--output-dir", type=Path, required=True)
    rp.add_argument("--max-parallel", type=int, default=3)
    rp.add_argument("--artifacts", nargs="*")
    rp.add_argument("--version", type=str)

    return parser.parse_args()


def main() -> None:
    args = parse_args()

    if args.action == "discover":
        removed = discover(args.install_dir, args.build_dir, args.skip_pattern)
        total_size = sum(e["size"] for e in removed)
        data = {
            "generated_at": datetime.now(timezone.utc).isoformat(),
            "install_dir": str(args.install_dir),
            "build_dir": str(args.build_dir),
            "total_saved_bytes": total_size,
            "removed": removed,
        }
        args.manifest.write_text(json.dumps(data, indent=2))
        print(f"Wrote manifest with {len(removed)} entries to {args.manifest}")
        print(f"Total space saved: {total_size / 1048576:.1f} MB")
    elif args.action == "apply":
        manifest = args.manifest
        if not manifest.exists():
            if not args.gh_run_id:
                raise FileNotFoundError(
                    f"Manifest {manifest} missing and no gh run ID provided"
                )
            downloaded = _wait_and_download_manifest(
                args.gh_run_id,
                args.gh_artifact,
                args.gh_download_dir,
                manifest.name,
                args.max_attempts,
                args.sleep_seconds,
            )
            if downloaded != manifest:
                shutil.copy(downloaded, manifest)
        apply_manifest(manifest, args.install_dir)
    elif args.action == "repackage":
        repackage(
            source_run_id=args.source_run_id,
            manifests_dir=args.manifests_dir,
            output_dir=args.output_dir,
            max_parallel=args.max_parallel,
            artifacts=args.artifacts,
            version=args.version,
        )


if __name__ == "__main__":
    main()
