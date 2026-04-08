"""Cache inspection helpers for persistent cache tests."""

import json
from pathlib import Path


def list_pch_files(workspace: Path) -> list[Path]:
    """Return all .pch files in the cache directory, sorted."""
    pch_dir = workspace / ".clice" / "cache" / "pch"
    if not pch_dir.exists():
        return []
    return sorted(pch_dir.glob("*.pch"))


def list_pcm_files(workspace: Path) -> list[Path]:
    """Return all .pcm files in the cache directory, sorted."""
    pcm_dir = workspace / ".clice" / "cache" / "pcm"
    if not pcm_dir.exists():
        return []
    return sorted(pcm_dir.glob("*.pcm"))


def read_cache_json(workspace: Path) -> dict | None:
    """Read and parse cache.json, or return None if absent."""
    path = workspace / ".clice" / "cache" / "cache.json"
    if not path.exists():
        return None
    return json.loads(path.read_text())


def list_tmp_files(workspace: Path) -> list[Path]:
    """Return stale .tmp files in pch and pcm cache directories."""
    tmp_files = []
    for subdir in ("pch", "pcm"):
        d = workspace / ".clice" / "cache" / subdir
        if d.exists():
            tmp_files.extend(d.glob("*.tmp"))
    return tmp_files
