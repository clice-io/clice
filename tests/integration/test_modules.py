"""Integration tests for C++20 module support through the full LSP server.

These are the Python equivalents of the C++ compile_graph_integration_tests
and module_worker_tests. They test the complete pipeline:
  MasterServer -> CompileGraph -> WorkerPool -> stateless/stateful workers.
"""

import json
import asyncio
from pathlib import Path

import pytest
from lsprotocol.types import (
    ClientCapabilities,
    DidCloseTextDocumentParams,
    DidOpenTextDocumentParams,
    DidSaveTextDocumentParams,
    HoverParams,
    InitializeParams,
    InitializedParams,
    Position,
    TextDocumentIdentifier,
    TextDocumentItem,
    WorkspaceFolder,
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _write_cdb(workspace: Path, files: list[str], extra_args: list[str] | None = None):
    """Generate compile_commands.json for the given source files."""
    cdb = []
    for f in files:
        args = ["clang++", "-std=c++20", "-fsyntax-only"]
        if extra_args:
            args.extend(extra_args)
        args.append(str(workspace / f))
        cdb.append(
            {
                "directory": str(workspace),
                "file": str(workspace / f),
                "arguments": args,
            }
        )
    (workspace / "compile_commands.json").write_text(json.dumps(cdb, indent=2))


def _write_cdb_entries(workspace: Path, entries: list[tuple[str, list[str]]]):
    """Generate compile_commands.json with per-file extra args.

    entries: list of (filename, extra_args) tuples.
    """
    cdb = []
    for filename, extra in entries:
        args = ["clang++", "-std=c++20", "-fsyntax-only"]
        args.extend(extra)
        args.append(str(workspace / filename))
        cdb.append(
            {
                "directory": str(workspace),
                "file": str(workspace / filename),
                "arguments": args,
            }
        )
    (workspace / "compile_commands.json").write_text(json.dumps(cdb, indent=2))


async def _init(client, workspace: Path):
    """Initialize the LSP server with a workspace."""
    result = await client.initialize_async(
        InitializeParams(
            capabilities=ClientCapabilities(),
            root_uri=workspace.as_uri(),
            workspace_folders=[WorkspaceFolder(uri=workspace.as_uri(), name="test")],
        )
    )
    client.initialized(InitializedParams())
    # Give the server time to load CDB and scan dependency graph.
    await asyncio.sleep(1.0)
    return result


def _open(client, workspace: Path, filename: str, version: int = 0):
    """Open a file and return its URI."""
    path = workspace / filename
    content = path.read_text(encoding="utf-8")
    uri = path.as_uri()
    client.text_document_did_open(
        DidOpenTextDocumentParams(
            text_document=TextDocumentItem(
                uri=uri, language_id="cpp", version=version, text=content
            )
        )
    )
    return uri, content


async def _open_and_wait(client, workspace: Path, filename: str, timeout: float = 60.0):
    """Open a file and wait for compilation diagnostics."""
    uri, content = _open(client, workspace, filename)
    event = client.wait_for_diagnostics(uri)
    await asyncio.wait_for(event.wait(), timeout=timeout)
    return uri, content


# ---------------------------------------------------------------------------
# Single module (no dependencies)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_single_module_no_deps(client, tmp_path):
    """A single module with no imports should compile without errors."""
    (tmp_path / "mod_a.cppm").write_text(
        "export module A;\nexport int foo() { return 42; }\n"
    )
    _write_cdb(tmp_path, ["mod_a.cppm"])
    await _init(client, tmp_path)

    uri, _ = await _open_and_wait(client, tmp_path, "mod_a.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Chained modules (A -> B, open B)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_chained_modules(client, tmp_path):
    """Opening a module that imports another should trigger dependency compilation."""
    (tmp_path / "mod_a.cppm").write_text(
        "export module A;\nexport int foo() { return 42; }\n"
    )
    (tmp_path / "mod_b.cppm").write_text(
        "export module B;\nimport A;\nexport int bar() { return foo() + 1; }\n"
    )
    _write_cdb(tmp_path, ["mod_a.cppm", "mod_b.cppm"])
    await _init(client, tmp_path)

    uri, _ = await _open_and_wait(client, tmp_path, "mod_b.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Diamond dependency (Base -> Left/Right -> Top)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_diamond_modules(client, tmp_path):
    """Diamond dependency graph should compile correctly."""
    (tmp_path / "base.cppm").write_text(
        "export module Base;\nexport int base_val() { return 10; }\n"
    )
    (tmp_path / "left.cppm").write_text(
        "export module Left;\n"
        "import Base;\n"
        "export int left_val() { return base_val() + 1; }\n"
    )
    (tmp_path / "right.cppm").write_text(
        "export module Right;\n"
        "import Base;\n"
        "export int right_val() { return base_val() + 2; }\n"
    )
    (tmp_path / "top.cppm").write_text(
        "export module Top;\n"
        "import Left;\n"
        "import Right;\n"
        "export int top_val() { return left_val() + right_val(); }\n"
    )
    _write_cdb(tmp_path, ["base.cppm", "left.cppm", "right.cppm", "top.cppm"])
    await _init(client, tmp_path)

    uri, _ = await _open_and_wait(client, tmp_path, "top.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Dotted module name (my.io, my.app)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_dotted_module_name(client, tmp_path):
    """Dotted module names should work correctly."""
    (tmp_path / "io.cppm").write_text("export module my.io;\nexport void print() {}\n")
    (tmp_path / "app.cppm").write_text(
        "export module my.app;\nimport my.io;\nexport void run() { print(); }\n"
    )
    _write_cdb(tmp_path, ["io.cppm", "app.cppm"])
    await _init(client, tmp_path)

    uri, _ = await _open_and_wait(client, tmp_path, "app.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Module implementation unit (module M; without export)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_module_implementation_unit(client, tmp_path):
    """A module implementation unit should compile using the interface PCM."""
    (tmp_path / "greeter.cppm").write_text(
        "export module Greeter;\nexport const char* greet();\n"
    )
    (tmp_path / "greeter_impl.cpp").write_text(
        'module Greeter;\nconst char* greet() { return "hello"; }\n'
    )
    _write_cdb(tmp_path, ["greeter.cppm", "greeter_impl.cpp"])
    await _init(client, tmp_path)

    uri, _ = await _open_and_wait(client, tmp_path, "greeter_impl.cpp")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Consumer file that imports a module (regular .cpp)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_consumer_imports_module(client, tmp_path):
    """A regular .cpp file that imports a module should get PCM deps compiled."""
    (tmp_path / "math.cppm").write_text(
        "export module Math;\nexport int add(int a, int b) { return a + b; }\n"
    )
    (tmp_path / "main.cpp").write_text(
        "import Math;\nint main() { return add(1, 2); }\n"
    )
    _write_cdb(tmp_path, ["math.cppm", "main.cpp"])
    await _init(client, tmp_path)

    uri, _ = await _open_and_wait(client, tmp_path, "main.cpp")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Module partitions (multiple partitions)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_module_partitions(client, tmp_path):
    """Module partitions should be compiled in correct order."""
    (tmp_path / "part_a.cppm").write_text(
        "export module Lib:A;\nexport int a_fn() { return 1; }\n"
    )
    (tmp_path / "part_b.cppm").write_text(
        "export module Lib:B;\nexport int b_fn() { return 2; }\n"
    )
    (tmp_path / "lib.cppm").write_text(
        "export module Lib;\n"
        "export import :A;\n"
        "export import :B;\n"
        "export int lib_fn() { return a_fn() + b_fn(); }\n"
    )
    _write_cdb(tmp_path, ["part_a.cppm", "part_b.cppm", "lib.cppm"])
    await _init(client, tmp_path)

    uri, _ = await _open_and_wait(client, tmp_path, "lib.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Partition interface (single partition)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_partition_interface(client, tmp_path):
    """A single partition interface re-exported from primary should compile."""
    (tmp_path / "part.cppm").write_text(
        "export module M:Part;\nexport int part_fn() { return 5; }\n"
    )
    (tmp_path / "primary.cppm").write_text(
        "export module M;\n"
        "export import :Part;\n"
        "export int primary_fn() { return part_fn() + 1; }\n"
    )
    _write_cdb(tmp_path, ["part.cppm", "primary.cppm"])
    await _init(client, tmp_path)

    uri, _ = await _open_and_wait(client, tmp_path, "primary.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Partition chain (partition importing another partition)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_partition_chain(client, tmp_path):
    """Partition importing another partition within same module."""
    (tmp_path / "types.cppm").write_text(
        "export module Sys:Types;\nexport struct Config { int value = 0; };\n"
    )
    (tmp_path / "core.cppm").write_text(
        "export module Sys:Core;\n"
        "import :Types;\n"
        "export Config make_config() { return {42}; }\n"
    )
    (tmp_path / "sys.cppm").write_text(
        "export module Sys;\nexport import :Types;\nexport import :Core;\n"
    )
    _write_cdb(tmp_path, ["types.cppm", "core.cppm", "sys.cppm"])
    await _init(client, tmp_path)

    uri, _ = await _open_and_wait(client, tmp_path, "sys.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Re-export (export import)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_re_export(client, tmp_path):
    """Re-exported module symbols should be accessible through the wrapper."""
    (tmp_path / "core.cppm").write_text(
        "export module Core;\nexport int core_fn() { return 1; }\n"
    )
    (tmp_path / "wrapper.cppm").write_text(
        "export module Wrapper;\n"
        "export import Core;\n"
        "export int wrap_fn() { return core_fn() + 10; }\n"
    )
    (tmp_path / "user.cppm").write_text(
        "export module User;\n"
        "import Wrapper;\n"
        "export int use_fn() { return core_fn() + wrap_fn(); }\n"
    )
    _write_cdb(tmp_path, ["core.cppm", "wrapper.cppm", "user.cppm"])
    await _init(client, tmp_path)

    uri, _ = await _open_and_wait(client, tmp_path, "user.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Export block syntax
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_export_block(client, tmp_path):
    """Module with export block syntax should compile correctly."""
    (tmp_path / "block.cppm").write_text(
        "export module Block;\n"
        "export {\n"
        "    int alpha() { return 1; }\n"
        "    int beta() { return 2; }\n"
        "    namespace ns {\n"
        "        int gamma() { return 3; }\n"
        "    }\n"
        "}\n"
    )
    (tmp_path / "consumer.cppm").write_text(
        "export module Consumer;\n"
        "import Block;\n"
        "export int total() { return alpha() + beta() + ns::gamma(); }\n"
    )
    _write_cdb(tmp_path, ["block.cppm", "consumer.cppm"])
    await _init(client, tmp_path)

    uri, _ = await _open_and_wait(client, tmp_path, "consumer.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Global module fragment
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_global_module_fragment(client, tmp_path):
    """Module with global module fragment (#include before module decl)."""
    (tmp_path / "legacy.h").write_text("inline int legacy_fn() { return 99; }\n")
    (tmp_path / "gmf.cppm").write_text(
        "module;\n"
        '#include "legacy.h"\n'
        "export module GMF;\n"
        "export int wrapped() { return legacy_fn(); }\n"
    )
    _write_cdb(tmp_path, ["gmf.cppm"], extra_args=["-I", str(tmp_path)])
    await _init(client, tmp_path)

    uri, _ = await _open_and_wait(client, tmp_path, "gmf.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Private module fragment
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_private_module_fragment(client, tmp_path):
    """Module with private module fragment should compile correctly."""
    (tmp_path / "priv.cppm").write_text(
        "export module Priv;\n"
        "export int public_fn();\n"
        "module : private;\n"
        "int public_fn() { return 42; }\n"
        "int private_helper() { return 7; }\n"
    )
    _write_cdb(tmp_path, ["priv.cppm"])
    await _init(client, tmp_path)

    uri, _ = await _open_and_wait(client, tmp_path, "priv.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Export namespace
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_export_namespace(client, tmp_path):
    """Module with exported namespace should compile correctly."""
    (tmp_path / "ns.cppm").write_text(
        "export module NS;\n"
        "export namespace math {\n"
        "    int add(int a, int b) { return a + b; }\n"
        "    int mul(int a, int b) { return a * b; }\n"
        "}\n"
    )
    (tmp_path / "calc.cppm").write_text(
        "export module Calc;\n"
        "import NS;\n"
        "export int compute() { return math::add(3, math::mul(4, 5)); }\n"
    )
    _write_cdb(tmp_path, ["ns.cppm", "calc.cppm"])
    await _init(client, tmp_path)

    uri, _ = await _open_and_wait(client, tmp_path, "calc.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# GMF with include + module import
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_gmf_with_import(client, tmp_path):
    """Module with GMF (#include) + import should compile correctly."""
    (tmp_path / "util.h").write_text("inline int util_helper() { return 7; }\n")
    (tmp_path / "base.cppm").write_text(
        "export module Base;\nexport int base() { return 100; }\n"
    )
    (tmp_path / "combined.cppm").write_text(
        "module;\n"
        '#include "util.h"\n'
        "export module Combined;\n"
        "import Base;\n"
        "export int combined() { return base() + util_helper(); }\n"
    )
    _write_cdb_entries(
        tmp_path,
        [
            ("base.cppm", []),
            ("combined.cppm", ["-I", str(tmp_path)]),
        ],
    )
    await _init(client, tmp_path)

    uri, _ = await _open_and_wait(client, tmp_path, "combined.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Independent modules (no shared deps)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_independent_modules(client, tmp_path):
    """Two independent modules should each compile without errors."""
    (tmp_path / "x.cppm").write_text("export module X;\nexport int x() { return 1; }\n")
    (tmp_path / "y.cppm").write_text("export module Y;\nexport int y() { return 2; }\n")
    _write_cdb(tmp_path, ["x.cppm", "y.cppm"])
    await _init(client, tmp_path)

    uri_x, _ = await _open_and_wait(client, tmp_path, "x.cppm")
    diags_x = client.diagnostics.get(uri_x, [])
    assert len(diags_x) == 0, f"Expected no diagnostics for X, got: {diags_x}"

    uri_y, _ = await _open_and_wait(client, tmp_path, "y.cppm")
    diags_y = client.diagnostics.get(uri_y, [])
    assert len(diags_y) == 0, f"Expected no diagnostics for Y, got: {diags_y}"


# ---------------------------------------------------------------------------
# Template export
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_template_export(client, tmp_path):
    """Module with exported templates should compile correctly."""
    (tmp_path / "tmpl.cppm").write_text(
        "export module Tmpl;\n"
        "export template<typename T>\n"
        "T identity(T x) { return x; }\n"
        "export template<typename T, typename U>\n"
        "auto pair_sum(T a, U b) { return a + b; }\n"
    )
    (tmp_path / "use_tmpl.cppm").write_text(
        "export module UseTmpl;\n"
        "import Tmpl;\n"
        "export int test() { return identity(42) + pair_sum(1, 2); }\n"
    )
    _write_cdb(tmp_path, ["tmpl.cppm", "use_tmpl.cppm"])
    await _init(client, tmp_path)

    uri, _ = await _open_and_wait(client, tmp_path, "use_tmpl.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Class export and inheritance across modules
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_class_export_and_inheritance(client, tmp_path):
    """Exported class with cross-module inheritance should compile."""
    (tmp_path / "shape.cppm").write_text(
        "export module Shape;\n"
        "export class Shape {\n"
        "public:\n"
        "    virtual ~Shape() = default;\n"
        "    virtual int area() const = 0;\n"
        "};\n"
    )
    (tmp_path / "circle.cppm").write_text(
        "export module Circle;\n"
        "import Shape;\n"
        "export class Circle : public Shape {\n"
        "    int r;\n"
        "public:\n"
        "    Circle(int r) : r(r) {}\n"
        "    int area() const override { return 3 * r * r; }\n"
        "};\n"
    )
    _write_cdb(tmp_path, ["shape.cppm", "circle.cppm"])
    await _init(client, tmp_path)

    uri, _ = await _open_and_wait(client, tmp_path, "circle.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Save triggers recompilation (close/reopen with new content)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_save_recompile(client, tmp_path):
    """Closing and reopening a modified module file should recompile without errors."""
    (tmp_path / "leaf.cppm").write_text(
        "export module Leaf;\nexport int leaf() { return 1; }\n"
    )
    (tmp_path / "mid.cppm").write_text(
        "export module Mid;\nimport Leaf;\nexport int mid() { return leaf() + 1; }\n"
    )
    _write_cdb(tmp_path, ["leaf.cppm", "mid.cppm"])
    await _init(client, tmp_path)

    # Open and compile Mid (which triggers Leaf PCM build).
    mid_uri, _ = await _open_and_wait(client, tmp_path, "mid.cppm")
    diags = client.diagnostics.get(mid_uri, [])
    assert len(diags) == 0

    # Open Leaf and wait for its initial compilation.
    leaf_uri, _ = _open(client, tmp_path, "leaf.cppm")
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
async def test_module_compile_error(client, tmp_path):
    """A module with an error should produce diagnostics."""
    (tmp_path / "good.cppm").write_text(
        "export module Good;\nexport int good() { return 1; }\n"
    )
    (tmp_path / "bad.cppm").write_text(
        "export module Bad;\n"
        "import Good;\n"
        "export int bad() { return UNDEFINED_SYMBOL; }\n"
    )
    _write_cdb(tmp_path, ["good.cppm", "bad.cppm"])
    await _init(client, tmp_path)

    uri, _ = await _open_and_wait(client, tmp_path, "bad.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) > 0, "Expected diagnostics for undefined symbol"


# ---------------------------------------------------------------------------
# Deep chain (5 modules)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_deep_chain(client, tmp_path):
    """A 5-level module chain should compile correctly."""
    (tmp_path / "m1.cppm").write_text(
        "export module M1;\nexport int f1() { return 1; }\n"
    )
    (tmp_path / "m2.cppm").write_text(
        "export module M2;\nimport M1;\nexport int f2() { return f1() + 1; }\n"
    )
    (tmp_path / "m3.cppm").write_text(
        "export module M3;\nimport M2;\nexport int f3() { return f2() + 1; }\n"
    )
    (tmp_path / "m4.cppm").write_text(
        "export module M4;\nimport M3;\nexport int f4() { return f3() + 1; }\n"
    )
    (tmp_path / "m5.cppm").write_text(
        "export module M5;\nimport M4;\nexport int f5() { return f4() + 1; }\n"
    )
    _write_cdb(tmp_path, ["m1.cppm", "m2.cppm", "m3.cppm", "m4.cppm", "m5.cppm"])
    await _init(client, tmp_path)

    uri, _ = await _open_and_wait(client, tmp_path, "m5.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Partition with GMF (#include inside global module fragment of partition)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_partition_with_gmf(client, tmp_path):
    """Partition with GMF (#include) should compile correctly."""
    (tmp_path / "config.h").write_text("#define MAX_SIZE 100\n")
    (tmp_path / "part_cfg.cppm").write_text(
        "module;\n"
        '#include "config.h"\n'
        "export module Cfg:Limits;\n"
        "export constexpr int max_size = MAX_SIZE;\n"
    )
    (tmp_path / "cfg.cppm").write_text("export module Cfg;\nexport import :Limits;\n")
    _write_cdb_entries(
        tmp_path,
        [
            ("part_cfg.cppm", ["-I", str(tmp_path)]),
            ("cfg.cppm", []),
        ],
    )
    await _init(client, tmp_path)

    uri, _ = await _open_and_wait(client, tmp_path, "cfg.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Cross-module partition + external import
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_partition_with_external_import(client, tmp_path):
    """Partition importing an external module should compile correctly."""
    (tmp_path / "ext.cppm").write_text(
        "export module Ext;\nexport int ext_val() { return 99; }\n"
    )
    (tmp_path / "part.cppm").write_text(
        "export module App:Core;\n"
        "import Ext;\n"
        "export int core_fn() { return ext_val() + 1; }\n"
    )
    (tmp_path / "app.cppm").write_text("export module App;\nexport import :Core;\n")
    _write_cdb(tmp_path, ["ext.cppm", "part.cppm", "app.cppm"])
    await _init(client, tmp_path)

    uri, _ = await _open_and_wait(client, tmp_path, "app.cppm")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"


# ---------------------------------------------------------------------------
# Hover on imported symbol (feature request after module compilation)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_hover_on_imported_symbol(client, tmp_path):
    """Hover on a symbol imported from a module should return info."""
    (tmp_path / "defs.cppm").write_text(
        "export module Defs;\nexport int magic_number() { return 42; }\n"
    )
    (tmp_path / "use.cpp").write_text(
        "import Defs;\nint main() { return magic_number(); }\n"
    )
    _write_cdb(tmp_path, ["defs.cppm", "use.cpp"])
    await _init(client, tmp_path)

    uri, _ = await _open_and_wait(client, tmp_path, "use.cpp")
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"

    # Hover on 'magic_number' (line 1, character 22 = inside 'magic_number()')
    hover = await client.text_document_hover_async(
        HoverParams(
            text_document=TextDocumentIdentifier(uri=uri),
            position=Position(line=1, character=22),
        )
    )
    assert hover is not None, "Hover on imported symbol should return info"
    assert hover.contents is not None
