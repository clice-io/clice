"""Integration tests for C++20 module support."""

import asyncio
import shutil

import pytest
from tests.conftest import generate_cdb
from lsprotocol.types import (
    DidCloseTextDocumentParams,
    DidOpenTextDocumentParams,
    HoverParams,
    Position,
    TextDocumentIdentifier,
    TextDocumentItem,
)


@pytest.mark.workspace("modules/single_module_no_deps")
async def test_single_module_no_deps(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "mod_a.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


@pytest.mark.workspace("modules/chained_modules")
async def test_chained_modules(client, workspace):
    """Opening mod_b that imports mod_a should trigger dependency compilation."""
    uri, _ = await client.open_and_wait(workspace / "mod_b.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


@pytest.mark.workspace("modules/diamond_modules")
async def test_diamond_modules(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "top.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


@pytest.mark.workspace("modules/dotted_module_name")
async def test_dotted_module_name(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "app.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


@pytest.mark.workspace("modules/module_implementation_unit")
async def test_module_implementation_unit(client, workspace):
    """Implementation unit (module M; without export) should compile using the interface PCM."""
    uri, _ = await client.open_and_wait(workspace / "greeter_impl.cpp")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


@pytest.mark.workspace("modules/consumer_imports_module")
async def test_consumer_imports_module(client, workspace):
    """A regular .cpp that imports a module should get PCM deps compiled first."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


@pytest.mark.workspace("modules/module_partitions")
async def test_module_partitions(client, workspace):
    """Partitions should be compiled in correct dependency order."""
    uri, _ = await client.open_and_wait(workspace / "lib.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


@pytest.mark.workspace("modules/partition_interface")
async def test_partition_interface(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "primary.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


@pytest.mark.workspace("modules/partition_chain")
async def test_partition_chain(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "sys.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


@pytest.mark.workspace("modules/re_export")
async def test_re_export(client, workspace):
    """Re-exported symbols (export import) should be accessible through the wrapper."""
    uri, _ = await client.open_and_wait(workspace / "user.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


@pytest.mark.workspace("modules/export_block")
async def test_export_block(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "consumer.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


@pytest.mark.workspace("modules/global_module_fragment")
async def test_global_module_fragment(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "gmf.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


@pytest.mark.workspace("modules/private_module_fragment")
async def test_private_module_fragment(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "priv.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


@pytest.mark.workspace("modules/export_namespace")
async def test_export_namespace(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "calc.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


@pytest.mark.workspace("modules/gmf_with_import")
async def test_gmf_with_import(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "combined.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


@pytest.mark.workspace("modules/independent_modules")
async def test_independent_modules(client, workspace):
    uri_x, _ = await client.open_and_wait(workspace / "x.cppm")
    diags_x = client.diagnostics.get(uri_x, [])
    assert len(diags_x) == 0, f"Expected no diagnostics for X, got: {diags_x}"

    uri_y, _ = await client.open_and_wait(workspace / "y.cppm")
    diags_y = client.diagnostics.get(uri_y, [])
    assert len(diags_y) == 0, f"Expected no diagnostics for Y, got: {diags_y}"


@pytest.mark.workspace("modules/template_export")
async def test_template_export(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "use_tmpl.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


@pytest.mark.workspace("modules/class_export_and_inheritance")
async def test_class_export_and_inheritance(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "circle.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


async def test_save_recompile(client, test_data_dir, tmp_path):
    """Closing and reopening a modified module file should recompile without errors."""
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

    event = client.wait_for_diagnostics(leaf_uri)
    client.text_document_did_open(
        DidOpenTextDocumentParams(
            text_document=TextDocumentItem(
                uri=leaf_uri, language_id="cpp", version=1, text=new_content
            )
        )
    )
    await asyncio.wait_for(event.wait(), timeout=60.0)

    diags = client.diagnostics.get(leaf_uri, [])
    assert len(diags) == 0, f"Expected no diagnostics after save, got: {diags}"


@pytest.mark.workspace("modules/module_compile_error")
async def test_module_compile_error(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "bad.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) > 0, "Expected diagnostics for undefined symbol"
    error_diag = diags[0]
    assert error_diag.range.start.line == 2, (
        f"Expected error on line 2, got line {error_diag.range.start.line}"
    )
    assert error_diag.severity == 1, (
        f"Expected severity Error (1), got {error_diag.severity}"
    )


@pytest.mark.workspace("modules/deep_chain")
async def test_deep_chain(client, workspace):
    """A 5-level module chain (m1->m2->...->m5) should compile correctly."""
    uri, _ = await client.open_and_wait(workspace / "m5.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


@pytest.mark.workspace("modules/partition_with_gmf")
async def test_partition_with_gmf(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "cfg.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


@pytest.mark.workspace("modules/partition_with_external_import")
async def test_partition_with_external_import(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "app.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


@pytest.mark.workspace("modules/hover_on_imported_symbol")
async def test_hover_on_imported_symbol(client, workspace):
    """Hover on a symbol imported from a module should return type info."""
    uri, _ = await client.open_and_wait(workspace / "use.cpp")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"

    hover = await client.text_document_hover_async(
        HoverParams(
            text_document=TextDocumentIdentifier(uri=uri),
            position=Position(line=3, character=11),
        )
    )
    assert hover is not None, "Hover on imported symbol should return info"
    assert hover.contents is not None


@pytest.mark.workspace("modules/no_modules_plain_cpp")
async def test_no_modules_plain_cpp(client, workspace):
    """Plain .cpp with no modules should compile normally (CompileGraph null path)."""
    uri, _ = await client.open_and_wait(workspace / "plain.cpp")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


@pytest.mark.workspace("modules/circular_module_dependency")
async def test_circular_module_dependency(client, workspace):
    """Circular module imports should not hang the server.

    The CompileGraph's cycle detection should prevent deadlock. We verify
    the server remains responsive by opening a non-cyclic file afterwards.
    """
    client.open(workspace / "cycle_a.cppm")
    await asyncio.sleep(5.0)

    uri_ok, _ = await client.open_and_wait(workspace / "ok.cppm")
    diags = client.diagnostics.get(uri_ok, [])
    assert len(diags) == 0, (
        f"Non-cyclic module should compile fine after cycle attempt, got: {diags}"
    )
