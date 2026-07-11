"""Integration tests for the PCH overlay: header symbols of open in-memory
files resolve through the PCH's paired index blob, independent of the disk
index and faithful to the live buffer's preprocessor context."""

import asyncio

from tests.tools.lifecycle import make_client, shutdown_client
from tests.tools.compile_commands import write_cdb
from tests.tools.checks import (
    MTIME_GRANULARITY,
    locations_of,
    wait_for_index,
    wait_for_recompile,
)
from tests.tools.workspace import (
    cache_root,
    did_change,
    list_pch_files,
    pin_cache_to_workspace,
)

NO_INDEXING = {"project": {"enable_indexing": False}}


async def test_definition_into_unindexed_header(client, tmp_path):
    (tmp_path / "foo.h").write_text("inline void foo() {}\n")
    (tmp_path / "main.cpp").write_text(
        '#include "foo.h"\nint main() { foo(); return 0; }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path, initialization_options=NO_INDEXING)

    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    # With background indexing off, only the PCH overlay knows the header.
    locs = locations_of(await client.definition_at(uri, 1, 13))
    assert any(
        loc.uri.endswith("foo.h") and loc.range.start.line == 0 for loc in locs
    ), (
        f"expected definition in foo.h line 0, got {[(loc.uri, loc.range.start.line) for loc in locs]}"
    )


async def test_references_include_header_rows(client, tmp_path):
    (tmp_path / "foo.h").write_text(
        "inline void foo() {}\ninline void bar() { foo(); }\n"
    )
    (tmp_path / "main.cpp").write_text(
        '#include "foo.h"\nint main() { foo(); return 0; }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path, initialization_options=NO_INDEXING)

    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    refs = locations_of(await client.references_at(uri, 1, 13))
    assert any(r.uri.endswith("foo.h") and r.range.start.line == 1 for r in refs), (
        f"expected header-internal reference, got {[(r.uri, r.range.start.line) for r in refs]}"
    )
    assert any(r.uri.endswith("main.cpp") for r in refs)


async def test_buffer_context_overrides_disk(client, tmp_path):
    (tmp_path / "crypto.h").write_text(
        "#ifdef USE_A\ninline void only_a() {}\n#else\ninline void only_b() {}\n#endif\n"
    )
    disk_text = '#include "crypto.h"\nint main() { only_b(); return 0; }\n'
    (tmp_path / "main.cpp").write_text(disk_text)
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert await wait_for_index(client, uri, "only_b"), "Index not ready after 30s"

    # The buffer's preamble now activates the branch no disk context has
    # ever seen; only the rebuilt PCH's overlay can resolve only_a.
    did_change(
        client,
        uri,
        2,
        '#define USE_A 1\n#include "crypto.h"\nint main() { only_a(); return 0; }\n',
    )
    await wait_for_recompile(client, uri)

    locs = locations_of(await client.definition_at(uri, 2, 13))
    assert any(
        loc.uri.endswith("crypto.h") and loc.range.start.line == 1 for loc in locs
    ), (
        f"expected only_a in crypto.h line 1, got {[(loc.uri, loc.range.start.line) for loc in locs]}"
    )


async def test_no_duplicate_reference_rows(client, tmp_path):
    (tmp_path / "foo.h").write_text(
        "inline void foo() {}\ninline void bar() { foo(); }\n"
    )
    (tmp_path / "main.cpp").write_text(
        '#include "foo.h"\nint main() { foo(); return 0; }\n'
    )
    (tmp_path / "other.cpp").write_text('#include "foo.h"\nint other() { return 0; }\n')
    write_cdb(tmp_path, ["main.cpp", "other.cpp"])
    await client.initialize(tmp_path)

    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    # Open files are skipped by background indexing; the closed other.cpp
    # is what carries foo.h's rows into the disk index.
    assert await wait_for_index(client, uri, "bar"), "Index not ready after 30s"

    # The header's rows exist in both its disk shard and the overlay; the
    # union must collapse them.
    refs = locations_of(await client.references_at(uri, 1, 13))
    keys = [(r.uri, r.range.start.line, r.range.start.character) for r in refs]
    assert len(keys) == len(set(keys)), f"duplicate reference rows: {keys}"
    assert any(key[0].endswith("foo.h") for key in keys)


async def test_preamble_macro_definition(client, tmp_path):
    (tmp_path / "main.cpp").write_text(
        "#define ANSWER 42\nint main() { return ANSWER; }\n"
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path, initialization_options=NO_INDEXING)

    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    # The #define lives in the preamble region swallowed by the PCH; its
    # definition is served from the overlay's main-file entry.
    locs = locations_of(await client.definition_at(uri, 1, 20))
    assert any(
        loc.uri.endswith("main.cpp") and loc.range.start.line == 0 for loc in locs
    ), (
        f"expected #define on line 0, got {[(loc.uri, loc.range.start.line) for loc in locs]}"
    )


async def test_preamble_links_survive_restart(executable, tmp_path):
    pin_cache_to_workspace(tmp_path)
    (tmp_path / "foo.h").write_text("inline void foo() {}\n")
    (tmp_path / "main.cpp").write_text(
        '#include "foo.h"\nint main() { foo(); return 0; }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])

    c1 = await make_client(executable, tmp_path)
    uri, _ = await c1.open_and_wait(tmp_path / "main.cpp")
    links = await c1.document_links(uri)
    assert any(link.target.endswith("foo.h") for link in links)
    pch_mtime = list_pch_files(tmp_path)[0].stat().st_mtime
    await shutdown_client(c1)

    # Session 2 hits the persisted PCH pair: the preamble's links must be
    # served from the reloaded blob, not lost with the process.
    c2 = await make_client(executable, tmp_path)
    uri2, _ = await c2.open_and_wait(tmp_path / "main.cpp")
    links2 = await c2.document_links(uri2)
    assert any(link.target.endswith("foo.h") for link in links2), (
        "preamble document links lost across restart"
    )
    assert list_pch_files(tmp_path)[0].stat().st_mtime == pch_mtime, (
        "PCH was rebuilt instead of reused"
    )
    await shutdown_client(c2)


async def test_missing_idx_rebuilds_pair(executable, tmp_path):
    pin_cache_to_workspace(tmp_path)
    (tmp_path / "foo.h").write_text("inline void foo() {}\n")
    (tmp_path / "main.cpp").write_text(
        '#include "foo.h"\nint main() { foo(); return 0; }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])

    c1 = await make_client(executable, tmp_path)
    uri, _ = await c1.open_and_wait(tmp_path / "main.cpp")
    locs = locations_of(await c1.definition_at(uri, 1, 13))
    assert any(loc.uri.endswith("foo.h") for loc in locs)
    pch_mtime = list_pch_files(tmp_path)[0].stat().st_mtime
    await shutdown_client(c1)

    # Half the pair vanishes (crash residue, external cleanup): the next
    # session must treat the PCH as a miss and rebuild both blobs.
    idx_files = list((cache_root(tmp_path) / "pch").glob("*.pch.idx"))
    assert idx_files, "expected a committed .pch.idx next to the PCH"
    for idx in idx_files:
        idx.unlink()
    await asyncio.sleep(MTIME_GRANULARITY)

    c2 = await make_client(executable, tmp_path)
    uri2, _ = await c2.open_and_wait(tmp_path / "main.cpp")
    locs2 = locations_of(await c2.definition_at(uri2, 1, 13))
    assert any(loc.uri.endswith("foo.h") for loc in locs2), (
        "overlay dead after losing the idx half of the pair"
    )
    assert list_pch_files(tmp_path)[0].stat().st_mtime != pch_mtime, (
        "PCH pair should have been rebuilt"
    )
    assert list((cache_root(tmp_path) / "pch").glob("*.pch.idx")), (
        "rebuilt pair is missing its idx blob"
    )
    await shutdown_client(c2)


async def test_header_edit_refreshes_overlay(client, tmp_path):
    (tmp_path / "foo.h").write_text("inline void foo() {}\n")
    (tmp_path / "main.cpp").write_text(
        '#include "foo.h"\nint main() { foo(); return 0; }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path, initialization_options=NO_INDEXING)

    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    locs = locations_of(await client.definition_at(uri, 1, 13))
    assert any(loc.range.start.line == 0 for loc in locs)

    # Same preamble text, so the PCH key is unchanged; the header edit must
    # still refresh the pair (deps_changed) and the served overlay with it.
    await asyncio.sleep(MTIME_GRANULARITY)
    (tmp_path / "foo.h").write_text("// moved\ninline void foo() {}\n")
    await wait_for_recompile(client, uri)

    locs = locations_of(await client.definition_at(uri, 1, 13))
    assert any(
        loc.uri.endswith("foo.h") and loc.range.start.line == 1 for loc in locs
    ), (
        f"overlay still serves pre-edit rows: {[(loc.uri, loc.range.start.line) for loc in locs]}"
    )
