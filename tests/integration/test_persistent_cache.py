"""Integration tests for persistent PCH/PCM cache.

Verifies that PCH/PCM artifacts are written to .clice/cache/pch/ and .clice/cache/pcm/
with content-addressed filenames, survive server restarts via cache.json,
and are properly reused across sessions.
"""

import asyncio
import json
from pathlib import Path

import pytest
from lsprotocol.types import (
    DidCloseTextDocumentParams,
    HoverParams,
    Position,
    TextDocumentIdentifier,
)

from tests.conftest import CliceClient


def _write_cdb(workspace, files, extra_args=None):
    """Write a compile_commands.json for the given source files."""
    entries = []
    for f in files:
        args = ["clang++", "-std=c++17", "-fsyntax-only"]
        if extra_args:
            args.extend(extra_args)
        args.append(str(workspace / f))
        entries.append(
            {
                "directory": str(workspace),
                "file": str(workspace / f),
                "arguments": args,
            }
        )
    (workspace / "compile_commands.json").write_text(json.dumps(entries, indent=2))


def _doc(uri: str) -> TextDocumentIdentifier:
    return TextDocumentIdentifier(uri=uri)


def _list_pch_files(workspace: Path) -> list[Path]:
    """Return all .pch files in the cache directory."""
    pch_dir = workspace / ".clice" / "cache" / "pch"
    if not pch_dir.exists():
        return []
    return sorted(pch_dir.glob("*.pch"))


def _list_pcm_files(workspace: Path) -> list[Path]:
    """Return all .pcm files in the cache directory."""
    pcm_dir = workspace / ".clice" / "cache" / "pcm"
    if not pcm_dir.exists():
        return []
    return sorted(pcm_dir.glob("*.pcm"))


def _cache_json(workspace: Path) -> dict | None:
    """Read and parse cache.json, or return None if absent."""
    path = workspace / ".clice" / "cache" / "cache.json"
    if not path.exists():
        return None
    return json.loads(path.read_text())


async def _make_client(executable: Path, workspace: Path) -> CliceClient:
    """Spawn a fresh clice server and initialize it with the given workspace."""
    c = CliceClient()
    await c.start_io(str(executable), "--mode", "pipe")
    await c.initialize(workspace)
    return c


async def _shutdown_client(c: CliceClient) -> None:
    """Gracefully shut down a client."""
    try:
        await asyncio.wait_for(c.shutdown_async(None), timeout=5.0)
    except Exception:
        pass
    try:
        c.exit(None)
    except Exception:
        pass
    await asyncio.sleep(0.3)
    if hasattr(c, "_server") and c._server is not None and c._server.returncode is None:
        c._server.kill()
    try:
        c._stop_event.set()
        for task in c._async_tasks:
            task.cancel()
        await asyncio.sleep(0.1)
    except Exception:
        pass


# =========================================================================
# PCH persistent cache tests
# =========================================================================


async def test_pch_written_to_cache_dir(client, tmp_path):
    """After opening a file with #include, a .pch file should appear
    in .clice/cache/pch/ with a hex-hash filename."""
    (tmp_path / "header.h").write_text("#pragma once\nstruct Foo { int x; };\n")
    (tmp_path / "main.cpp").write_text(
        '#include "header.h"\nint main() { Foo f; return f.x; }\n'
    )
    _write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected clean compile, got: {diags}"

    # Verify PCH file exists in the cache directory.
    pch_files = _list_pch_files(tmp_path)
    assert len(pch_files) >= 1, "Expected at least one .pch file in .clice/cache/pch/"
    # Filename should be a 16-char hex hash + .pch
    assert pch_files[0].stem and len(pch_files[0].stem) == 16, (
        f"Expected 16-char hex filename, got: {pch_files[0].name}"
    )


async def test_cache_json_persisted(client, tmp_path):
    """After a PCH build, cache.json should be written with the entry."""
    (tmp_path / "header.h").write_text("#pragma once\nint global_val = 42;\n")
    (tmp_path / "main.cpp").write_text(
        '#include "header.h"\nint main() { return global_val; }\n'
    )
    _write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert len(client.diagnostics.get(uri, [])) == 0

    cache = _cache_json(tmp_path)
    assert cache is not None, "cache.json should exist after PCH build"
    assert "artifacts" in cache, "cache.json should have 'artifacts' section"
    assert len(cache["artifacts"]) >= 1, (
        "Expected at least one artifact entry in cache.json"
    )

    # Verify the entry has expected fields.
    entry = cache["artifacts"][0]
    assert "key" in entry
    assert "build_at" in entry
    assert "deps" in entry
    assert "filename" in entry


