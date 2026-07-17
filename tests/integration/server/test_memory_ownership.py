"""Ownership-gauge tests for memory lifecycle regressions.

Each leak class is pinned by a deterministic counter from the
clice/internal/stats hook instead of a brittle RSS assertion: shards flip
back to disk after a save, saves write only the true dirty set, and
cancelled builds leave no tmp blobs behind.
"""

import asyncio

from lsprotocol.types import (
    DidChangeTextDocumentParams,
    DidCloseTextDocumentParams,
    DidSaveTextDocumentParams,
    TextDocumentContentChangeWholeDocument,
    TextDocumentIdentifier,
    VersionedTextDocumentIdentifier,
)

from tests.tools.checks import (
    MTIME_GRANULARITY,
    assert_no_anomaly,
    wait_for_index,
    wait_for_recompile,
)
from tests.tools.compile_commands import write_cdb
from tests.tools.workspace import (
    doc,
    get_field,
    list_tmp_files,
    pin_cache_to_workspace,
)


async def wait_stats(client, predicate, *, timeout=30.0, message=""):
    """Poll clice/internal/stats until predicate(stats) holds."""
    deadline = asyncio.get_event_loop().time() + timeout
    while True:
        stats = await client.stats()
        if predicate(stats):
            return stats
        if asyncio.get_event_loop().time() > deadline:
            raise AssertionError(f"{message or 'stats condition'} not met: {stats}")
        await asyncio.sleep(0.2)


async def test_shards_flip_back_after_save(client, tmp_path):
    pin_cache_to_workspace(tmp_path)
    files = []
    for i in range(4):
        name = f"file{i}.cpp"
        (tmp_path / name).write_text(f"int func_{i}() {{ return {i}; }}\n")
        files.append(name)
    write_cdb(tmp_path, files)
    await client.initialize(tmp_path)

    uri, _ = await client.open_and_wait(tmp_path / "file0.cpp")
    assert await wait_for_index(client, uri, "func_3"), (
        "background index did not finish"
    )

    stats = await wait_stats(
        client,
        lambda s: get_field(s, "indexInmemoryShards") == 0,
        message="shards did not flip back after save",
    )
    assert get_field(stats, "lastSaveShards") >= 1, (
        f"the settled round should have written shards: {stats}"
    )
    assert_no_anomaly(client, tmp_path)


async def test_save_writes_only_dirty_shards(client, tmp_path):
    pin_cache_to_workspace(tmp_path)
    files = []
    for i in range(4):
        name = f"file{i}.cpp"
        (tmp_path / name).write_text(f"int func_{i}() {{ return {i}; }}\n")
        files.append(name)
    write_cdb(tmp_path, files)
    await client.initialize(tmp_path)

    uri, _ = await client.open_and_wait(tmp_path / "file0.cpp")
    assert await wait_for_index(client, uri, "func_3"), (
        "background index did not finish"
    )
    await wait_stats(
        client,
        lambda s: get_field(s, "indexInmemoryShards") == 0,
        message="initial round did not settle",
    )
    # The first workspace tick only seeds the stat baseline.
    await client.poll("workspace")

    # Change one file on disk and tick the tracker: only its shard should
    # be re-merged and re-saved.
    await asyncio.sleep(MTIME_GRANULARITY)
    (tmp_path / "file2.cpp").write_text("int func_2_renamed() { return 2; }\n")
    await client.poll("workspace")
    assert await wait_for_index(client, uri, "func_2_renamed"), "reindex did not land"

    stats = await wait_stats(
        client,
        lambda s: get_field(s, "indexInmemoryShards") == 0,
        message="incremental round did not settle",
    )
    # Load-bearing assumptions for the exact count: the files are
    # standalone (no includes, so no header-shard fan-out) and only
    # background indexing merges shards (the open file's interactive
    # compile does not contribute one).
    assert get_field(stats, "lastSaveShards") == 1, (
        f"an incremental save must write only the touched shard: {stats}"
    )

    # A round with nothing to write commits zero shards: saving the open
    # file schedules a round, but its shard is served by the session and
    # background indexing skips open files.
    client.text_document_did_save(
        DidSaveTextDocumentParams(text_document=TextDocumentIdentifier(uri=uri))
    )
    await wait_stats(
        client,
        lambda s: get_field(s, "lastSaveShards") == 0,
        message="a no-op round must save zero shards",
    )
    assert_no_anomaly(client, tmp_path)


