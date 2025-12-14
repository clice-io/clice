#!/usr/bin/env python3
"""
Prune unused LLVM binaries/libraries from the install tree.

Two modes:
  - discover: iteratively delete candidates and rebuild to confirm they are unused,
              then write the deletion list to a manifest JSON file.
  - apply:    read the manifest and delete the listed files without rebuilding
              (useful for LTO builds that trust the non-LTO pruning result).
"""

import argparse
import json
import shutil
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable, List, Optional


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Prune unused LLVM artifacts.")
    parser.add_argument(
        "--action",
        choices=["discover", "apply"],
        default="discover",
        help="discover: probe deletable files and record them; apply: delete using manifest",
    )
    parser.add_argument(
        "--install-dir",
        type=Path,
        default=Path(".llvm/build-install/lib"),
        help="Path to the LLVM install lib directory (default: .llvm/build-install/lib)",
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=Path("build"),
        help="CMake build directory used for validation builds (default: build)",
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        default=Path("pruned-libs.json"),
        help="Path to manifest JSON for recording or applying deletions",
    )
    parser.add_argument(
        "--gh-run-id",
        type=str,
        help="GitHub run ID to download manifest from when applying",
    )
    parser.add_argument(
        "--gh-artifact",
        type=str,
        default="llvm-pruned-libs",
        help="Artifact name/pattern that contains the manifest",
    )
    parser.add_argument(
        "--gh-download-dir",
        type=Path,
        default=Path("artifacts"),
        help="Directory to place downloaded artifacts",
    )
    parser.add_argument(
        "--max-attempts",
        type=int,
        default=30,
        help="Maximum attempts when waiting for manifest download",
    )
    parser.add_argument(
        "--sleep-seconds",
        type=int,
        default=60,
        help="Seconds to sleep between manifest download attempts",
    )
    return parser.parse_args()


def _print_failure_output(
    result: subprocess.CalledProcessError, max_lines: int = 50
) -> None:
    stdout = getattr(result, "stdout", "") or ""
    stderr = getattr(result, "stderr", "") or ""
    combined = stdout + stderr
    if not combined:
        return
    print("Build output (last lines):")
    lines = combined.splitlines()
    for line in lines[-max_lines:]:
        print(line)


def run_build(build_dir: Path) -> bool:
    cmd = ["cmake", "--build", str(build_dir)]
    print(f"Running: {' '.join(cmd)}")
    try:
        subprocess.run(cmd, check=True, capture_output=True, text=True)
        return True
    except subprocess.CalledProcessError as exc:
        _print_failure_output(exc)
        return False


def candidate_files(install_dir: Path) -> Iterable[Path]:
    if not install_dir.is_dir():
        raise FileNotFoundError(f"lib dir not found: {install_dir}")
    for path in sorted(install_dir.iterdir()):
        if not path.is_file():
            continue
        if path.suffix.lower() in {".a", ".lib"}:
            yield path
        else:
            print(f"Skipping non-static file: {path.name}")


def try_delete(path: Path, build_dir: Path) -> bool:
    backup = path.with_suffix(path.suffix + ".bak")
    print(f"Testing deletion: {path}")
    shutil.move(path, backup)
    success = run_build(build_dir)
    if success:
        backup.unlink(missing_ok=True)
        print(f"Safe to delete: {path.name}")
        return True
    shutil.move(backup, path)
    print(f"Required; restored: {path.name}")
    return False


def discover(install_dir: Path, build_dir: Path) -> List[str]:
    deletable: List[str] = []
    for path in candidate_files(install_dir):
        if try_delete(path, build_dir):
            deletable.append(path.name)
    return deletable


def write_manifest(
    manifest: Path, removed: List[str], install_dir: Path, build_dir: Path
) -> None:
    data = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "install_dir": str(install_dir),
        "build_dir": str(build_dir),
        "removed": removed,
    }
    manifest.write_text(json.dumps(data, indent=2))
    print(f"Wrote manifest with {len(removed)} entries to {manifest}")


def apply_manifest(manifest: Path, install_dir: Path) -> None:
    if not manifest.is_file():
        raise FileNotFoundError(f"Manifest not found: {manifest}")
    data = json.loads(manifest.read_text())
    removed = data.get("removed", [])
    if not isinstance(removed, list):
        raise ValueError("Manifest missing 'removed' list")
    for name in removed:
        target = install_dir / name
        if target.exists():
            print(f"Deleting {target}")
            target.unlink()
        else:
            print(f"Already absent: {target}")


def find_manifest(download_dir: Path, manifest_name: str) -> Optional[Path]:
    for path in download_dir.rglob(manifest_name):
        if path.is_file():
            return path
    return None


def wait_and_download_manifest(
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
            f"[{attempt}/{max_attempts}] Downloading manifest via gh run download "
            f"(run={run_id}, artifact={artifact})"
        )
        cmd = [
            "gh",
            "run",
            "download",
            str(run_id),
            "--pattern",
            artifact,
            "--dir",
            str(download_dir),
        ]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode == 0:
            found = find_manifest(download_dir, manifest_name)
            if found:
                print(f"Found manifest at {found}")
                return found
            print("Download succeeded but manifest not found; retrying...")
        else:
            print("gh run download failed; stderr follows:")
            print(result.stderr)
        if attempt < max_attempts:
            time.sleep(sleep_seconds)
    raise RuntimeError("Manifest could not be downloaded within the allotted attempts")


def ensure_manifest(
    manifest: Path,
    run_id: Optional[str],
    artifact: str,
    download_dir: Path,
    max_attempts: int,
    sleep_seconds: int,
) -> Path:
    if manifest.exists():
        return manifest
    if not run_id:
        raise FileNotFoundError(
            f"Manifest {manifest} missing and no gh run ID provided for download"
        )
    downloaded = wait_and_download_manifest(
        run_id=run_id,
        artifact=artifact,
        download_dir=download_dir,
        manifest_name=manifest.name,
        max_attempts=max_attempts,
        sleep_seconds=sleep_seconds,
    )
    if downloaded != manifest:
        shutil.copy(downloaded, manifest)
    return manifest


def main() -> None:
    args = parse_args()
    install_dir = args.install_dir
    build_dir = args.build_dir
    if args.action == "discover":
        deletable = discover(install_dir, build_dir)
        write_manifest(args.manifest, deletable, install_dir, build_dir)
    else:
        manifest = ensure_manifest(
            manifest=args.manifest,
            run_id=args.gh_run_id,
            artifact=args.gh_artifact,
            download_dir=args.gh_download_dir,
            max_attempts=args.max_attempts,
            sleep_seconds=args.sleep_seconds,
        )
        apply_manifest(manifest, install_dir)


if __name__ == "__main__":
    main()
