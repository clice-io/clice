"""Integration tests for C++20 module support through the full LSP server.

These are the Python equivalents of the C++ compile_graph_integration_tests
and module_worker_tests. They test the complete pipeline:
  MasterServer -> CompileGraph -> WorkerPool -> stateless/stateful workers.
"""

import asyncio
import shutil

import pytest
from conftest import generate_cdb
from lsprotocol.types import (
    DidCloseTextDocumentParams,
    DidOpenTextDocumentParams,
    HoverParams,
    Position,
    TextDocumentIdentifier,
    TextDocumentItem,
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


async def _init(client, workspace):
    """Initialize the LSP server with a workspace and wait for CDB scan."""
    result = await client.initialize(workspace)
    # Give the server time to load CDB and scan dependency graph.
    await asyncio.sleep(2.0)
    return result


# ---------------------------------------------------------------------------
# Single module (no dependencies)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/single_module_no_deps")
async def test_single_module_no_deps(client, ws):
    """A single module with no imports should compile without errors."""
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "mod_a.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Chained modules (A -> B, open B)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/chained_modules")
async def test_chained_modules(client, ws):
    """Opening a module that imports another should trigger dependency compilation."""
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "mod_b.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Diamond dependency (Base -> Left/Right -> Top)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/diamond_modules")
async def test_diamond_modules(client, ws):
    """Diamond dependency graph should compile correctly."""
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "top.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Dotted module name (my.io, my.app)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/dotted_module_name")
async def test_dotted_module_name(client, ws):
    """Dotted module names should work correctly."""
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "app.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Module implementation unit (module M; without export)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/module_implementation_unit")
async def test_module_implementation_unit(client, ws):
    """A module implementation unit should compile using the interface PCM."""
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "greeter_impl.cpp")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Consumer file that imports a module (regular .cpp)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/consumer_imports_module")
async def test_consumer_imports_module(client, ws):
    """A regular .cpp file that imports a module should get PCM deps compiled."""
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "main.cpp")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Module partitions (multiple partitions)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/module_partitions")
async def test_module_partitions(client, ws):
    """Module partitions should be compiled in correct order."""
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "lib.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Partition interface (single partition)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/partition_interface")
async def test_partition_interface(client, ws):
    """A single partition interface re-exported from primary should compile."""
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "primary.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Partition chain (partition importing another partition)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/partition_chain")
async def test_partition_chain(client, ws):
    """Partition importing another partition within same module."""
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "sys.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Re-export (export import)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/re_export")
async def test_re_export(client, ws):
    """Re-exported module symbols should be accessible through the wrapper."""
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "user.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Export block syntax
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/export_block")
async def test_export_block(client, ws):
    """Module with export block syntax should compile correctly."""
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "consumer.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Global module fragment
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/global_module_fragment")
async def test_global_module_fragment(client, ws):
    """Module with global module fragment (#include before module decl)."""
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "gmf.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Private module fragment
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/private_module_fragment")
async def test_private_module_fragment(client, ws):
    """Module with private module fragment should compile correctly."""
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "priv.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Export namespace
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/export_namespace")
async def test_export_namespace(client, ws):
    """Module with exported namespace should compile correctly."""
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "calc.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# GMF with include + module import
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/gmf_with_import")
async def test_gmf_with_import(client, ws):
    """Module with GMF (#include) + import should compile correctly."""
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "combined.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Independent modules (no shared deps)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/independent_modules")
async def test_independent_modules(client, ws):
    """Two independent modules should each compile without errors."""
    await _init(client, ws)

    uri_x, _ = await client.open_and_wait(ws / "x.cppm")
    diags_x = client.diagnostics.get(uri_x, [])
    assert len(diags_x) == 0, f"Expected no diagnostics for X, got: {diags_x}"

    uri_y, _ = await client.open_and_wait(ws / "y.cppm")
    diags_y = client.diagnostics.get(uri_y, [])
    assert len(diags_y) == 0, f"Expected no diagnostics for Y, got: {diags_y}"


