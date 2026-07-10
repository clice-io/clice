"""Integration tests for the clice/inactiveRegions notification.

Pushed after every compile with the untaken #if branch bodies of the
current compilation context. The preamble's share comes from the PCH
build (conditions inside the bound never replay in the AST compile);
a #if cut by the bound resumes via the open-conditional stack.
"""

import asyncio

from tests.tools.compile_commands import write_cdb, write_entries


async def wait_regions(captured, timeout=15.0):
    deadline = asyncio.get_event_loop().time() + timeout
    while asyncio.get_event_loop().time() < deadline:
        if captured and captured[-1].regions:
            return captured[-1].regions
        await asyncio.sleep(0.05)
    return captured[-1].regions if captured else []


def capture(client):
    captured = []

    @client.feature("clice/inactiveRegions")
    def on_inactive(params):
        captured.append(params)

    return captured


async def test_inactive_after_bound(client, tmp_path):
    """Conditions entirely past the preamble bound (no PCH involvement)."""
    (tmp_path / "main.cpp").write_text(
        "int a();\n#if 0\nint dead();\n#endif\nint main() { return 0; }\n"
    )
    write_cdb(tmp_path, ["main.cpp"])
    captured = capture(client)

    await client.initialize(tmp_path)
    await client.open_and_wait(tmp_path / "main.cpp")
    regions = await wait_regions(captured)
    assert [(r.start.line, r.end.line) for r in regions] == [(2, 3)], regions


async def test_inactive_across_bound(client, tmp_path):
    """A #if inside the preamble bound lives in the PCH; its #elif/#endif
    replay in the AST compile and resume from the PCH's open stack."""
    (tmp_path / "render.h").write_text(
        "#pragma once\n"
        "#if defined(USE_VULKAN)\n"
        'inline const char* backend() { return "vk"; }\n'
        "#elif defined(USE_METAL)\n"
        'inline const char* backend() { return "mt"; }\n'
        "#endif\n"
    )
    (tmp_path / "render_vk.cpp").write_text(
        '#include "render.h"\nint main() { return backend()[0]; }\n'
    )
    write_entries(tmp_path, [("render_vk.cpp", ["-DUSE_VULKAN"])])
    captured = capture(client)

    await client.initialize(tmp_path)
    await client.open_and_wait(tmp_path / "render.h")
    regions = await wait_regions(captured)
    assert [(r.start.line, r.end.line) for r in regions] == [(4, 5)], regions


async def test_inactive_else_branch(client, tmp_path):
    """#else carries no condition value; inactivity is derived from
    whether an earlier branch was taken."""
    (tmp_path / "main.cpp").write_text(
        "#define USE_A 1\n"
        "int x();\n"
        "#if USE_A\n"
        "int active();\n"
        "#else\n"
        "int dead();\n"
        "#endif\n"
        "int main() { return 0; }\n"
    )
    write_cdb(tmp_path, ["main.cpp"])
    captured = capture(client)

    await client.initialize(tmp_path)
    await client.open_and_wait(tmp_path / "main.cpp")
    regions = await wait_regions(captured)
    assert [(r.start.line, r.end.line) for r in regions] == [(5, 6)], regions
