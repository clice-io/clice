"""File tracker: CDB reload and disk-change discovery via clice/internal/poll.

The polling loops are disabled in tests (intervals 0); each test drives one
deterministic tick through the poll hook. The first workspace tick only
seeds the stat baseline, so tests poll once before mutating the disk.
"""

import asyncio

from tests.integration.utils import write_cdb
from tests.integration.utils.assertions import assert_has_errors, assert_no_errors
from tests.integration.utils.wait import (
    MTIME_GRANULARITY,
    wait_for_index,
    wait_for_recompile,
)
from tests.integration.utils.workspace import get_field

GATED_MAIN = """\
#ifndef FEATURE
#error missing FEATURE
#endif
int main() { return 0; }
"""

HEADER_V1 = """\
#define VALUE 1
#define TARGET alpha
inline int alpha() { return 1; }
inline int beta() { return 2; }
"""

HEADER_V2 = """\
#define VALUE 2
#define TARGET beta
inline int alpha() { return 1; }
inline int beta() { return 2; }
"""


async def events_of(client, loop):
    return get_field(await client.poll(loop), "events")


async def reference_uris(client, uri, line, character):
    refs = await client.references_at(uri, line, character, include_declaration=False)
    return [ref.uri for ref in (refs or [])]


async def wait_for_reference(client, uri, line, character, expected_uri, timeout=30):
    for _ in range(timeout):
        if expected_uri in await reference_uris(client, uri, line, character):
            return True
        await asyncio.sleep(1)
    return False


async def test_cdb_flag_change_recompiles(client, tmp_path):
    (tmp_path / "main.cpp").write_text(GATED_MAIN, newline="\n")
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    main_uri = (tmp_path / "main.cpp").as_uri()
    await client.open_and_wait(tmp_path / "main.cpp")
    assert_has_errors(client, main_uri, "gate must fire without -DFEATURE")

    write_cdb(tmp_path, ["main.cpp"], extra_args=["-DFEATURE"])
    assert await events_of(client, "cdb") == 1

    await wait_for_recompile(client, main_uri)
    assert_no_errors(client, main_uri, "open file must pick up the new flags")


async def test_cdb_new_entry_indexed(client, tmp_path):
    (tmp_path / "main.cpp").write_text("int main() { return 0; }\n", newline="\n")
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    main_uri = (tmp_path / "main.cpp").as_uri()
    await client.open_and_wait(tmp_path / "main.cpp")

    (tmp_path / "lib.cpp").write_text("int lib_entry() { return 1; }\n", newline="\n")
    write_cdb(tmp_path, ["main.cpp", "lib.cpp"])
    assert await events_of(client, "cdb") == 1

    assert await wait_for_index(client, main_uri, "lib_entry"), (
        "file added to the CDB was never indexed"
    )


async def test_cdb_removed_entry_recheck(client, tmp_path):
    (tmp_path / "header.h").write_text(
        "inline int shared() { return 0; }\n", newline="\n"
    )
    (tmp_path / "gone.cpp").write_text('#include "header.h"\n', newline="\n")
    write_cdb(tmp_path, ["gone.cpp"])
    await client.initialize(tmp_path)

    header_uri = (tmp_path / "header.h").as_uri()
    result = await client.query_context(header_uri)
    assert get_field(result, "total") >= 1, "gone.cpp must host the header initially"

    write_cdb(tmp_path, [])
    assert await events_of(client, "cdb") == 1

    result = await client.query_context(header_uri)
    assert get_field(result, "total") == 0, "removed entry must stop hosting the header"


