"""Cache inspection helpers for persistent cache tests."""

import json
from pathlib import Path

# Versioned root of the unified cache store; bump together with
# cache_format_version in src/server/state/workspace.h.
CACHE_ROOT = Path(".clice") / "cache" / "v3"


def cache_root(workspace: Path) -> Path:
    """Return the versioned cache store root for a workspace."""
    return workspace / CACHE_ROOT


def pin_cache_to_workspace(workspace: Path) -> None:
    """Write a clice.toml that pins cache_dir to <workspace>/.clice/."""
    (workspace / "clice.toml").write_text(
        '[project]\ncache_dir = "${workspace}/.clice"\n'
    )


def list_pch_files(workspace: Path) -> list[Path]:
    """Return all .pch files in the cache directory, sorted."""
    pch_dir = cache_root(workspace) / "pch"
    if not pch_dir.exists():
        return []
    return sorted(pch_dir.glob("*.pch"))


def list_pcm_files(workspace: Path) -> list[Path]:
    """Return all .pcm files in the cache directory, sorted."""
    pcm_dir = cache_root(workspace) / "pcm"
    if not pcm_dir.exists():
        return []
    return sorted(pcm_dir.glob("*.pcm"))


def read_cache_json(workspace: Path) -> dict | None:
    """Read and parse cache.json, or return None if absent."""
    path = cache_root(workspace) / "cache.json"
    if not path.exists():
        return None
    return json.loads(path.read_text())


def list_tmp_files(workspace: Path) -> list[Path]:
    """Return in-flight tmp files of all store instances.

    Committed blobs appear atomically, so anything under tmp/ is either an
    in-flight write of a live server or crash residue awaiting cleanup.
    """
    tmp_dir = cache_root(workspace) / "tmp"
    if not tmp_dir.exists():
        return []
    return sorted(p for p in tmp_dir.rglob("*") if p.is_file())