# ---------------------------------------------------------------------------
# Template export
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/template_export")
async def test_template_export(client, ws):
    """Module with exported templates should compile correctly."""
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "use_tmpl.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Class export and inheritance across modules
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/class_export_and_inheritance")
async def test_class_export_and_inheritance(client, ws):
    """Exported class with cross-module inheritance should compile."""
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "circle.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Save triggers recompilation (close/reopen with new content)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_save_recompile(client, test_data_dir, tmp_path):
    """Closing and reopening a modified module file should recompile without errors."""
    # This test mutates source files at runtime, so copy data to tmp_path.
    src = test_data_dir / "modules" / "save_recompile"
    for f in src.iterdir():
        if f.is_file():
            shutil.copy2(f, tmp_path / f.name)

    generate_cdb(tmp_path)
    await _init(client, tmp_path)

    # Open and compile Mid (which triggers Leaf PCM build).
    mid_uri, _ = await client.open_and_wait(tmp_path / "mid.cppm")
    diags = client.diagnostics.get(mid_uri, [])
    assert len(diags) == 0

    # Open Leaf and wait for its initial compilation.
    leaf_uri, _ = client.open(tmp_path / "leaf.cppm")
    event = client.wait_for_diagnostics(leaf_uri)
    await asyncio.wait_for(event.wait(), timeout=60.0)

    # Close Leaf, modify on disk, and reopen with new content.
    client.text_document_did_close(
        DidCloseTextDocumentParams(text_document=TextDocumentIdentifier(uri=leaf_uri))
    )

    new_content = "export module Leaf;\nexport int leaf() { return 100; }\n"
    (tmp_path / "leaf.cppm").write_text(new_content)

    # Reopen with new content triggers compilation.
    event = client.wait_for_diagnostics(leaf_uri)
    client.text_document_did_open(
        DidOpenTextDocumentParams(
            text_document=TextDocumentItem(
                uri=leaf_uri, language_id="cpp", version=1, text=new_content
            )
        )
    )
    await asyncio.wait_for(event.wait(), timeout=60.0)

    # Should still compile without errors after change.
    diags = client.diagnostics.get(leaf_uri, [])
    assert len(diags) == 0, f"Expected no diagnostics after save, got: {diags}"


# ---------------------------------------------------------------------------
# Compilation failure (undefined symbol in module)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/module_compile_error")
async def test_module_compile_error(client, ws):
    """A module with an error should produce diagnostics."""
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "bad.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) > 0, "Expected diagnostics for undefined symbol"
    # The error should be on line 2 (0-indexed) where UNDEFINED_SYMBOL is used.
    error_diag = diags[0]
    assert error_diag.range.start.line == 2, (
        f"Expected error on line 2, got line {error_diag.range.start.line}"
    )
    # Severity 1 = Error in LSP spec.
    assert error_diag.severity == 1, (
        f"Expected severity Error (1), got {error_diag.severity}"
    )


# ---------------------------------------------------------------------------
# Deep chain (5 modules)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/deep_chain")
async def test_deep_chain(client, ws):
    """A 5-level module chain should compile correctly."""
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "m5.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Partition with GMF (#include inside global module fragment of partition)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/partition_with_gmf")
async def test_partition_with_gmf(client, ws):
    """Partition with GMF (#include) should compile correctly."""
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "cfg.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Cross-module partition + external import
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/partition_with_external_import")
async def test_partition_with_external_import(client, ws):
    """Partition importing an external module should compile correctly."""
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "app.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Hover on imported symbol (feature request after module compilation)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/hover_on_imported_symbol")
async def test_hover_on_imported_symbol(client, ws):
    """Hover on a symbol imported from a module should return info."""
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "use.cpp")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"

    # Hover on 'magic_number' (line 3, character 11 = start of 'magic_number()')
    hover = await client.text_document_hover_async(
        HoverParams(
            text_document=TextDocumentIdentifier(uri=uri),
            position=Position(line=3, character=11),
        )
    )
    assert hover is not None, "Hover on imported symbol should return info"
    assert hover.contents is not None


# ---------------------------------------------------------------------------
# Plain C++ file with no modules (compile_graph == null path)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/no_modules_plain_cpp")
async def test_no_modules_plain_cpp(client, ws):
    """A plain C++ file with no modules should compile normally (no CompileGraph)."""
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "plain.cpp")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Circular module dependency (cycle detection)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/circular_module_dependency")
async def test_circular_module_dependency(client, ws):
    """Circular module imports should not hang the server.

    When modules form a cycle (CycA imports CycB, CycB imports CycA),
    the CompileGraph's cycle detection should prevent deadlock.  The PCM
    builds will fail, so the server may skip the final compilation and
    never publish diagnostics.  The key assertion is that the server
    remains responsive — we verify this by successfully performing a
    subsequent operation (opening a non-cyclic file).
    """
    await _init(client, ws)

    # Open a cyclic file — the server should not hang.
    client.open(ws / "cycle_a.cppm")
    # Give the server time to attempt (and fail) the cyclic PCM builds.
    await asyncio.sleep(5.0)

    # Verify the server is still responsive by opening a non-cyclic file.
    uri_ok, _ = await client.open_and_wait(ws / "ok.cppm")
    diags = client.diagnostics.get(uri_ok, [])
    assert len(diags) == 0, (
        f"Non-cyclic module should compile fine after cycle attempt, got: {diags}"
    )
