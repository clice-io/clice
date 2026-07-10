"""Integration tests for PCH interaction with header contexts.

Covers the unified-preamble design: the -include'd synthesized prefix is
baked into the PCH via the predefines buffer, clang's PPOpts validation
subsumes the -include on reuse (no double processing), and a header with
no directives of its own (bound == 0) still gets a PCH.
"""

from tests.tools.compile_commands import write_cdb
from tests.tools.checks import assert_clean_compile
from tests.tools.workspace import list_pch_files


async def test_prefix_not_reprocessed(client, tmp_path):
    """A bare definition in the synthesized prefix must not be processed
    twice (once in the PCH, once via -include) — that would be an error."""
    (tmp_path / "utils.h").write_text(
        "inline int next_id() { return shared_counter + 1; }\n"
    )
    (tmp_path / "main.cpp").write_text(
        'int shared_counter = 0;\n#include "utils.h"\nint main() { return next_id(); }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    await client.open_and_wait(tmp_path / "main.cpp")
    utils_uri, _ = await client.open_and_wait(tmp_path / "utils.h")
    assert_clean_compile(client, utils_uri)


async def test_def_file_builds_pch(client, tmp_path):
    """An X-macro .def file has no directives of its own (bound == 0), but
    the header context PCH must still be built to cache the prefix."""
    (tmp_path / "errors.def").write_text("X(ok, 0)\nX(bad, 1)\n")
    (tmp_path / "main.cpp").write_text(
        "#define X(name, code) inline int handle_##name() { return code; }\n"
        '#include "errors.def"\n'
        "#undef X\n"
        "int main() { return handle_ok(); }\n"
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    await client.open_and_wait(tmp_path / "main.cpp")
    before = len(list_pch_files(tmp_path))

    def_uri, _ = await client.open_and_wait(tmp_path / "errors.def")
    assert_clean_compile(client, def_uri)
    assert len(list_pch_files(tmp_path)) == before + 1, (
        "bound == 0 header context must still build a PCH for the prefix"
    )


async def test_bare_header_skips_pch(client, tmp_path):
    """A self-contained header with no directives of its own has nothing to
    precompile — no PCH must be built for it."""
    (tmp_path / "bare.h").write_text("inline int bare() { return 1; }\n")
    (tmp_path / "main.cpp").write_text(
        '#include "bare.h"\nint main() { return bare(); }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    await client.open_and_wait(tmp_path / "main.cpp")
    before = len(list_pch_files(tmp_path))

    bare_uri, _ = await client.open_and_wait(tmp_path / "bare.h")
    assert_clean_compile(client, bare_uri)
    assert len(list_pch_files(tmp_path)) == before, (
        "A self-contained header with an empty preamble must not build a PCH"
    )


async def test_links_merge_empty_pch(client, tmp_path):
    """documentLink must stay valid JSON when the PCH contributes no links
    (a preamble of only #defines) but the body has includes."""
    (tmp_path / "list.def").write_text("X(alpha)\n")
    (tmp_path / "main.cpp").write_text(
        "#define X(name) int name;\n"
        "int before = 0;\n"
        '#include "list.def"\n'
        "#undef X\n"
        "int main() { return alpha; }\n"
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    main_uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    links = await client.document_links(main_uri)
    assert links, "Expected a document link for the body include"
