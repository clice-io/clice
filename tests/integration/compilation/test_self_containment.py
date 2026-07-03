"""Integration tests for automatic self-containment detection.

Headers without a CDB entry compile self-contained first (borrowed host
command, no prefix synthesis). When the trial diagnostics indicate missing
includer context, the server falls back to prefix synthesis transparently —
only the final diagnostics are published. Verdicts and user context
choices persist across server sessions via cache.json.
"""

import json

from tests.conftest import make_client, shutdown_client
from tests.integration.utils import write_cdb
from tests.integration.utils.assertions import assert_clean_compile
from tests.integration.utils.cache import read_cache_json


def get_field(obj, key, default=None):
    if isinstance(obj, dict):
        return obj.get(key, default)
    return getattr(obj, key, default)


def prefix_files(workspace):
    prefix_dir = workspace / ".clice" / "header_context"
    return sorted(prefix_dir.glob("*.h")) if prefix_dir.exists() else []


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
    entries = [
        {
            "directory": str(tmp_path),
            "file": str(tmp_path / f),
            "arguments": ["clang++", "-std=c++17", "-fsyntax-only", str(tmp_path / f)],
        }
        for f in ("a.cpp", "b.cpp")
    ]
    (tmp_path / "compile_commands.json").write_text(json.dumps(entries))

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
