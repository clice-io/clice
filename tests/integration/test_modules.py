"""Integration tests for C++20 module support through the full LSP server.

These are the Python equivalents of the C++ compile_graph_integration_tests
and module_worker_tests. They test the complete pipeline:
  MasterServer -> CompileGraph -> WorkerPool -> stateless/stateful workers.
"""

import asyncio
import shutil
import subprocess
from pathlib import Path

import pytest
from lsprotocol.types import (
    DidCloseTextDocumentParams,
    DidOpenTextDocumentParams,
    HoverParams,
    Position,
    TextDocumentIdentifier,
    TextDocumentItem,
)

# Directory containing pre-written module source files for each test case.
_DATA_DIR = Path(__file__).resolve().parent.parent / "data" / "modules"


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _generate_cdb(workspace: Path):
    """Generate compile_commands.json using CMake with Ninja backend."""
    cmake = shutil.which("cmake")
    if cmake is None:
        raise RuntimeError("cmake executable not found in PATH")
    build_dir = workspace / "build"
    subprocess.run(
        [
            cmake,
            "-G",
            "Ninja",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            "-S",
            str(workspace),
            "-B",
            str(build_dir),
        ],
        check=True,
        capture_output=True,
        text=True,
        timeout=120,
    )
    shutil.copy2(
        build_dir / "compile_commands.json", workspace / "compile_commands.json"
    )


async def _init(client, workspace: Path):
    """Initialize the LSP server with a workspace and wait for CDB scan."""
    result = await client.initialize(workspace)
    # Give the server time to load CDB and scan dependency graph.
    await asyncio.sleep(2.0)
    return result