async def test_cdb_appears_after_startup(client, tmp_path):
    (tmp_path / "main.cpp").write_text(GATED_MAIN, newline="\n")
    (tmp_path / "lib.cpp").write_text("int lib_entry() { return 1; }\n", newline="\n")
    await client.initialize(tmp_path)

    main_uri = (tmp_path / "main.cpp").as_uri()
    await client.open_and_wait(tmp_path / "main.cpp")
    assert_has_errors(client, main_uri, "guessed command cannot define FEATURE")

    # The editor was opened first; cmake runs later.
    write_cdb(tmp_path, ["main.cpp", "lib.cpp"], extra_args=["-DFEATURE"])
    assert await events_of(client, "cdb") == 1

    await wait_for_recompile(client, main_uri)
    assert_no_errors(client, main_uri, "open file must switch to the discovered CDB")
    assert await wait_for_index(client, main_uri, "lib_entry"), (
        "closed file from the discovered CDB was never indexed"
    )


async def test_checkout_updates_workspace(client, tmp_path):
    (tmp_path / "header.h").write_text(HEADER_V1, newline="\n")
    main_v1 = '#include "header.h"\nstatic_assert(VALUE == 2, "");\nint main() { return 0; }\n'
    (tmp_path / "main.cpp").write_text(main_v1, newline="\n")
    closed_v1 = '#include "header.h"\nint use_target() { return TARGET(); }\n'
    (tmp_path / "closed.cpp").write_text(closed_v1, newline="\n")
    write_cdb(tmp_path, ["main.cpp", "closed.cpp"])
    await client.initialize(tmp_path)

    header_uri = (tmp_path / "header.h").as_uri()
    main_uri = (tmp_path / "main.cpp").as_uri()
    closed_uri = (tmp_path / "closed.cpp").as_uri()
    await client.open_and_wait(tmp_path / "main.cpp")
    assert_has_errors(client, main_uri, "static_assert must fire against header V1")
    assert await wait_for_reference(client, header_uri, 2, 11, closed_uri), (
        "initial index never resolved the closed TU's alpha call"
    )

    assert await events_of(client, "workspace") == 0  # seeding sweep

    # Simulate git checkout: rewrite files on disk, no didSave.
    await asyncio.sleep(MTIME_GRANULARITY)
    (tmp_path / "header.h").write_text(HEADER_V2, newline="\n")
    (tmp_path / "closed.cpp").write_text(
        closed_v1 + "int checkout_added() { return 3; }\n", newline="\n"
    )
    assert await events_of(client, "workspace") == 2

    await wait_for_recompile(client, main_uri)
    assert_no_errors(client, main_uri, "open file must compile against the new header")
    assert await wait_for_reference(client, header_uri, 3, 11, closed_uri), (
        "closed TU was not reindexed against the new header"
    )
    assert await wait_for_index(client, main_uri, "checkout_added"), (
        "closed TU's own disk change was not indexed"
    )


async def test_touch_emits_no_events(client, tmp_path):
    (tmp_path / "header.h").write_text(HEADER_V1, newline="\n")
    (tmp_path / "main.cpp").write_text('#include "header.h"\n', newline="\n")
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    assert await events_of(client, "workspace") == 0  # seeding sweep

    # mtime bump, identical bytes: the content-hash check must stay silent.
    await asyncio.sleep(MTIME_GRANULARITY)
    (tmp_path / "header.h").write_text(HEADER_V1, newline="\n")
    assert await events_of(client, "workspace") == 0


async def test_cdb_polling_loop_live(client, tmp_path):
    (tmp_path / "main.cpp").write_text(GATED_MAIN, newline="\n")
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(
        tmp_path, initialization_options={"tracker": {"cdb_poll_seconds": 1}}
    )

    main_uri = (tmp_path / "main.cpp").as_uri()
    await client.open_and_wait(tmp_path / "main.cpp")
    assert_has_errors(client, main_uri)

    write_cdb(tmp_path, ["main.cpp"], extra_args=["-DFEATURE"])
    # No hook: the 1s poll loop needs two stable ticks (settle debounce).
    await asyncio.sleep(6)

    await wait_for_recompile(client, main_uri)
    assert_no_errors(
        client, main_uri, "the polling loop must reload the CDB on its own"
    )
