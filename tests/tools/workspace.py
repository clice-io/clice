"""On-disk workspace helpers: sources, document edits, cache inspection."""

import json
from pathlib import Path

from lsprotocol.types import (
    DidChangeTextDocumentParams,
    TextDocumentContentChangeWholeDocument,
    TextDocumentIdentifier,
    VersionedTextDocumentIdentifier,
)


def doc(uri: str) -> TextDocumentIdentifier:
    """Create a TextDocumentIdentifier from a URI string."""
    return TextDocumentIdentifier(uri=uri)


def write_source(workspace: Path, name: str, content: str) -> Path:
    """Write a source file to the workspace directory. Returns the file path."""
    path = workspace / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content)
    return path


def did_change(client, uri: str, version: int, text: str) -> None:
    """Send a didChange notification with whole-document replacement."""
    client.text_document_did_change(
        DidChangeTextDocumentParams(
            text_document=VersionedTextDocumentIdentifier(uri=uri, version=version),
            content_changes=[TextDocumentContentChangeWholeDocument(text=text)],
        )
    )


def get_field(obj, key, default=None):
    """Read a field from a dict or attribute-style LSP response object."""
    if isinstance(obj, dict):
        return obj.get(key, default)
    return getattr(obj, key, default)


# Versioned root of the unified cache store; bump together with
# cache_format_version in src/server/state/workspace.h.
CACHE_ROOT = Path(".clice") / "cache" / "v4"


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