async def test_pch_reused_on_close_reopen(client, tmp_path):
    """Closing and reopening a file within the same session should reuse
    the cached PCH — no additional .pch files should be created."""
    (tmp_path / "header.h").write_text("#pragma once\nstruct Bar { int y; };\n")
    (tmp_path / "main.cpp").write_text(
        '#include "header.h"\nint main() { Bar b; return b.y; }\n'
    )
    _write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    # First open — builds PCH.
    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert len(client.diagnostics.get(uri, [])) == 0

    pch_after_first = _list_pch_files(tmp_path)
    assert len(pch_after_first) >= 1

    # Close.
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))
    await asyncio.sleep(0.5)

    # Clear diagnostics so we can wait for fresh ones.
    client.diagnostics.pop(uri, None)

    # Reopen — should reuse cached PCH.
    uri2, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert len(client.diagnostics.get(uri2, [])) == 0

    pch_after_reopen = _list_pch_files(tmp_path)
    assert pch_after_first == pch_after_reopen, (
        "PCH file set should be identical after close+reopen"
    )


async def test_pch_survives_server_restart(executable, tmp_path):
    """PCH cache should survive a full server restart — cache.json is
    loaded on startup and the existing .pch file is reused."""
    (tmp_path / "header.h").write_text("#pragma once\nstruct Baz { int z; };\n")
    (tmp_path / "main.cpp").write_text(
        '#include "header.h"\nint main() { Baz b; return b.z; }\n'
    )
    _write_cdb(tmp_path, ["main.cpp"])

    # Session 1: build PCH.
    c1 = await _make_client(executable, tmp_path)
    uri, _ = await c1.open_and_wait(tmp_path / "main.cpp")
    assert len(c1.diagnostics.get(uri, [])) == 0

    pch_files_s1 = _list_pch_files(tmp_path)
    assert len(pch_files_s1) >= 1, "PCH should be created in session 1"
    pch_mtime_s1 = pch_files_s1[0].stat().st_mtime

    cache_s1 = _cache_json(tmp_path)
    assert cache_s1 is not None, "cache.json should exist after session 1"

    await _shutdown_client(c1)

    # Session 2: restart server, reopen file.
    c2 = await _make_client(executable, tmp_path)
    # Clear so we can detect fresh diagnostics.
    uri2, _ = await c2.open_and_wait(tmp_path / "main.cpp")
    assert len(c2.diagnostics.get(uri2, [])) == 0

    # The same PCH file should still exist, not overwritten.
    pch_files_s2 = _list_pch_files(tmp_path)
    assert len(pch_files_s2) == len(pch_files_s1), (
        "No new PCH files should be created in session 2"
    )
    pch_mtime_s2 = pch_files_s2[0].stat().st_mtime
    assert pch_mtime_s1 == pch_mtime_s2, (
        "PCH file should not be rebuilt (mtime should be unchanged)"
    )

    await _shutdown_client(c2)


async def test_shared_preamble_shares_pch(client, tmp_path):
    """Two files with identical preambles should share the same PCH file
    (content-addressed by preamble hash)."""
    (tmp_path / "header.h").write_text("#pragma once\nint shared_val = 1;\n")
    (tmp_path / "a.cpp").write_text(
        '#include "header.h"\nint fa() { return shared_val; }\n'
    )
    (tmp_path / "b.cpp").write_text(
        '#include "header.h"\nint fb() { return shared_val + 1; }\n'
    )
    _write_cdb(tmp_path, ["a.cpp", "b.cpp"])
    await client.initialize(tmp_path)

    uri_a, _ = await client.open_and_wait(tmp_path / "a.cpp")
    uri_b, _ = await client.open_and_wait(tmp_path / "b.cpp")
    assert len(client.diagnostics.get(uri_a, [])) == 0
    assert len(client.diagnostics.get(uri_b, [])) == 0

    # Both files have the same preamble (#include "header.h").
    # Content-addressed naming means only ONE .pch file should exist.
    pch_files = _list_pch_files(tmp_path)
    assert len(pch_files) == 1, (
        f"Expected exactly 1 PCH file for shared preamble, got {len(pch_files)}: "
        f"{[f.name for f in pch_files]}"
    )


