#!/usr/bin/env python3

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


PRIVATE_CLANG_FILES = [
    "Sema/CoroutineStmtBuilder.h",
    "Sema/TypeLocBuilder.h",
    "Sema/TreeTransform.h",
]


def read_manifest(path: Path) -> list[dict]:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def normalize_build_type(name: str) -> str:
    lowered = name.lower()
    if "debug" in lowered:
        return "Debug"
    return "Release"


def detect_platform() -> str:
    plat = sys.platform
    if plat.startswith("win"):
        return "Windows"
    if plat == "darwin":
        return "macosx"
    if plat.startswith("linux"):
        return "Linux"
    raise RuntimeError(f"Unsupported platform: {plat}")


def pick_artifact(
    manifest: list[dict], version: str, build_type: str, is_lto: bool, platform: str
) -> dict:
    base_version = version.split("+", 1)[0]
    for entry in manifest:
        if entry.get("version") != version:
            continue
        if entry.get("platform") != platform.lower():
            continue
        if entry.get("build_type") != build_type:
            continue
        if bool(entry.get("is_lto")) != is_lto:
            continue
        return entry
    raise RuntimeError(
        f"No matching LLVM artifact in manifest for version={base_version}, platform={platform}, "
        f"build_type={build_type}, is_lto={is_lto}"
    )


def sha256sum(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(url: str, dest: Path, token: str | None) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    headers = {"User-Agent": "clice-setup-llvm"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = Request(url, headers=headers)
    try:
        with urlopen(request) as response, dest.open("wb") as handle:
            shutil.copyfileobj(response, handle)
    except HTTPError as err:
        raise RuntimeError(f"HTTP error {err.code} while downloading {url}") from err
    except URLError as err:
        raise RuntimeError(f"Failed to download {url}: {err.reason}") from err


def ensure_download(
    url: str, dest: Path, expected_sha256: str, token: str | None
) -> None:
    if dest.exists():
        current = sha256sum(dest)
        if current == expected_sha256:
            return
        dest.unlink()
    download(url, dest, token)
    current = sha256sum(dest)
    if current != expected_sha256:
        dest.unlink(missing_ok=True)
        raise RuntimeError(
            f"SHA256 mismatch for {dest.name}: expected {expected_sha256}, got {current}"
        )


def extract_archive(archive: Path, dest_dir: Path) -> None:
    dest_dir.mkdir(parents=True, exist_ok=True)
    name = archive.name.lower()
    if name.endswith(".tar.xz") or name.endswith(".tar.gz") or name.endswith(".tar"):
        with tarfile.open(archive, "r:*") as tar:
            tar.extractall(path=dest_dir)
        return
    if name.endswith(".7z"):
        seven_z = shutil.which("7z") or shutil.which("7zz")
        if not seven_z:
            raise RuntimeError("7z/7zz not found; required to extract .7z archives")
        subprocess.run([seven_z, "x", "-y", str(archive), f"-o{dest_dir}"], check=True)
        return
    raise RuntimeError(f"Unsupported archive format: {archive}")


def parse_version_tuple(text: str) -> tuple[int, ...]:
    digits = []
    current = ""
    for ch in text:
        if ch.isdigit():
            current += ch
        else:
            if current:
                digits.append(int(current))
                current = ""
            if ch in {".", "-"}:
                continue
    if current:
        digits.append(int(current))
    return tuple(digits)


def system_llvm_ok(required_version: str, build_type: str) -> Path | None:
    if build_type.lower().startswith("debug"):
        return None
    llvm_config = shutil.which("llvm-config")
    if not llvm_config:
        return None
    try:
        version = subprocess.check_output([llvm_config, "--version"], text=True).strip()
        prefix = subprocess.check_output([llvm_config, "--prefix"], text=True).strip()
    except (subprocess.CalledProcessError, OSError):
        return None
    required = parse_version_tuple(required_version.split("+", 1)[0])
    found = parse_version_tuple(version)
    if not found or found < required:
        return None
    return Path(prefix)


def github_api(url: str, token: str | None) -> dict:
    headers = {
        "Accept": "application/vnd.github+json",
        "User-Agent": "clice-setup-llvm",
    }
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = Request(url, headers=headers)
    with urlopen(request) as response:
        return json.load(response)


def lookup_llvm_commit(version: str, token: str | None) -> str | None:
    tag_version = version.split("+", 1)[0]
    tag = f"llvmorg-{tag_version}"
    ref_url = f"https://api.github.com/repos/llvm/llvm-project/git/ref/tags/{tag}"
    try:
        ref = github_api(ref_url, token)
    except Exception:
        return None
    obj = ref.get("object") or {}
    obj_type = obj.get("type")
    obj_sha = obj.get("sha")
    if obj_type == "commit":
        return obj_sha
    if obj_type == "tag" and obj_sha:
        tag_url = f"https://api.github.com/repos/llvm/llvm-project/git/tags/{obj_sha}"
        try:
            tag_info = github_api(tag_url, token)
        except Exception:
            return None
        return tag_info.get("object", {}).get("sha") or tag_info.get("sha")
    return None


def ensure_private_headers(
    install_path: Path, work_dir: Path, version: str, token: str | None, offline: bool
) -> None:
    missing = []
    for rel in PRIVATE_CLANG_FILES:
        if (install_path / "include" / "clang" / rel).exists():
            continue
        if (work_dir / "include" / "clang" / rel).exists():
            continue
        missing.append(rel)
    if not missing or offline:
        return
    commit = lookup_llvm_commit(version, token)
    if not commit:
        return
    for rel in missing:
        dest = work_dir / "include" / "clang" / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        url = f"https://raw.githubusercontent.com/llvm/llvm-project/{commit}/clang/lib/{rel}"
        download(url, dest, token)


def main() -> None:
    parser = argparse.ArgumentParser(description="Setup LLVM dependencies for CMake")
    parser.add_argument("--version", required=True)
    parser.add_argument("--build-type", required=True)
    parser.add_argument("--binary-dir", required=True)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--install-path")
    parser.add_argument("--enable-lto", action="store_true")
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    build_type = normalize_build_type(args.build_type)
    platform_name = detect_platform()
    manifest = read_manifest(Path(args.manifest))

    install_path: Path | None = None
    if args.install_path:
        candidate = Path(args.install_path)
        if candidate.exists():
            install_path = candidate
        else:
            raise RuntimeError(
                f"Provided LLVM_INSTALL_PATH does not exist: {candidate}"
            )

    if install_path is None:
        detected = system_llvm_ok(args.version, build_type)
        if detected:
            install_path = detected

    binary_dir = Path(args.binary_dir).resolve()
    install_root = binary_dir / ".llvm"

    artifact = None
    if install_path is None:
        artifact = pick_artifact(
            manifest, args.version, build_type, args.enable_lto, platform_name
        )
        filename = artifact["filename"]
        url_version = args.version.replace("+", "%2B")
        url = f"https://github.com/clice-io/clice-llvm/releases/download/{url_version}/{filename}"
        download_path = binary_dir / filename
        ensure_download(url, download_path, artifact["sha256"], token)
        extract_archive(download_path, install_root)
        install_path = install_root
    else:
        install_path = install_path.resolve()

    cmake_dir = install_path / "lib" / "cmake" / "llvm"
    ensure_private_headers(install_path, binary_dir, args.version, token, args.offline)

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8") as handle:
        json.dump(
            {
                "install_path": str(install_path),
                "cmake_dir": str(cmake_dir),
                "artifact": artifact or {},
            },
            handle,
            indent=2,
        )
        handle.write("\n")


if __name__ == "__main__":
    main()
