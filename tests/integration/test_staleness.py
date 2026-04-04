"""Integration tests for mtime-based staleness tracking.

Verifies that ensure_compiled() and ensure_pch() detect dependency file
changes via mtime snapshots, triggering recompilation without relying
on didSave to mark everything dirty.
"""

import asyncio
import json
import os
import shutil

import pytest
from lsprotocol.types import (
    DidSaveTextDocumentParams,
    HoverParams,
    Position,
    TextDocumentIdentifier,
)


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


async def test_header_change_invalidates_ast(client, tmp_path):
    """Modifying a header on disk should cause recompilation on next hover,
    even though didSave was never called (mtime-based detection)."""
    # Setup: main.cpp includes header.h
    (tmp_path / "header.h").write_text("inline int value() { return 1; }\n")
    (tmp_path / "main.cpp").write_text(
        '#include "header.h"\nint main() { return value(); }\n'
    )
    _write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    # First compile — should succeed with no diagnostics.
    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected clean compile, got: {diags}"

    # Modify header on disk — introduce an error.
    # Sleep briefly to ensure mtime changes (filesystem granularity).
    # Ensure mtime advances past filesystem granularity (1s on some FSes).
    await asyncio.sleep(1.1)
    (tmp_path / "header.h").write_text(
        "inline int value() { return }\n"
    )  # syntax error

    # Send another hover — ensure_compiled should detect mtime change
    # in deps and trigger recompilation. The recompilation publishes
    # fresh diagnostics as a side effect.
    event = client.wait_for_diagnostics(uri)
    await client.text_document_hover_async(
        HoverParams(text_document=_doc(uri), position=Position(line=0, character=0))
    )
    await asyncio.wait_for(event.wait(), timeout=60.0)

    # Should now have diagnostics from the broken header.
    diags = client.diagnostics.get(uri, [])
    assert len(diags) > 0, "Expected diagnostics after header change"


async def test_header_change_invalidates_pch(client, tmp_path):
    """Modifying a preamble header on disk should trigger PCH rebuild."""
    (tmp_path / "header.h").write_text("#pragma once\nstruct Foo { int x; };\n")
    (tmp_path / "main.cpp").write_text(
        '#include "header.h"\nint main() { Foo f; return f.x; }\n'
    )
    _write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    # First compile — success.
    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0

    # Modify header — rename struct field.
    # Ensure mtime advances past filesystem granularity (1s on some FSes).
    await asyncio.sleep(1.1)
    (tmp_path / "header.h").write_text(
        "#pragma once\nstruct Foo { int y; };\n"  # x -> y
    )

    # Hover again — PCH should rebuild, AST should recompile.
    # main.cpp uses f.x which no longer exists → diagnostics expected.
    event = client.wait_for_diagnostics(uri)
    await client.text_document_hover_async(
        HoverParams(text_document=_doc(uri), position=Position(line=0, character=0))
    )
    await asyncio.wait_for(event.wait(), timeout=30.0)

    diags = client.diagnostics.get(uri, [])
    assert len(diags) > 0, "Expected error after header field rename"


async def test_no_change_skips_recompile(client, tmp_path):
    """When no dependency has changed, ensure_compiled should fast-path."""
    (tmp_path / "main.cpp").write_text("int main() { return 0; }\n")
    _write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0

    # Second hover — should use cached AST (no recompilation).
    # Verify it returns quickly and doesn't crash.
    hover = await client.text_document_hover_async(
        HoverParams(text_document=_doc(uri), position=Position(line=0, character=4))
    )
    # "main" should be hoverable.
    assert hover is not None


async def test_didsave_with_module_deps(client, test_data_dir, tmp_path):
    """didSave on a module file should invalidate CompileGraph dependents."""
    src = test_data_dir / "modules" / "save_recompile"
    for f in src.iterdir():
        if f.is_file():
            shutil.copy2(f, tmp_path / f.name)

    from tests.conftest import generate_cdb

    generate_cdb(tmp_path)
    await client.initialize(tmp_path)

    # Open and compile Mid (which imports Leaf).
    mid_uri, _ = await client.open_and_wait(tmp_path / "mid.cppm")
    diags = client.diagnostics.get(mid_uri, [])
    assert len(diags) == 0

    # Modify Leaf on disk and send didSave — should invalidate Mid's deps.
    new_leaf = "export module Leaf;\nexport int leaf() { return 999; }\n"
    (tmp_path / "leaf.cppm").write_text(new_leaf)

    leaf_path = tmp_path / "leaf.cppm"
    client.text_document_did_save(
        DidSaveTextDocumentParams(
            text_document=TextDocumentIdentifier(uri=leaf_path.as_uri())
        )
    )

    # Hover on Mid should trigger recompilation (Leaf PCM was invalidated).
    event = client.wait_for_diagnostics(mid_uri)
    await client.text_document_hover_async(
        HoverParams(text_document=_doc(mid_uri), position=Position(line=0, character=0))
    )
    await asyncio.wait_for(event.wait(), timeout=60.0)

    diags = client.diagnostics.get(mid_uri, [])
    assert len(diags) == 0, f"Expected clean compile after module update, got: {diags}"