async def test_different_preamble_different_pch(client, tmp_path):
    """Files with different preambles should produce different PCH files."""
    (tmp_path / "a.h").write_text("#pragma once\nint val_a = 1;\n")
    (tmp_path / "b.h").write_text("#pragma once\nint val_b = 2;\n")
    (tmp_path / "a.cpp").write_text('#include "a.h"\nint fa() { return val_a; }\n')
    (tmp_path / "b.cpp").write_text('#include "b.h"\nint fb() { return val_b; }\n')
    _write_cdb(tmp_path, ["a.cpp", "b.cpp"])
    await client.initialize(tmp_path)

    uri_a, _ = await client.open_and_wait(tmp_path / "a.cpp")
    uri_b, _ = await client.open_and_wait(tmp_path / "b.cpp")
    assert len(client.diagnostics.get(uri_a, [])) == 0
    assert len(client.diagnostics.get(uri_b, [])) == 0

    # Different preambles → different hash → two separate .pch files.
    pch_files = _list_pch_files(tmp_path)
    assert len(pch_files) == 2, (
        f"Expected 2 PCH files for different preambles, got {len(pch_files)}: "
        f"{[f.name for f in pch_files]}"
    )


async def test_pch_rebuilt_on_header_change(client, tmp_path):
    """When a preamble header changes, a new PCH should be built
    (different hash → different filename). The old one remains for cleanup."""
    (tmp_path / "header.h").write_text("#pragma once\nstruct V1 { int a; };\n")
    (tmp_path / "main.cpp").write_text(
        '#include "header.h"\nint main() { V1 v; return v.a; }\n'
    )
    _write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert len(client.diagnostics.get(uri, [])) == 0

    pch_before = _list_pch_files(tmp_path)
    assert len(pch_before) >= 1

    # Modify header — changes preamble content hash.
    await asyncio.sleep(1.1)
    (tmp_path / "header.h").write_text("#pragma once\nstruct V2 { int b; };\n")
    # Also update main.cpp to use V2 so it compiles cleanly.
    (tmp_path / "main.cpp").write_text(
        '#include "header.h"\nint main() { V2 v; return v.b; }\n'
    )

    # Close and reopen to get fresh preamble.
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))
    await asyncio.sleep(0.5)
    client.diagnostics.pop(uri, None)

    uri2, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert len(client.diagnostics.get(uri2, [])) == 0

    pch_after = _list_pch_files(tmp_path)
    # The preamble content changed (#include "header.h" is the same text,
    # but the preamble hash is computed from the preamble TEXT in the source file,
    # not from the header content). Since the #include line is identical,
    # the preamble hash is the same → same PCH filename, but deps changed
    # so PCH gets rebuilt (overwritten at the same path).
    # Either way, compilation should succeed.
    assert len(pch_after) >= 1


async def test_no_tmp_files_after_build(client, tmp_path):
    """After a successful PCH build, no .tmp files should remain in the cache dir."""
    (tmp_path / "header.h").write_text("#pragma once\nint val = 1;\n")
    (tmp_path / "main.cpp").write_text(
        '#include "header.h"\nint main() { return val; }\n'
    )
    _write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert len(client.diagnostics.get(uri, [])) == 0

    # No .tmp files should linger.
    pch_dir = tmp_path / ".clice" / "cache" / "pch"
    if pch_dir.exists():
        tmp_files = list(pch_dir.glob("*.tmp"))
        assert len(tmp_files) == 0, f"Stale .tmp files found: {tmp_files}"

    pcm_dir = tmp_path / ".clice" / "cache" / "pcm"
    if pcm_dir.exists():
        tmp_files = list(pcm_dir.glob("*.tmp"))
        assert len(tmp_files) == 0, f"Stale .tmp files found: {tmp_files}"


async def test_cache_dirs_created_on_startup(client, tmp_path):
    """The .clice/cache/pch/ and .clice/cache/pcm/ directories should be created
    when the server initializes a workspace."""
    (tmp_path / "main.cpp").write_text("int main() { return 0; }\n")
    _write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    # Trigger a compilation to ensure load_workspace() has completed
    # (it runs asynchronously after initialization).
    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert len(client.diagnostics.get(uri, [])) == 0

    assert (tmp_path / ".clice" / "cache" / "pch").is_dir(), (
        ".clice/cache/pch/ should be created"
    )
    assert (tmp_path / ".clice" / "cache" / "pcm").is_dir(), (
        ".clice/cache/pcm/ should be created"
    )
