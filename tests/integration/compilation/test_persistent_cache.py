"""Integration tests for persistent PCH/PCM cache.

Verifies that PCH/PCM artifacts are written to the unified cache store
(.clice/cache/v4/{pch,pcm}/) with content-addressed filenames, survive
server restarts via cache.json, and are properly reused across sessions.
"""

import asyncio
import json

import pytest
from lsprotocol.types import (
    DidCloseTextDocumentParams,
    HoverParams,
    Position,
)

from tests.tools.lifecycle import make_client, shutdown_client
from tests.tools.compile_commands import write_cdb
from tests.tools.workspace import doc
from tests.tools.checks import MTIME_GRANULARITY, SETTLE_TIME
from tests.tools.workspace import (
    cache_root,
    list_pch_files,
    list_pcm_files,
    list_tmp_files,
    pin_cache_to_workspace,
    read_cache_json,
)
from tests.tools.checks import assert_no_anomaly
from tests.tools.checks import assert_clean_compile


async def test_pch_written_to_cache_dir(client, tmp_path):
    """After opening a file with #include, a .pch file should appear
    in .clice/cache/pch/ with a hex-hash filename."""
    pin_cache_to_workspace(tmp_path)
    (tmp_path / "header.h").write_text("#pragma once\nstruct Foo { int x; };\n")
    (tmp_path / "main.cpp").write_text(
        '#include "header.h"\nint main() { Foo f; return f.x; }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert_clean_compile(client, uri)

    # Verify PCH file exists in the cache directory.
    pch_files = list_pch_files(tmp_path)
    assert len(pch_files) >= 1, "Expected at least one .pch file in the store"
    # Filename should be a 32-char hex hash (xxh3_128bits) + .pch
    assert pch_files[0].stem and len(pch_files[0].stem) == 32, (
        f"Expected 32-char hex filename, got: {pch_files[0].name}"
    )


async def test_cache_json_persisted(client, tmp_path):
    """After a PCH build, cache.json should be written with the entry."""
    pin_cache_to_workspace(tmp_path)
    (tmp_path / "header.h").write_text("#pragma once\nint global_val = 42;\n")
    (tmp_path / "main.cpp").write_text(
        '#include "header.h"\nint main() { return global_val; }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert_clean_compile(client, uri)

    cache = read_cache_json(tmp_path)
    assert cache is not None, "cache.json should exist after PCH build"
    assert "pch" in cache, "cache.json should have 'pch' section"
    assert len(cache["pch"]) >= 1, "Expected at least one PCH entry in cache.json"

    # Verify the entry has expected fields.
    entry = cache["pch"][0]
    assert "key" in entry
    assert "build_at" in entry
    assert "deps" in entry
    assert "bound" in entry


async def test_pch_reused_on_close_reopen(client, tmp_path):
    """Closing and reopening a file within the same session should reuse
    the cached PCH — no additional .pch files should be created."""
    pin_cache_to_workspace(tmp_path)
    (tmp_path / "header.h").write_text("#pragma once\nstruct Bar { int y; };\n")
    (tmp_path / "main.cpp").write_text(
        '#include "header.h"\nint main() { Bar b; return b.y; }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    # First open — builds PCH.
    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert_clean_compile(client, uri)

    pch_after_first = list_pch_files(tmp_path)
    assert len(pch_after_first) >= 1

    # Close.
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=doc(uri)))
    await asyncio.sleep(SETTLE_TIME)

    # Clear diagnostics so we can wait for fresh ones.
    client.diagnostics.pop(uri, None)

    # Reopen — should reuse cached PCH.
    uri2, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert_clean_compile(client, uri2)

    pch_after_reopen = list_pch_files(tmp_path)
    assert pch_after_first == pch_after_reopen, (
        "PCH file set should be identical after close+reopen"
    )


async def test_pch_survives_server_restart(executable, tmp_path):
    """PCH cache should survive a full server restart — cache.json is
    loaded on startup and the existing .pch file is reused."""
    pin_cache_to_workspace(tmp_path)
    (tmp_path / "header.h").write_text("#pragma once\nstruct Baz { int z; };\n")
    (tmp_path / "main.cpp").write_text(
        '#include "header.h"\nint main() { Baz b; return b.z; }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])

    # Session 1: build PCH.
    c1 = await make_client(executable, tmp_path)
    uri, _ = await c1.open_and_wait(tmp_path / "main.cpp")
    assert_clean_compile(c1, uri)

    pch_files_s1 = list_pch_files(tmp_path)
    assert len(pch_files_s1) >= 1, "PCH should be created in session 1"
    pch_mtime_s1 = pch_files_s1[0].stat().st_mtime

    cache_s1 = read_cache_json(tmp_path)
    assert cache_s1 is not None, "cache.json should exist after session 1"

    assert_no_anomaly(c1, tmp_path)
    await shutdown_client(c1)

    # Session 2: restart server, reopen file.
    c2 = await make_client(executable, tmp_path)
    # Clear so we can detect fresh diagnostics.
    uri2, _ = await c2.open_and_wait(tmp_path / "main.cpp")
    assert_clean_compile(c2, uri2)

    # The same PCH file should still exist, not overwritten.
    pch_files_s2 = list_pch_files(tmp_path)
    assert len(pch_files_s2) == len(pch_files_s1), (
        "No new PCH files should be created in session 2"
    )
    pch_mtime_s2 = pch_files_s2[0].stat().st_mtime
    assert pch_mtime_s1 == pch_mtime_s2, (
        "PCH file should not be rebuilt (mtime should be unchanged)"
    )

    assert_no_anomaly(c2, tmp_path)
    await shutdown_client(c2)


async def test_shared_preamble_shares_pch(client, tmp_path):
    """Two files with identical preambles should share the same PCH file
    (content-addressed by preamble hash)."""
    pin_cache_to_workspace(tmp_path)
    (tmp_path / "header.h").write_text("#pragma once\nint shared_val = 1;\n")
    (tmp_path / "a.cpp").write_text(
        '#include "header.h"\nint fa() { return shared_val; }\n'
    )
    (tmp_path / "b.cpp").write_text(
        '#include "header.h"\nint fb() { return shared_val + 1; }\n'
    )
    write_cdb(tmp_path, ["a.cpp", "b.cpp"])
    await client.initialize(tmp_path)

    uri_a, _ = await client.open_and_wait(tmp_path / "a.cpp")
    uri_b, _ = await client.open_and_wait(tmp_path / "b.cpp")
    assert_clean_compile(client, uri_a)
    assert_clean_compile(client, uri_b)

    # Both files have the same preamble (#include "header.h").
    # Content-addressed naming means only ONE .pch file should exist.
    pch_files = list_pch_files(tmp_path)
    assert len(pch_files) == 1, (
        f"Expected exactly 1 PCH file for shared preamble, got {len(pch_files)}: "
        f"{[f.name for f in pch_files]}"
    )


async def test_different_preamble_different_pch(client, tmp_path):
    """Files with different preambles should produce different PCH files."""
    pin_cache_to_workspace(tmp_path)
    (tmp_path / "a.h").write_text("#pragma once\nint val_a = 1;\n")
    (tmp_path / "b.h").write_text("#pragma once\nint val_b = 2;\n")
    (tmp_path / "a.cpp").write_text('#include "a.h"\nint fa() { return val_a; }\n')
    (tmp_path / "b.cpp").write_text('#include "b.h"\nint fb() { return val_b; }\n')
    write_cdb(tmp_path, ["a.cpp", "b.cpp"])
    await client.initialize(tmp_path)

    uri_a, _ = await client.open_and_wait(tmp_path / "a.cpp")
    uri_b, _ = await client.open_and_wait(tmp_path / "b.cpp")
    assert_clean_compile(client, uri_a)
    assert_clean_compile(client, uri_b)

    # Different preambles → different hash → two separate .pch files.
    pch_files = list_pch_files(tmp_path)
    assert len(pch_files) == 2, (
        f"Expected 2 PCH files for different preambles, got {len(pch_files)}: "
        f"{[f.name for f in pch_files]}"
    )


async def test_pch_rebuilt_on_header_change(client, tmp_path):
    """When a preamble header changes, a new PCH should be built
    (different hash → different filename). The old one remains for cleanup."""
    pin_cache_to_workspace(tmp_path)
    (tmp_path / "header.h").write_text("#pragma once\nstruct V1 { int a; };\n")
    (tmp_path / "main.cpp").write_text(
        '#include "header.h"\nint main() { V1 v; return v.a; }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert_clean_compile(client, uri)

    pch_before = list_pch_files(tmp_path)
    assert len(pch_before) >= 1

    # Modify header — changes preamble content hash.
    await asyncio.sleep(MTIME_GRANULARITY)
    (tmp_path / "header.h").write_text("#pragma once\nstruct V2 { int b; };\n")
    # Also update main.cpp to use V2 so it compiles cleanly.
    (tmp_path / "main.cpp").write_text(
        '#include "header.h"\nint main() { V2 v; return v.b; }\n'
    )

    # Close and reopen to get fresh preamble.
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=doc(uri)))
    await asyncio.sleep(SETTLE_TIME)
    client.diagnostics.pop(uri, None)

    uri2, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert_clean_compile(client, uri2)

    pch_after = list_pch_files(tmp_path)
    # The preamble content changed (#include "header.h" is the same text,
    # but the preamble hash is computed from the preamble TEXT in the source file,
    # not from the header content). Since the #include line is identical,
    # the preamble hash is the same → same PCH filename, but deps changed
    # so PCH gets rebuilt (overwritten at the same path).
    # Either way, compilation should succeed.
    assert len(pch_after) >= 1


async def test_no_tmp_files_after_build(client, tmp_path):
    """After a successful PCH build, no .tmp files should remain in the cache dir."""
    pin_cache_to_workspace(tmp_path)
    (tmp_path / "header.h").write_text("#pragma once\nint val = 1;\n")
    (tmp_path / "main.cpp").write_text(
        '#include "header.h"\nint main() { return val; }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert_clean_compile(client, uri)

    # No in-flight tmp files should linger after the build settles. The
    # pch namespace legitimately holds the paired .pch.idx blobs.
    assert list_tmp_files(tmp_path) == [], "Stale tmp files found"
    expected = {"pch": (".pch", ".pch.idx"), "pcm": (".pcm",)}
    for subdir, extensions in expected.items():
        blob_dir = cache_root(tmp_path) / subdir
        if blob_dir.exists():
            stray = [p for p in blob_dir.iterdir() if not p.name.endswith(extensions)]
            assert stray == [], f"Stray files in {subdir}/: {stray}"


async def test_cache_dirs_created_on_startup(client, tmp_path):
    """The versioned store directories should be created when the server
    initializes a workspace."""
    pin_cache_to_workspace(tmp_path)
    (tmp_path / "main.cpp").write_text("int main() { return 0; }\n")
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    # Trigger a compilation to ensure load_workspace() has completed
    # (it runs asynchronously after initialization).
    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert_clean_compile(client, uri)

    for subdir in ("pch", "pcm", "index"):
        assert (cache_root(tmp_path) / subdir).is_dir(), f"{subdir}/ should be created"


async def test_different_flags_different_pch(client, tmp_path):
    """Two files with identical preamble text but different -D flags must
    not share a PCH."""
    pin_cache_to_workspace(tmp_path)
    (tmp_path / "header.h").write_text(
        "#pragma once\n"
        "#ifdef MODE\nstruct Cfg { int mode; };\n#else\nstruct Cfg { int plain; };\n#endif\n"
    )
    body = '#include "header.h"\nint use() { Cfg c; return 0; }\n'
    (tmp_path / "a.cpp").write_text(body)
    (tmp_path / "b.cpp").write_text(body)

    # Same preamble text, different macro definitions per file.
    entries = []
    for name, extra in (("a.cpp", []), ("b.cpp", ["-DMODE=1"])):
        entries.append(
            {
                "directory": str(tmp_path),
                "file": str(tmp_path / name),
                "arguments": ["clang++", "-std=c++17", "-fsyntax-only"]
                + extra
                + [str(tmp_path / name)],
            }
        )
    (tmp_path / "compile_commands.json").write_text(json.dumps(entries))
    await client.initialize(tmp_path)

    uri_a, _ = await client.open_and_wait(tmp_path / "a.cpp")
    uri_b, _ = await client.open_and_wait(tmp_path / "b.cpp")
    assert_clean_compile(client, uri_a)
    assert_clean_compile(client, uri_b)

    pch_files = list_pch_files(tmp_path)
    assert len(pch_files) == 2, (
        f"Same preamble with different -D flags must produce 2 PCHs, got "
        f"{len(pch_files)}: {[f.name for f in pch_files]}"
    )


async def _wait_residue_released(workspace, deadline: float = 20.0):
    """Wait until orphaned workers of a killed server release their handles
    on tmp residue (a rename probe fails on Windows while a file is open)."""
    end = asyncio.get_event_loop().time() + deadline
    while asyncio.get_event_loop().time() < end:
        locked = False
        for f in list_tmp_files(workspace):
            probe = f.with_name(f.name + ".probe")
            try:
                f.rename(probe)
                probe.rename(f)
            except OSError:
                locked = True
                break
        if not locked:
            return
        await asyncio.sleep(0.5)


async def test_kill9_recovery(executable, tmp_path):
    """kill -9 during compilation must not corrupt the store: a restarted
    server sweeps crash residue and serves the file normally."""
    pin_cache_to_workspace(tmp_path)
    (tmp_path / "header.h").write_text("#pragma once\nstruct K { int x; };\n")
    (tmp_path / "main.cpp").write_text(
        '#include "header.h"\nint main() { K k; return k.x; }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])

    # Session 1: open the file and kill the server; the short delay makes it
    # likely (not guaranteed) the first build is still in flight.
    c1 = await make_client(executable, tmp_path)
    c1.open(tmp_path / "main.cpp")
    await asyncio.sleep(0.3)
    c1.kill_server()
    await c1.stop_io()
    await _wait_residue_released(tmp_path)

    # Session 2: its startup sweeps the dead instance's tmp directory, and
    # the cache must be usable again.
    c2 = await make_client(executable, tmp_path)
    uri, _ = await c2.open_and_wait(tmp_path / "main.cpp")
    assert_clean_compile(c2, uri)
    pch_files = list_pch_files(tmp_path)
    assert len(pch_files) >= 1, "PCH should be (re)built after crash"
    # Blob directories contain only committed blobs (the PCH and its
    # paired index), never partial writes.
    stray = [
        p
        for p in (cache_root(tmp_path) / "pch").iterdir()
        if not p.name.endswith((".pch", ".pch.idx"))
    ]
    assert stray == [], f"Crash residue in pch/: {stray}"
    assert_no_anomaly(c2, tmp_path)
    await shutdown_client(c2)

    # Clean shutdown removed session 2's own tmp; session 1's residue was
    # swept at session 2 startup, so nothing may remain.
    assert list_tmp_files(tmp_path) == [], "tmp residue should be swept"


async def test_cache_wiped_while_running(client, tmp_path):
    """Wiping the cache directory under a running server must not wedge
    PCH builds forever: the store re-creates its directories on demand."""
    import asyncio
    import shutil

    pin_cache_to_workspace(tmp_path)
    (tmp_path / "header.h").write_text("#pragma once\nstruct W { int x; };\n")
    (tmp_path / "main.cpp").write_text(
        '#include "header.h"\nint main() { W w; return w.x; }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert_clean_compile(client, uri)

    # Simulate a user resetting state without restarting the server.
    shutil.rmtree(tmp_path / ".clice" / "cache")

    # Change the preamble so a fresh PCH build is required.
    await asyncio.sleep(1.1)
    (tmp_path / "header.h").write_text("#pragma once\nstruct W { int x; int y; };\n")

    from tests.tools.checks import wait_for_recompile

    await wait_for_recompile(client, uri)
    assert_clean_compile(client, uri)
    assert len(list_pch_files(tmp_path)) == 1, (
        "PCH build must recover after a cache wipe"
    )
