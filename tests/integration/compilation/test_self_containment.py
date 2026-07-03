"""Integration tests for automatic self-containment detection.

Headers without a CDB entry compile self-contained first (borrowed host
command, no prefix synthesis). When the trial diagnostics indicate missing
includer context, the server falls back to prefix synthesis transparently —
only the final diagnostics are published. Verdicts and user context
choices persist across server sessions via cache.json.
"""

from tests.conftest import make_client, shutdown_client
from tests.integration.utils import get_field, write_cdb, write_entries
from tests.integration.utils.assertions import assert_clean_compile
from tests.integration.utils.cache import read_cache_json
from tests.integration.utils.wait import wait_for_recompile


def prefix_files(workspace):
    prefix_dir = workspace / ".clice" / "header_context"
    if not prefix_dir.exists():
        return []
    return sorted(
        f
        for f in prefix_dir.glob("*.h")
        if not f.name.endswith((".suffix.h", ".self.h"))
    )


async def test_self_contained_skips_synthesis(client, tmp_path):
    """A self-contained header borrows a command but gets no prefix."""
    (tmp_path / "types.h").write_text("#pragma once\nstruct Point { int x; int y; };\n")
    (tmp_path / "helper.h").write_text(
        '#pragma once\n#include "types.h"\ninline int get_x(Point p) { return p.x; }\n'
    )
    (tmp_path / "main.cpp").write_text(
        '#include "helper.h"\nint main() { return get_x({1, 2}); }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    await client.open_and_wait(tmp_path / "main.cpp")
    helper_uri, _ = await client.open_and_wait(tmp_path / "helper.h")
    assert_clean_compile(client, helper_uri)
    assert prefix_files(tmp_path) == [], (
        "Self-contained headers must not synthesize a prefix"
    )


async def test_fallback_on_missing_context(client, tmp_path):
    """A non-self-contained header falls back to prefix synthesis
    automatically; the trial's error diagnostics are never published."""
    (tmp_path / "types.h").write_text("#pragma once\nstruct Point { int x; int y; };\n")
    (tmp_path / "utils.h").write_text("inline int get_x(Point p) { return p.x; }\n")
    (tmp_path / "main.cpp").write_text(
        '#include "types.h"\n#include "utils.h"\nint main() { return get_x({1, 2}); }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    await client.open_and_wait(tmp_path / "main.cpp")
    utils_uri, _ = await client.open_and_wait(tmp_path / "utils.h")
    assert_clean_compile(client, utils_uri)
    assert len(prefix_files(tmp_path)) == 1, (
        "Fallback must synthesize exactly one prefix"
    )


async def test_verdict_persisted_across_sessions(executable, tmp_path):
    """The NeedsContext verdict lands in cache.json and survives restarts."""
    (tmp_path / "types.h").write_text("#pragma once\nstruct Point { int x; int y; };\n")
    (tmp_path / "utils.h").write_text("inline int get_x(Point p) { return p.x; }\n")
    (tmp_path / "main.cpp").write_text(
        '#include "types.h"\n#include "utils.h"\nint main() { return get_x({1, 2}); }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])

    c1 = await make_client(executable, tmp_path)
    await c1.open_and_wait(tmp_path / "main.cpp")
    utils_uri, _ = await c1.open_and_wait(tmp_path / "utils.h")
    assert_clean_compile(c1, utils_uri)
    await shutdown_client(c1)

    cache = read_cache_json(tmp_path)
    assert cache is not None and cache.get("header_modes"), (
        f"Expected a persisted header mode, got: {cache and cache.keys()}"
    )

    c2 = await make_client(executable, tmp_path)
    await c2.open_and_wait(tmp_path / "main.cpp")
    utils_uri2, _ = await c2.open_and_wait(tmp_path / "utils.h")
    assert_clean_compile(c2, utils_uri2)
    await shutdown_client(c2)


async def test_choice_persisted_across_sessions(executable, tmp_path):
    """A switchContext choice is restored on didOpen in a later session."""
    (tmp_path / "shared.h").write_text("VALUE_TYPE get_value();\n")
    (tmp_path / "a.cpp").write_text(
        '#define VALUE_TYPE int\n#include "shared.h"\nint main() { return 0; }\n'
    )
    (tmp_path / "b.cpp").write_text(
        '#define VALUE_TYPE float\n#include "shared.h"\nfloat f() { return 0; }\n'
    )
    write_entries(tmp_path, [("a.cpp", []), ("b.cpp", [])])

    c1 = await make_client(executable, tmp_path)
    await c1.open_and_wait(tmp_path / "a.cpp")
    await c1.open_and_wait(tmp_path / "b.cpp")
    shared_uri, _ = c1.open(tmp_path / "shared.h")
    b_uri = c1.path_to_uri(tmp_path / "b.cpp")
    switch = await c1.switch_context(shared_uri, b_uri)
    assert get_field(switch, "success") is True
    await shutdown_client(c1)

    c2 = await make_client(executable, tmp_path)
    shared_uri2, _ = c2.open(tmp_path / "shared.h")
    current = await c2.current_context(shared_uri2)
    ctx = get_field(current, "context")
    assert ctx is not None and "b.cpp" in get_field(ctx, "uri"), (
        f"Persisted context choice should be restored on didOpen, got: {current}"
    )
    await shutdown_client(c2)


async def test_ordinary_error_no_fallback(client, tmp_path):
    """A self-contained header with a benign syntax error must not trigger
    prefix synthesis nor persist any verdict."""
    (tmp_path / "typo.h").write_text("inline int broken() { return }\n")  # syntax error
    (tmp_path / "main.cpp").write_text('#include "typo.h"\nint main() { return 0; }\n')
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    typo_uri, _ = await client.open_and_wait(tmp_path / "typo.h")
    diags = client.diagnostics.get(typo_uri, [])
    assert diags, "The syntax error must be published"
    assert prefix_files(tmp_path) == [], (
        "Ordinary errors must not trigger prefix synthesis"
    )


async def test_header_save_resets_verdict(executable, tmp_path):
    """Saving the header itself re-evaluates its self-containment: a header
    that gains its own include stops using the synthesized prefix."""
    import asyncio

    from lsprotocol.types import (
        DidChangeTextDocumentParams,
        DidSaveTextDocumentParams,
        TextDocumentContentChangeWholeDocument,
        VersionedTextDocumentIdentifier,
    )

    from tests.integration.utils import doc

    (tmp_path / "types.h").write_text("#pragma once\nstruct Point { int x; int y; };\n")
    utils_h = tmp_path / "utils.h"
    utils_h.write_text("inline int get_x(Point p) { return p.x; }\n")
    (tmp_path / "main.cpp").write_text(
        '#include "types.h"\n#include "utils.h"\nint main() { return get_x({1, 2}); }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])

    c = await make_client(executable, tmp_path)
    await c.open_and_wait(tmp_path / "main.cpp")
    utils_uri, _ = await c.open_and_wait(utils_h)
    assert_clean_compile(c, utils_uri)
    assert len(prefix_files(tmp_path)) == 1, "Initial verdict: needs context"

    # Make the header self-contained on disk and in the buffer, then save.
    await asyncio.sleep(1.1)
    new_text = '#include "types.h"\ninline int get_x(Point p) { return p.x; }\n'
    utils_h.write_text(new_text)
    c.text_document_did_change(
        DidChangeTextDocumentParams(
            text_document=VersionedTextDocumentIdentifier(uri=utils_uri, version=2),
            content_changes=[TextDocumentContentChangeWholeDocument(text=new_text)],
        )
    )
    c.text_document_did_save(DidSaveTextDocumentParams(text_document=doc(utils_uri)))

    await wait_for_recompile(c, utils_uri)
    assert_clean_compile(c, utils_uri)
    await shutdown_client(c)

    # After shutdown the persisted verdict must be gone.
    cache = read_cache_json(tmp_path)
    assert not (cache or {}).get("header_modes"), (
        f"Verdict should be reset after the header was saved, got: {cache}"
    )


async def test_dependency_change_retries_trial(client, tmp_path):
    """A header judged self-contained must be re-evaluated when one of its
    own includes changes: here foo.h stops providing FOO, and only the
    includer context (the host's define) can still supply it."""
    import asyncio

    (tmp_path / "foo.h").write_text("#pragma once\n#define FOO 1\n")
    (tmp_path / "h.h").write_text(
        '#pragma once\n#include "foo.h"\ninline int get() { return FOO; }\n'
    )
    (tmp_path / "main.cpp").write_text(
        '#define FOO 2\n#include "h.h"\nint main() { return get(); }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    await client.open_and_wait(tmp_path / "main.cpp")
    h_uri, _ = await client.open_and_wait(tmp_path / "h.h")
    assert_clean_compile(client, h_uri)
    assert prefix_files(tmp_path) == [], "Initially self-contained"

    # foo.h stops defining FOO; only the host's #define can provide it now.
    await asyncio.sleep(1.1)
    (tmp_path / "foo.h").write_text("#pragma once\n")

    await wait_for_recompile(client, h_uri)
    assert_clean_compile(client, h_uri)
    assert len(prefix_files(tmp_path)) == 1, (
        "Dependency change must re-run the trial and fall back to synthesis"
    )


async def test_suffix_closes_embedding(client, tmp_path):
    """X-macro fragments embedded in an enum or a function body compile
    cleanly: the synthesized suffix closes the surrounding braces."""
    (tmp_path / "errors.def").write_text(
        'X(Ok, 0, "success")\nX(NotFound, 1, "not found")\n'
    )
    (tmp_path / "main.cpp").write_text(
        "#define X(name, code, msg) name = code,\n"
        "enum ErrorCode {\n"
        '#include "errors.def"\n'
        "};\n"
        "#undef X\n"
        "int main() { return Ok; }\n"
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    await client.open_and_wait(tmp_path / "main.cpp")
    def_uri, _ = await client.open_and_wait(tmp_path / "errors.def")
    assert_clean_compile(client, def_uri)


async def test_suffix_function_body(client, tmp_path):
    """The doc's classic register_all() case: statements expanded inside a
    function body, closing brace restored by the suffix."""
    (tmp_path / "handlers.def").write_text("X(alpha)\nX(beta)\n")
    (tmp_path / "main.cpp").write_text(
        "inline void handle(int) {}\n"
        "enum Ids { alpha, beta };\n"
        "void register_all() {\n"
        "#define X(name) handle(name);\n"
        '#include "handlers.def"\n'
        "#undef X\n"
        "}\n"
        "int main() { register_all(); return 0; }\n"
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    await client.open_and_wait(tmp_path / "main.cpp")
    def_uri, _ = await client.open_and_wait(tmp_path / "handlers.def")
    assert_clean_compile(client, def_uri)


async def test_open_synthesized_artifact(client, tmp_path):
    """Opening a synthesized prefix file compiles it with its host's
    command (it is a fragment of that TU), not with junk context."""
    (tmp_path / "types.h").write_text("#pragma once\nstruct Point { int x; int y; };\n")
    (tmp_path / "utils.h").write_text("inline int get_x(Point p) { return p.x; }\n")
    (tmp_path / "main.cpp").write_text(
        '#include "types.h"\n#include "utils.h"\nint main() { return get_x({1, 2}); }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    await client.open_and_wait(tmp_path / "main.cpp")
    utils_uri, _ = await client.open_and_wait(tmp_path / "utils.h")
    assert_clean_compile(client, utils_uri)

    prefixes = prefix_files(tmp_path)
    assert len(prefixes) == 1
    prefix_uri, _ = await client.open_and_wait(prefixes[0])
    assert_clean_compile(client, prefix_uri)

    # No context of its own, and no further synthesis chained off it.
    q = await client.query_context(prefix_uri)
    assert get_field(q, "total") == 0, q
    assert len(prefix_files(tmp_path)) == 1, (
        "Opening an artifact must not synthesize more"
    )


async def test_unbalanced_brace_degrades_gracefully(client, tmp_path):
    """A user-typed unbalanced brace in an embedded fragment steals the
    suffix's closer: diagnostics must appear, and the server must keep
    serving requests afterwards."""
    (tmp_path / "list.def").write_text("X(alpha)\nvoid oops() {\n")  # unbalanced {
    (tmp_path / "main.cpp").write_text(
        "#define X(name) int name;\n"
        "enum Ids {\n"
        '#include "list.def"\n'
        "};\n"
        "#undef X\n"
        "int main() { return 0; }\n"
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    await client.open_and_wait(tmp_path / "main.cpp")
    def_uri, _ = await client.open_and_wait(tmp_path / "list.def")
    diags = client.diagnostics.get(def_uri, [])
    assert diags, "The imbalance must surface as diagnostics"

    # The server stays healthy: a follow-up request still answers.
    q = await client.query_context(def_uri)
    assert get_field(q, "total") >= 1
