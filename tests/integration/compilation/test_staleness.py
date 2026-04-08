"""Integration tests for mtime-based staleness tracking.

Verifies that ensure_compiled() and ensure_pch() detect dependency file
changes via mtime snapshots, triggering recompilation without relying
on didSave to mark everything dirty.
"""

import asyncio
import shutil

import pytest
from lsprotocol.types import (
    DidChangeTextDocumentParams,
    DidCloseTextDocumentParams,
    DidSaveTextDocumentParams,
    HoverParams,
    Position,
    TextDocumentContentChangeWholeDocument,
    TextDocumentIdentifier,
    VersionedTextDocumentIdentifier,
)

from tests.integration.utils import write_cdb, doc
from tests.integration.utils.wait import wait_for_recompile
from tests.integration.utils.assertions import assert_clean_compile, assert_has_errors


# =========================================================================
# Staleness detection tests
# =========================================================================


async def test_header_change_invalidates_ast(client, tmp_path):
    """Modifying a header on disk should cause recompilation on next hover,
    even though didSave was never called (mtime-based detection)."""
    # Setup: main.cpp includes header.h
    (tmp_path / "header.h").write_text("inline int value() { return 1; }\n")
    (tmp_path / "main.cpp").write_text(
        '#include "header.h"\nint main() { return value(); }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    # First compile — should succeed with no diagnostics.
    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert_clean_compile(client, uri)

    # Modify header on disk — introduce an error.
    # Ensure mtime advances past filesystem granularity (1s on some FSes).
    await asyncio.sleep(1.1)
    (tmp_path / "header.h").write_text(
        "inline int value() { return }\n"
    )  # syntax error

    # Send another hover — ensure_compiled should detect mtime change
    # in deps and trigger recompilation. The recompilation publishes
    # fresh diagnostics as a side effect.
    await wait_for_recompile(client, uri)

    # Should now have diagnostics from the broken header.
    assert_has_errors(client, uri, "Expected diagnostics after header change")


async def test_header_change_invalidates_pch(client, tmp_path):
    """Modifying a preamble header on disk should trigger PCH rebuild."""
    (tmp_path / "header.h").write_text("#pragma once\nstruct Foo { int x; };\n")
    (tmp_path / "main.cpp").write_text(
        '#include "header.h"\nint main() { Foo f; return f.x; }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    # First compile — success.
    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert_clean_compile(client, uri)

    # Modify header — rename struct field.
    # Ensure mtime advances past filesystem granularity (1s on some FSes).
    await asyncio.sleep(1.1)
    (tmp_path / "header.h").write_text(
        "#pragma once\nstruct Foo { int y; };\n"  # x -> y
    )

    # Hover again — PCH should rebuild, AST should recompile.
    # main.cpp uses f.x which no longer exists → diagnostics expected.
    await wait_for_recompile(client, uri, timeout=30.0)

    assert_has_errors(client, uri, "Expected error after header field rename")


async def test_no_change_skips_recompile(client, tmp_path):
    """When no dependency has changed, ensure_compiled should fast-path."""
    (tmp_path / "main.cpp").write_text("int main() { return 0; }\n")
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert_clean_compile(client, uri)

    # Second hover — should use cached AST (no recompilation).
    # Verify it returns quickly and doesn't crash.
    hover = await client.text_document_hover_async(
        HoverParams(text_document=doc(uri), position=Position(line=0, character=4))
    )
    # "main" should be hoverable.
    assert hover is not None


async def test_touch_without_content_change_skips_recompile(client, tmp_path):
    """Layer 2: touching a header (mtime changes) without modifying content
    should NOT trigger recompilation — the hash check catches this."""
    (tmp_path / "header.h").write_text("inline int value() { return 1; }\n")
    (tmp_path / "main.cpp").write_text(
        '#include "header.h"\nint main() { return value(); }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert_clean_compile(client, uri)

    # Touch the header — mtime changes but content stays the same.
    await asyncio.sleep(1.1)
    original_content = (tmp_path / "header.h").read_text()
    (tmp_path / "header.h").write_text(original_content)

    # Hover triggers ensure_compiled which runs deps_changed.
    # Layer 2 hash confirms nothing actually changed → cached AST reused.
    # Hover on "main" (line 1, col 4) which should be hoverable.
    hover = await client.text_document_hover_async(
        HoverParams(text_document=doc(uri), position=Position(line=1, character=4))
    )
    assert hover is not None

    # No new diagnostics should appear — the file is still clean.
    assert_clean_compile(client, uri)


async def test_header_replaced_with_different_content(client, tmp_path):
    """Replacing a header file with different content should be detected
    and trigger recompilation reflecting the new content."""
    (tmp_path / "header.h").write_text("inline int value() { return 1; }\n")
    (tmp_path / "main.cpp").write_text(
        '#include "header.h"\nint main() { return value(); }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert_clean_compile(client, uri)

    # Replace header — delete and recreate with a breaking change.
    await asyncio.sleep(1.1)
    (tmp_path / "header.h").unlink()
    (tmp_path / "header.h").write_text("inline int renamed_value() { return 1; }\n")

    # main.cpp still calls value() which no longer exists → error.
    await wait_for_recompile(client, uri)

    assert_has_errors(client, uri, "Expected diagnostics after header replacement")


async def test_fix_error_clears_diagnostics(client, tmp_path):
    """After introducing and fixing an error in a header, diagnostics
    should clear on the next recompilation cycle."""
    (tmp_path / "header.h").write_text("inline int value() { return }\n")  # broken
    (tmp_path / "main.cpp").write_text(
        '#include "header.h"\nint main() { return value(); }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    # First compile — should produce diagnostics.
    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert_has_errors(client, uri, "Expected diagnostics from broken header")

    # Fix the header.
    await asyncio.sleep(1.1)
    (tmp_path / "header.h").write_text("inline int value() { return 1; }\n")

    # Hover triggers recompilation — diagnostics should clear.
    await wait_for_recompile(client, uri)

    assert_clean_compile(client, uri)


async def test_multiple_files_share_header(client, tmp_path):
    """When a shared header changes, all open files that depend on it
    should detect the staleness independently."""
    (tmp_path / "shared.h").write_text("inline int shared() { return 1; }\n")
    (tmp_path / "a.cpp").write_text(
        '#include "shared.h"\nint fa() { return shared(); }\n'
    )
    (tmp_path / "b.cpp").write_text(
        '#include "shared.h"\nint fb() { return shared(); }\n'
    )
    write_cdb(tmp_path, ["a.cpp", "b.cpp"])
    await client.initialize(tmp_path)

    uri_a, _ = await client.open_and_wait(tmp_path / "a.cpp")
    uri_b, _ = await client.open_and_wait(tmp_path / "b.cpp")
    assert_clean_compile(client, uri_a)
    assert_clean_compile(client, uri_b)

    # Break the shared header.
    await asyncio.sleep(1.1)
    (tmp_path / "shared.h").write_text("inline int shared() { return }\n")

    # Both files should get diagnostics after hover.
    await wait_for_recompile(client, uri_a)
    assert_has_errors(client, uri_a, "File A should have diagnostics")

    await wait_for_recompile(client, uri_b)
    assert_has_errors(client, uri_b, "File B should have diagnostics")


async def test_transitive_header_change(client, tmp_path):
    """A change to a transitively included header should be detected."""
    (tmp_path / "base.h").write_text("inline int base() { return 1; }\n")
    (tmp_path / "mid.h").write_text('#include "base.h"\n')
    (tmp_path / "main.cpp").write_text(
        '#include "mid.h"\nint main() { return base(); }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert_clean_compile(client, uri)

    # Modify the transitive dep (base.h).
    await asyncio.sleep(1.1)
    (tmp_path / "base.h").write_text("inline int base() { return }\n")  # broken

    await wait_for_recompile(client, uri)

    assert_has_errors(client, uri, "Expected diagnostics from transitive header change")


# =========================================================================
# didChange / didOpen / didSave / didClose lifecycle tests
# =========================================================================


async def test_didchange_body_edit_recompiles(client, tmp_path):
    """Editing the body (not preamble) via didChange should trigger
    recompilation and update diagnostics."""
    (tmp_path / "main.cpp").write_text("int main() { return 0; }\n")
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert_clean_compile(client, uri)

    # Introduce a body error via didChange.
    event = client.wait_for_diagnostics(uri)
    client.text_document_did_change(
        DidChangeTextDocumentParams(
            text_document=VersionedTextDocumentIdentifier(uri=uri, version=1),
            content_changes=[
                TextDocumentContentChangeWholeDocument(
                    text="int main() { return }\n"  # missing expression
                )
            ],
        )
    )
    await client.text_document_hover_async(
        HoverParams(text_document=doc(uri), position=Position(line=0, character=4))
    )
    await asyncio.wait_for(event.wait(), timeout=30.0)

    assert_has_errors(client, uri, "Expected diagnostics after body error")


async def test_didchange_preamble_edit_recompiles(client, tmp_path):
    """Changing a preamble #include via didChange should trigger PCH rebuild
    and recompilation reflecting the new header's declarations."""
    (tmp_path / "a.h").write_text("#pragma once\ninline int from_a() { return 1; }\n")
    (tmp_path / "b.h").write_text("#pragma once\ninline int from_b() { return 2; }\n")
    (tmp_path / "main.cpp").write_text(
        '#include "a.h"\nint main() { return from_a(); }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert_clean_compile(client, uri)

    # Switch from a.h to b.h and call from_b() instead.
    event = client.wait_for_diagnostics(uri)
    client.text_document_did_change(
        DidChangeTextDocumentParams(
            text_document=VersionedTextDocumentIdentifier(uri=uri, version=1),
            content_changes=[
                TextDocumentContentChangeWholeDocument(
                    text='#include "b.h"\nint main() { return from_b(); }\n'
                )
            ],
        )
    )
    await client.text_document_hover_async(
        HoverParams(text_document=doc(uri), position=Position(line=1, character=4))
    )
    await asyncio.wait_for(event.wait(), timeout=30.0)

    # Should compile cleanly — from_b() is available via b.h.
    assert_clean_compile(client, uri)


async def test_didclose_then_reopen(client, tmp_path):
    """Closing and reopening a file should work correctly — the server
    should not retain stale state from the previous session."""
    (tmp_path / "main.cpp").write_text("int main() { return 0; }\n")
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert_clean_compile(client, uri)

    # Close the file.
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=doc(uri)))

    # Modify on disk while closed.
    await asyncio.sleep(1.1)
    (tmp_path / "main.cpp").write_text("int main() { return }\n")  # broken

    # Reopen — should compile the new (broken) content from disk.
    uri2, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert_has_errors(client, uri2, "Expected diagnostics after reopen with broken content")


async def test_didclose_clears_hover(client, tmp_path):
    """After didClose, hover on the closed file should return None."""
    (tmp_path / "main.cpp").write_text("int main() { return 0; }\n")
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")

    client.text_document_did_close(DidCloseTextDocumentParams(text_document=doc(uri)))

    hover = await client.text_document_hover_async(
        HoverParams(text_document=doc(uri), position=Position(line=0, character=4))
    )
    assert hover is None, "Hover on closed file should return None"


async def test_didsave_triggers_recompile_for_dependents(client, tmp_path):
    """didSave on a header file should mark dependent documents dirty."""
    (tmp_path / "header.h").write_text("inline int value() { return 1; }\n")
    (tmp_path / "main.cpp").write_text(
        '#include "header.h"\nint main() { return value(); }\n'
    )
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")
    assert_clean_compile(client, uri)

    # Modify header on disk and send didSave.
    await asyncio.sleep(1.1)
    (tmp_path / "header.h").write_text("inline int value() { return }\n")  # broken
    client.text_document_did_save(
        DidSaveTextDocumentParams(
            text_document=TextDocumentIdentifier(uri=(tmp_path / "header.h").as_uri())
        )
    )

    # Hover should detect the change and recompile.
    await wait_for_recompile(client, uri)

    assert_has_errors(client, uri, "Expected diagnostics after didSave on broken header")


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
    assert_clean_compile(client, mid_uri)

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
    await wait_for_recompile(client, mid_uri)

    assert_clean_compile(client, mid_uri)
