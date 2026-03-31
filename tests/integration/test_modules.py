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
# Single module (no dependencies)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/single_module_no_deps")
async def test_single_module_no_deps(client, workspace):
    """A single module with no imports should compile without errors."""
    uri, _ = await client.open_and_wait(workspace / "mod_a.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Chained modules (A -> B, open B)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/chained_modules")
async def test_chained_modules(client: CliceClient, workspace):
    """Opening a module that imports another should trigger dependency compilation."""
    uri, _ = await client.open_and_wait(workspace / "mod_b.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Diamond dependency (Base -> Left/Right -> Top)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/diamond_modules")
async def test_diamond_modules(client, workspace):
    """Diamond dependency graph should compile correctly."""
    uri, _ = await client.open_and_wait(workspace / "top.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Dotted module name (my.io, my.app)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/dotted_module_name")
async def test_dotted_module_name(client, workspace):
    """Dotted module names should work correctly."""
    uri, _ = await client.open_and_wait(workspace / "app.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Module implementation unit (module M; without export)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/module_implementation_unit")
async def test_module_implementation_unit(client, workspace):
    """A module implementation unit should compile using the interface PCM."""
    uri, _ = await client.open_and_wait(workspace / "greeter_impl.cpp")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Consumer file that imports a module (regular .cpp)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/consumer_imports_module")
async def test_consumer_imports_module(client, workspace):
    """A regular .cpp file that imports a module should get PCM deps compiled."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Module partitions (multiple partitions)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/module_partitions")
async def test_module_partitions(client, workspace):
    """Module partitions should be compiled in correct order."""
    uri, _ = await client.open_and_wait(workspace / "lib.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Partition interface (single partition)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/partition_interface")
async def test_partition_interface(client, workspace):
    """A single partition interface re-exported from primary should compile."""
    uri, _ = await client.open_and_wait(workspace / "primary.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Partition chain (partition importing another partition)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/partition_chain")
async def test_partition_chain(client, workspace):
    """Partition importing another partition within same module."""
    uri, _ = await client.open_and_wait(workspace / "sys.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Re-export (export import)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/re_export")
async def test_re_export(client, workspace):
    """Re-exported module symbols should be accessible through the wrapper."""
    uri, _ = await client.open_and_wait(workspace / "user.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Export block syntax
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/export_block")
async def test_export_block(client, workspace):
    """Module with export block syntax should compile correctly."""
    uri, _ = await client.open_and_wait(workspace / "consumer.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Global module fragment
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/global_module_fragment")
async def test_global_module_fragment(client, workspace):
    """Module with global module fragment (#include before module decl)."""
    uri, _ = await client.open_and_wait(workspace / "gmf.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Private module fragment
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/private_module_fragment")
async def test_private_module_fragment(client, workspace):
    """Module with private module fragment should compile correctly."""
    uri, _ = await client.open_and_wait(workspace / "priv.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Export namespace
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/export_namespace")
async def test_export_namespace(client, workspace):
    """Module with exported namespace should compile correctly."""
    uri, _ = await client.open_and_wait(workspace / "calc.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# GMF with include + module import
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/gmf_with_import")
async def test_gmf_with_import(client, workspace):
    """Module with GMF (#include) + import should compile correctly."""
    uri, _ = await client.open_and_wait(workspace / "combined.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Independent modules (no shared deps)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/independent_modules")
async def test_independent_modules(client, workspace):
    """Two independent modules should each compile without errors."""
    uri_x, _ = await client.open_and_wait(workspace / "x.cppm")
    diags_x = client.diagnostics.get(uri_x, [])
    assert len(diags_x) == 0, f"Expected no diagnostics for X, got: {diags_x}"

    uri_y, _ = await client.open_and_wait(workspace / "y.cppm")
    diags_y = client.diagnostics.get(uri_y, [])
    assert len(diags_y) == 0, f"Expected no diagnostics for Y, got: {diags_y}"


# ---------------------------------------------------------------------------
# Template export
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/template_export")
async def test_template_export(client, workspace):
    """Module with exported templates should compile correctly."""
    uri, _ = await client.open_and_wait(workspace / "use_tmpl.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Class export and inheritance across modules
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/class_export_and_inheritance")
async def test_class_export_and_inheritance(client, workspace):
    """Exported class with cross-module inheritance should compile."""
    uri, _ = await client.open_and_wait(workspace / "circle.cppm")
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
    await client.initialize(tmp_path)

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
async def test_module_compile_error(client, workspace):
    """A module with an error should produce diagnostics."""
    uri, _ = await client.open_and_wait(workspace / "bad.cppm")
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
async def test_deep_chain(client, workspace):
    """A 5-level module chain should compile correctly."""
    uri, _ = await client.open_and_wait(workspace / "m5.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Partition with GMF (#include inside global module fragment of partition)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/partition_with_gmf")
async def test_partition_with_gmf(client, workspace):
    """Partition with GMF (#include) should compile correctly."""
    uri, _ = await client.open_and_wait(workspace / "cfg.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Cross-module partition + external import
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/partition_with_external_import")
async def test_partition_with_external_import(client, workspace):
    """Partition importing an external module should compile correctly."""
    uri, _ = await client.open_and_wait(workspace / "app.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Hover on imported symbol (feature request after module compilation)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/hover_on_imported_symbol")
async def test_hover_on_imported_symbol(client, workspace):
    """Hover on a symbol imported from a module should return info."""
    uri, _ = await client.open_and_wait(workspace / "use.cpp")
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
async def test_no_modules_plain_cpp(client, workspace):
    """A plain C++ file with no modules should compile normally (no CompileGraph)."""
    uri, _ = await client.open_and_wait(workspace / "plain.cpp")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Circular module dependency (cycle detection)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("modules/circular_module_dependency")
async def test_circular_module_dependency(client, workspace):
    """Circular module imports should not hang the server.

    When modules form a cycle (CycA imports CycB, CycB imports CycA),
    the CompileGraph's cycle detection should prevent deadlock.  The PCM
    builds will fail, so the server may skip the final compilation and
    never publish diagnostics.  The key assertion is that the server
    remains responsive — we verify this by successfully performing a
    subsequent operation (opening a non-cyclic file).
    """
    # Open a cyclic file — the server should not hang.
    client.open(workspace / "cycle_a.cppm")
    # Give the server time to attempt (and fail) the cyclic PCM builds.
    await asyncio.sleep(5.0)

    # Verify the server is still responsive by opening a non-cyclic file.
    uri_ok, _ = await client.open_and_wait(workspace / "ok.cppm")
    diags = client.diagnostics.get(uri_ok, [])
    assert len(diags) == 0, (
        f"Non-cyclic module should compile fine after cycle attempt, got: {diags}"
    )