async def test_cancel_storm_leaves_no_tmp(client, tmp_path):
    pin_cache_to_workspace(tmp_path)
    (tmp_path / "header.h").write_text("#pragma once\nint base_val = 1;\n")
    (tmp_path / "main.cpp").write_text(
        '#include "header.h"\nint main() { return base_val; }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")

    # Each edit changes the preamble text, so each supersedes the previous
    # PCH build under a fresh content key.
    for i in range(15):
        client.text_document_did_change(
            DidChangeTextDocumentParams(
                text_document=VersionedTextDocumentIdentifier(uri=uri, version=i + 2),
                content_changes=[
                    TextDocumentContentChangeWholeDocument(
                        text=f'#define STORM {i}\n#include "header.h"\n'
                        "int main() { return base_val; }\n"
                    )
                ],
            )
        )
        await asyncio.sleep(0.05)

    await wait_for_recompile(client, uri)
    await wait_stats(
        client,
        lambda s: get_field(s, "pendingTmpFiles") == 0,
        message="cancelled builds leaked tmp blobs",
    )
    assert list_tmp_files(tmp_path) == [], (
        "tmp directory should be empty after settling"
    )
    # Shape smoke for the gauges no test asserts on: a missing or renamed
    # field reads back None, not a number.
    stats = await client.stats()
    assert get_field(stats, "pchCacheEntries") >= 1, f"a PCH was built: {stats}"
    assert get_field(stats, "sessions") == 1, f"one open document: {stats}"
    assert_no_anomaly(client, tmp_path)


async def test_preamble_state_released(client, tmp_path):
    pin_cache_to_workspace(tmp_path)
    names = []
    for i in range(3):
        (tmp_path / f"h{i}.h").write_text(f"#pragma once\nint distinct_{i} = {i};\n")
        (tmp_path / f"m{i}.cpp").write_text(
            f'#include "h{i}.h"\nint use_{i}() {{ return distinct_{i}; }}\n'
        )
        names.append(f"m{i}.cpp")
    write_cdb(tmp_path, names)
    await client.initialize(tmp_path)

    uris = []
    for i in range(3):
        uri, _ = await client.open_and_wait(tmp_path / f"m{i}.cpp")
        uris.append(uri)
    stats = await client.stats()
    assert get_field(stats, "pchLoadedStates") == 3, (
        f"three distinct preambles: {stats}"
    )

    for uri in uris:
        client.text_document_did_close(
            DidCloseTextDocumentParams(text_document=doc(uri))
        )
    # Budget follows the open count (open + 2, the slack keeping a
    # just-closed state warm): with everything closed at least one of the
    # three states must unload instead of staying mapped forever.
    await wait_stats(
        client,
        lambda s: get_field(s, "pchLoadedStates") <= 2,
        message="closing documents must release loaded preamble states",
    )

    # Reload after unload: reopening must reopen the blob from disk and
    # keep serving queries against the preamble's symbols.
    uri0, _ = await client.open_and_wait(tmp_path / "m0.cpp")
    hover = await client.hover_at(uri0, 1, 25)
    assert hover is not None, "query must survive an unload/reload cycle"
    stats = await client.stats()
    assert get_field(stats, "pchLoadedStates") >= 1, f"state reloaded: {stats}"
    assert_no_anomaly(client, tmp_path)


async def test_same_preamble_shared(client, tmp_path):
    pin_cache_to_workspace(tmp_path)
    (tmp_path / "shared.h").write_text("#pragma once\nint shared_val = 1;\n")
    names = []
    for i in range(4):
        (tmp_path / f"s{i}.cpp").write_text(
            f'#include "shared.h"\nint fn_{i}() {{ return shared_val; }}\n'
        )
        names.append(f"s{i}.cpp")
    write_cdb(tmp_path, names)
    await client.initialize(tmp_path)

    for i in range(4):
        await client.open_and_wait(tmp_path / f"s{i}.cpp")
    # Identical preambles share one content key, and sharing means one
    # blob: opening more consumers must not multiply loaded states.
    stats = await client.stats()
    assert get_field(stats, "pchLoadedStates") == 1, f"one shared key: {stats}"
    assert get_field(stats, "pchCacheEntries") == 1, f"one shared entry: {stats}"
    assert_no_anomaly(client, tmp_path)
