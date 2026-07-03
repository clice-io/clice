"""Integration tests for compilation context switching.

Covers source files with multiple CDB entries (selected by command hash),
include-occurrence contexts for guard-less headers, context deduplication
by canonical flags, host ranking, and switch validation.
"""

from tests.integration.utils import get_field, write_cdb, write_entries
from tests.integration.utils.assertions import assert_clean_compile, assert_has_errors
from tests.integration.utils.wait import wait_for_recompile


async def test_source_command_switch(client, tmp_path):
    """Switching between two CDB entries of one source must recompile it
    under the selected flags."""
    (tmp_path / "main.cpp").write_text(
        "#ifndef EXPECTED\n#error missing EXPECTED\n#endif\nint main() { return 0; }\n"
    )
    write_entries(tmp_path, [("main.cpp", ["-DEXPECTED"]), ("main.cpp", [])])
    await client.initialize(tmp_path)

    # Default entry is the first one: EXPECTED defined, clean.
    main_uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert_clean_compile(client, main_uri)

    query = await client.query_context(main_uri)
    assert get_field(query, "total") == 2
    contexts = get_field(query, "contexts", [])
    hashes = {get_field(c, "label"): get_field(c, "commandHash") for c in contexts}
    assert all(hashes.values()), (
        f"Source contexts must carry commandHash, got: {contexts}"
    )
    plain_hash = next(h for l, h in hashes.items() if "-DEXPECTED" not in l)
    defined_hash = next(h for l, h in hashes.items() if "-DEXPECTED" in l)

    # Switch to the entry without the define: the #error must fire.
    switch = await client.switch_context(main_uri, main_uri, command_hash=plain_hash)
    assert get_field(switch, "success") is True
    await wait_for_recompile(client, main_uri)
    assert_has_errors(client, main_uri, "Expected #error without -DEXPECTED")

    current = await client.current_context(main_uri)
    assert get_field(get_field(current, "context"), "commandHash") == plain_hash

    # And back.
    switch = await client.switch_context(main_uri, main_uri, command_hash=defined_hash)
    assert get_field(switch, "success") is True
    await wait_for_recompile(client, main_uri)
    assert_clean_compile(client, main_uri)


