#!/usr/bin/env python3

import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path


def sha256sum(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_platform(name: str) -> str:
    lowered = name.lower()
    if "windows" in lowered:
        return "windows"
    if "linux" in lowered:
        return "linux"
    if "macos" in lowered:
        return "macosx"
    raise ValueError(f"Unable to determine platform from filename: {name}")


def parse_build_type(name: str) -> str:
    lowered = name.lower()
    if "debug" in lowered:
        return "Debug"
    return "RelWithDebInfo"


def build_metadata_entry(path: Path, version: str) -> dict:
    filename = path.name
    return {
        "version": version,
        "filename": filename,
        "sha256": sha256sum(path),
        "lto": "-lto" in filename.lower(),
        "asan": "-asan" in filename.lower(),
        "platform": parse_platform(filename),
        "build_type": parse_build_type(filename),
    }


def main() -> None:
    if len(sys.argv) != 4:
        print(
            "Usage: upload-llvm.py <tag> <target_repo> <workflow_id>", file=sys.stderr
        )
        sys.exit(1)

    tag, target_repo, workflow_id = sys.argv[1:]
    artifacts_dir = Path("artifacts")

    if not artifacts_dir.is_dir():
        print(f"Artifacts directory not found: {artifacts_dir}", file=sys.stderr)
        sys.exit(1)

    artifact_files = sorted(
        p
        for p in artifacts_dir.rglob("*")
        if p.is_file() and p.suffix.lower() != ".json"
    )

    if not artifact_files:
        print("No artifacts found to upload.", file=sys.stderr)
        sys.exit(1)

    version_without_prefix = tag.lstrip("vV")
    metadata = [
        build_metadata_entry(path, version_without_prefix) for path in artifact_files
    ]

    json_path = artifacts_dir / "llvm-manifest.json"
    with json_path.open("w", encoding="utf-8") as handle:
        json.dump(metadata, handle, indent=2)
        handle.write("\n")

    assets = [str(path) for path in artifact_files]
    assets.append(str(json_path))

    env = os.environ.copy()

    print(f"Checking for existing release {tag} in {target_repo}...")
    view_result = subprocess.run(
        ["gh", "release", "view", tag, "--repo", target_repo],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    if view_result.returncode == 0:
        print(f"Deleting existing release {tag} in {target_repo}...")
        subprocess.run(
            ["gh", "release", "delete", tag, "--repo", target_repo, "-y"],
            env=env,
            check=True,
        )

    print(f"Creating release {tag} in {target_repo} with {len(assets)} assets...")
    # fmt: off
    args = [
        "gh", "release", "create", tag, *assets,
        "--repo", target_repo,
        "--title", tag,
        "--notes", f"Artifacts build from workflow run https://github.com/clice-io/clice/actions/runs/{workflow_id}",
        "--latest",
    ]
    # fmt: on
    subprocess.run(args, env=env, check=True)


if __name__ == "__main__":
    main()