# ---------------------------------------------------------------------------
# Single module (no dependencies)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_single_module_no_deps(client):
    """A single module with no imports should compile without errors."""
    ws = _DATA_DIR / "single_module_no_deps"
    _generate_cdb(ws)
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "mod_a.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Chained modules (A -> B, open B)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_chained_modules(client):
    """Opening a module that imports another should trigger dependency compilation."""
    ws = _DATA_DIR / "chained_modules"
    _generate_cdb(ws)
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "mod_b.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Diamond dependency (Base -> Left/Right -> Top)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_diamond_modules(client):
    """Diamond dependency graph should compile correctly."""
    ws = _DATA_DIR / "diamond_modules"
    _generate_cdb(ws)
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "top.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Dotted module name (my.io, my.app)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_dotted_module_name(client):
    """Dotted module names should work correctly."""
    ws = _DATA_DIR / "dotted_module_name"
    _generate_cdb(ws)
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "app.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Module implementation unit (module M; without export)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_module_implementation_unit(client):
    """A module implementation unit should compile using the interface PCM."""
    ws = _DATA_DIR / "module_implementation_unit"
    _generate_cdb(ws)
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "greeter_impl.cpp")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Consumer file that imports a module (regular .cpp)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_consumer_imports_module(client):
    """A regular .cpp file that imports a module should get PCM deps compiled."""
    ws = _DATA_DIR / "consumer_imports_module"
    _generate_cdb(ws)
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "main.cpp")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Module partitions (multiple partitions)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_module_partitions(client):
    """Module partitions should be compiled in correct order."""
    ws = _DATA_DIR / "module_partitions"
    _generate_cdb(ws)
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "lib.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Partition interface (single partition)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_partition_interface(client):
    """A single partition interface re-exported from primary should compile."""
    ws = _DATA_DIR / "partition_interface"
    _generate_cdb(ws)
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "primary.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Partition chain (partition importing another partition)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_partition_chain(client):
    """Partition importing another partition within same module."""
    ws = _DATA_DIR / "partition_chain"
    _generate_cdb(ws)
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "sys.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Re-export (export import)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_re_export(client):
    """Re-exported module symbols should be accessible through the wrapper."""
    ws = _DATA_DIR / "re_export"
    _generate_cdb(ws)
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "user.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Export block syntax
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_export_block(client):
    """Module with export block syntax should compile correctly."""
    ws = _DATA_DIR / "export_block"
    _generate_cdb(ws)
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "consumer.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Global module fragment
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_global_module_fragment(client):
    """Module with global module fragment (#include before module decl)."""
    ws = _DATA_DIR / "global_module_fragment"
    _generate_cdb(ws)
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "gmf.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Private module fragment
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_private_module_fragment(client):
    """Module with private module fragment should compile correctly."""
    ws = _DATA_DIR / "private_module_fragment"
    _generate_cdb(ws)
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "priv.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Export namespace
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_export_namespace(client):
    """Module with exported namespace should compile correctly."""
    ws = _DATA_DIR / "export_namespace"
    _generate_cdb(ws)
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "calc.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# GMF with include + module import
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_gmf_with_import(client):
    """Module with GMF (#include) + import should compile correctly."""
    ws = _DATA_DIR / "gmf_with_import"
    _generate_cdb(ws)
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "combined.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Independent modules (no shared deps)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_independent_modules(client):
    """Two independent modules should each compile without errors."""
    ws = _DATA_DIR / "independent_modules"
    _generate_cdb(ws)
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
async def test_template_export(client):
    """Module with exported templates should compile correctly."""
    ws = _DATA_DIR / "template_export"
    _generate_cdb(ws)
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "use_tmpl.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Class export and inheritance across modules
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_class_export_and_inheritance(client):
    """Exported class with cross-module inheritance should compile."""
    ws = _DATA_DIR / "class_export_and_inheritance"
    _generate_cdb(ws)
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "circle.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Save triggers recompilation (close/reopen with new content)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_save_recompile(client, tmp_path):
    """Closing and reopening a modified module file should recompile without errors."""
    # This test mutates source files at runtime, so copy data to tmp_path.
    src = _DATA_DIR / "save_recompile"
    for f in src.iterdir():
        if f.is_file():
            shutil.copy2(f, tmp_path / f.name)

    _generate_cdb(tmp_path)
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
async def test_module_compile_error(client):
    """A module with an error should produce diagnostics."""
    ws = _DATA_DIR / "module_compile_error"
    _generate_cdb(ws)
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
async def test_deep_chain(client):
    """A 5-level module chain should compile correctly."""
    ws = _DATA_DIR / "deep_chain"
    _generate_cdb(ws)
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "m5.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Partition with GMF (#include inside global module fragment of partition)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_partition_with_gmf(client):
    """Partition with GMF (#include) should compile correctly."""
    ws = _DATA_DIR / "partition_with_gmf"
    _generate_cdb(ws)
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "cfg.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Cross-module partition + external import
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_partition_with_external_import(client):
    """Partition importing an external module should compile correctly."""
    ws = _DATA_DIR / "partition_with_external_import"
    _generate_cdb(ws)
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "app.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Hover on imported symbol (feature request after module compilation)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_hover_on_imported_symbol(client):
    """Hover on a symbol imported from a module should return info."""
    ws = _DATA_DIR / "hover_on_imported_symbol"
    _generate_cdb(ws)
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
async def test_no_modules_plain_cpp(client):
    """A plain C++ file with no modules should compile normally (no CompileGraph)."""
    ws = _DATA_DIR / "no_modules_plain_cpp"
    _generate_cdb(ws)
    await _init(client, ws)

    uri, _ = await client.open_and_wait(ws / "plain.cpp")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Circular module dependency (cycle detection)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_circular_module_dependency(client):
    """Circular module imports should not hang the server.

    When modules form a cycle (CycA imports CycB, CycB imports CycA),
    the CompileGraph's cycle detection should prevent deadlock.  The PCM
    builds will fail, so the server may skip the final compilation and
    never publish diagnostics.  The key assertion is that the server
    remains responsive — we verify this by successfully performing a
    subsequent operation (opening a non-cyclic file).
    """
    ws = _DATA_DIR / "circular_module_dependency"
    _generate_cdb(ws)
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