async def test_occurrence_switch(client, tmp_path):
    """A guard-less header included twice by one host provides one context
    per include occurrence."""
    (tmp_path / "list.def").write_text("X(alpha)\n")
    (tmp_path / "main.cpp").write_text(
        "#define X(name) int name;\n"
        '#include "list.def"\n'
        "#undef X\n"
        "#define X(name) void get_##name();\n"
        '#include "list.def"\n'
        "#undef X\n"
        "int main() { return alpha; }\n"
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    main_uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    def_uri, _ = await client.open_and_wait(tmp_path / "list.def")

    query = await client.query_context(def_uri)
    assert get_field(query, "total") == 2, (
        f"Expected 2 occurrence contexts, got {query}"
    )
    contexts = get_field(query, "contexts", [])
    occurrences = sorted(get_field(c, "occurrence") for c in contexts)
    assert occurrences == [0, 1], f"Expected occurrences 0 and 1, got: {contexts}"

    # Pin each occurrence; both must compile cleanly and be reported back.
    for occ in (0, 1):
        switch = await client.switch_context(def_uri, main_uri, occurrence=occ)
        assert get_field(switch, "success") is True, f"switch to occurrence {occ}"
        await wait_for_recompile(client, def_uri)
        assert_clean_compile(client, def_uri)

        current = await client.current_context(def_uri)
        assert get_field(get_field(current, "context"), "occurrence") == occ


async def test_context_dedup_and_ranking(client, tmp_path):
    """Hosts with identical canonical flags collapse into one context, and
    the representative is the best-ranked host (matching stem wins)."""
    (tmp_path / "widget.h").write_text("inline int widget_size() { return 4; }\n")
    for name in ("zzz.cpp", "widget.cpp", "aaa.cpp"):
        (tmp_path / name).write_text(
            f'#include "widget.h"\nint {name[0]}() {{ return widget_size(); }}\n'
        )
    write_cdb(tmp_path, ["zzz.cpp", "widget.cpp", "aaa.cpp"])
    await client.initialize(tmp_path)

    await client.open_and_wait(tmp_path / "widget.cpp")
    widget_uri, _ = client.open(tmp_path / "widget.h")

    query = await client.query_context(widget_uri)
    assert get_field(query, "total") == 1, (
        f"Identical flags must dedupe to one context, got: {query}"
    )
    contexts = get_field(query, "contexts", [])
    assert "widget.cpp" in get_field(contexts[0], "uri"), (
        f"Representative should be the stem-matching host, got: {contexts}"
    )


async def test_switch_rejects_non_includer(client, tmp_path):
    """Switching a header to a source that does not include it must fail."""
    (tmp_path / "utils.h").write_text("inline int util() { return 1; }\n")
    (tmp_path / "main.cpp").write_text(
        '#include "utils.h"\nint main() { return util(); }\n'
    )
    (tmp_path / "other.cpp").write_text("int other() { return 2; }\n")
    write_cdb(tmp_path, ["main.cpp", "other.cpp"])
    await client.initialize(tmp_path)

    await client.open_and_wait(tmp_path / "main.cpp")
    utils_uri, _ = client.open(tmp_path / "utils.h")

    other_uri = client.path_to_uri(tmp_path / "other.cpp")
    switch = await client.switch_context(utils_uri, other_uri)
    assert get_field(switch, "success") is False, (
        "Switching to a non-including host must be rejected"
    )


async def test_occurrence_out_of_range(client, tmp_path):
    """Pinning an occurrence beyond the include count must be rejected."""
    (tmp_path / "list.def").write_text("X(alpha)\n")
    (tmp_path / "main.cpp").write_text(
        "#define X(name) int name;\n"
        '#include "list.def"\n'
        "#undef X\n"
        "int main() { return alpha; }\n"
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    main_uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    def_uri, _ = client.open(tmp_path / "list.def")

    switch = await client.switch_context(def_uri, main_uri, occurrence=5)
    assert get_field(switch, "success") is False, (
        "Out-of-range occurrence must be rejected"
    )


async def test_query_context_pagination(client, tmp_path):
    """queryContext pages results: 12 distinct configs yield 10 + 2."""
    (tmp_path / "common.h").write_text("inline int common() { return 1; }\n")
    entries = []
    for n in range(12):
        name = f"s{n:02}.cpp"
        (tmp_path / name).write_text(
            f'#include "common.h"\nint f{n}() {{ return common() + FLAVOR; }}\n'
        )
        entries.append((name, [f"-DFLAVOR={n}"]))
    write_entries(tmp_path, entries)
    await client.initialize(tmp_path)

    await client.open_and_wait(tmp_path / "s00.cpp")
    common_uri, _ = client.open(tmp_path / "common.h")

    first = await client.query_context(common_uri)
    assert get_field(first, "total") == 12
    assert len(get_field(first, "contexts", [])) == 10

    second = await client.query_context(common_uri, offset=10)
    assert get_field(second, "total") == 12
    assert len(get_field(second, "contexts", [])) == 2

    # The two pages must not overlap.
    first_uris = {get_field(c, "uri") for c in get_field(first, "contexts", [])}
    second_uris = {get_field(c, "uri") for c in get_field(second, "contexts", [])}
    assert not (first_uris & second_uris)


async def test_stale_epoch_rejected(client, tmp_path):
    """A switch made against an outdated queryContext listing is rejected
    with stale=true; re-querying yields a fresh epoch that works."""
    import asyncio

    from lsprotocol.types import DidSaveTextDocumentParams

    from tests.integration.utils import doc

    (tmp_path / "shared.h").write_text("VALUE_TYPE get_value();\n")
    (tmp_path / "main.cpp").write_text(
        '#define VALUE_TYPE int\n#include "shared.h"\nint main() { return 0; }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    main_uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    shared_uri, _ = client.open(tmp_path / "shared.h")

    query = await client.query_context(shared_uri)
    old_epoch = get_field(query, "epoch")
    assert old_epoch, f"queryContext must stamp an epoch, got: {query}"

    # Any save bumps the workspace epoch.
    client.text_document_did_save(
        DidSaveTextDocumentParams(text_document=doc(main_uri))
    )
    await asyncio.sleep(0.5)

    switch = await client.switch_context(shared_uri, main_uri, epoch=old_epoch)
    assert get_field(switch, "success") is False
    assert get_field(switch, "stale") is True, (
        f"Expected stale rejection, got: {switch}"
    )

    fresh = await client.query_context(shared_uri)
    switch = await client.switch_context(
        shared_uri, main_uri, epoch=get_field(fresh, "epoch")
    )
    assert get_field(switch, "success") is True, f"Fresh epoch must work, got: {switch}"
