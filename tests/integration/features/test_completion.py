"""Integration tests for #include completion and import completion in clice."""

import asyncio

import pytest
from lsprotocol.types import (
    HoverParams,
    Position,
    TextDocumentIdentifier,
)

from tests.integration.utils import doc
from tests.integration.utils.workspace import did_change


# ---------------------------------------------------------------------------
# #include completion
# ---------------------------------------------------------------------------


@pytest.mark.workspace("include_completion")
async def test_include_completion_quoted(client, workspace):
    """Completion after #include " should list local headers."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")

    # Update content to trigger include completion for "my" prefix.
    did_change(client, uri, 1, '#include "my')

    result = await client.completion_at(uri, 0, 12)  # After "my"

    assert result is not None
    items = result.items if hasattr(result, "items") else result
    labels = [item.label for item in items]
    assert "myheader.h" in labels

    client.close(uri)


@pytest.mark.workspace("include_completion")
async def test_include_completion_subdirectory(client, workspace):
    """Completion for #include "subdir/ should list files in subdir."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")

    did_change(client, uri, 1, '#include "subdir/')

    result = await client.completion_at(uri, 0, 17)  # After "subdir/"

    assert result is not None
    items = result.items if hasattr(result, "items") else result
    labels = [item.label for item in items]
    assert "nested.h" in labels

    client.close(uri)


@pytest.mark.workspace("include_completion")
async def test_include_completion_angle_bracket(client, workspace):
    """Completion after #include < should list system headers."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")

    did_change(client, uri, 1, "#include <cstd")

    result = await client.completion_at(uri, 0, 14)  # After "cstd"

    assert result is not None
    items = result.items if hasattr(result, "items") else result
    labels = [item.label for item in items]
    # Should contain at least some standard library headers starting with "cstd".
    cstd_labels = [name for name in labels if name.startswith("cstd")]
    assert len(cstd_labels) > 0, f"Expected cstd* headers, got: {labels}"

    client.close(uri)


@pytest.mark.workspace("include_completion")
async def test_no_include_completion_on_regular_code(client, workspace):
    """Regular code should NOT trigger include completion (goes to worker)."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")

    did_change(client, uri, 1, "int x = ")

    result = await client.completion_at(uri, 0, 8)

    # Should return results from clang (keywords, etc.), not include paths.
    # Verify none of the results look like header filenames.
    assert result is not None
    items = result.items if hasattr(result, "items") else result
    labels = [item.label for item in items]
    assert "myheader.h" not in labels
    assert "nested.h" not in labels

    client.close(uri)


@pytest.mark.workspace("include_completion")
async def test_include_completion_empty_prefix(client, workspace):
    """Completion after #include " with no prefix should list all local headers."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")

    did_change(client, uri, 1, '#include "')

    result = await client.completion_at(uri, 0, 10)  # Right after the quote

    assert result is not None
    items = result.items if hasattr(result, "items") else result
    labels = [item.label for item in items]
    # With empty prefix, should list available headers including myheader.h
    # and the subdir/ directory entry.
    assert "myheader.h" in labels

    client.close(uri)


# ---------------------------------------------------------------------------
# import completion
# ---------------------------------------------------------------------------


@pytest.mark.workspace("modules/chained_modules")
async def test_import_completion_basic(client, workspace):
    """Import completion should list known modules."""
    # First open mod_a to ensure it's scanned and module A is registered.
    await client.open_and_wait(workspace / "mod_a.cppm")

    # Open mod_b and change its content to an incomplete import line.
    uri_b, _ = client.open(workspace / "mod_b.cppm")
    did_change(client, uri_b, 1, "import ")

    result = await client.completion_at(uri_b, 0, 7)

    assert result is not None
    items = result.items if hasattr(result, "items") else result
    labels = [item.label for item in items]
    assert "A" in labels, f"Expected 'A' in completion labels, got: {labels}"


@pytest.mark.workspace("modules/chained_modules")
async def test_import_completion_with_prefix(client, workspace):
    """Import completion with prefix should filter to matching modules."""
    # Open mod_a to register module A.
    await client.open_and_wait(workspace / "mod_a.cppm")

    # Open mod_b and type 'import A' (with prefix).
    uri_b, _ = client.open(workspace / "mod_b.cppm")
    did_change(client, uri_b, 1, "import A")

    result = await client.completion_at(uri_b, 0, 8)

    assert result is not None
    items = result.items if hasattr(result, "items") else result
    labels = [item.label for item in items]
    assert "A" in labels, f"Expected 'A' in completion labels, got: {labels}"


@pytest.mark.workspace("modules/dotted_module_name")
async def test_import_completion_dotted_names(client, workspace):
    """Import completion should return dotted module names like my.app and my.io."""
    # Open both module files to register them.
    await client.open_and_wait(workspace / "io.cppm")
    await client.open_and_wait(workspace / "app.cppm")

    # Change app.cppm to an incomplete import with dotted prefix.
    uri_app, _ = client.open(workspace / "app.cppm")
    did_change(client, uri_app, 1, "import my.")

    result = await client.completion_at(uri_app, 0, 10)

    assert result is not None
    items = result.items if hasattr(result, "items") else result
    labels = [item.label for item in items]
    assert "my.app" in labels or "my.io" in labels, (
        f"Expected dotted module names in completion labels, got: {labels}"
    )


@pytest.mark.workspace("modules/consumer_imports_module")
async def test_buffer_aware_module_deps(client, workspace):
    """Adding import in buffer (unsaved) should still build the needed PCM."""
    # Open the module file first so it gets scanned.
    await client.open_and_wait(workspace / "math.cppm")

    # Open main.cpp with new content that imports Math (simulating unsaved edit).
    uri, _ = client.open(workspace / "main.cpp")
    did_change(client, uri, 1, "import Math;\nint x = add(1, 2);\n")

    # Trigger compilation via hover (pull-based model).
    event = client.wait_for_diagnostics(uri)
    await client.text_document_hover_async(
        HoverParams(
            text_document=TextDocumentIdentifier(uri=uri),
            position=Position(line=0, character=0),
        )
    )

    # Wait for diagnostics.
    await asyncio.wait_for(event.wait(), timeout=60.0)

    diags = client.diagnostics.get(uri, [])
    # Should have no errors if Math PCM was built successfully from buffer scan.
    errors = [d for d in diags if d.severity == 1]
    assert len(errors) == 0, f"Expected no errors, got: {errors}"
