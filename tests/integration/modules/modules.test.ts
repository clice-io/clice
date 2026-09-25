/// Integration tests for C++20 module support.

import * as fs from "node:fs";
import * as path from "node:path";
import * as proto from "vscode-languageserver-protocol";
import {
    IDLE_TIMEOUT,
    locationsOf,
    sleep,
    withTimeout,
    type CliceClient,
} from "@clice/tools/client";
import { DATA_DIR } from "@clice/tools/compile-commands";
import { expect, test } from "../fixtures.ts";

/// URIs of the definition locations at a position.
async function definitionUris(
    client: CliceClient,
    uri: string,
    line: number,
    character: number,
): Promise<string[]> {
    const result = (await client.definitionAt(uri, line, character)) as
        | proto.Location
        | proto.Location[]
        | null;
    return locationsOf(result).map((loc) => loc.uri);
}

test("single module no deps", async ({ session }) => {
    const { client } = await session("modules/single_module_no_deps");
    const [uri] = await client.openAndWait("mod_a.cppm");
    const diags = client.diagnostics.get(uri) ?? [];
    expect(diags.length, `Expected no diagnostics, got: ${JSON.stringify(diags)}`).toBe(0);
});

/// Opening mod_b that imports mod_a should trigger dependency compilation.
test("chained modules", async ({ session }) => {
    const { client } = await session("modules/chained_modules");
    const [uri] = await client.openAndWait("mod_b.cppm");
    const diags = client.diagnostics.get(uri) ?? [];
    expect(diags.length, `Expected no diagnostics, got: ${JSON.stringify(diags)}`).toBe(0);
});

test("diamond modules", async ({ session }) => {
    const { client } = await session("modules/diamond_modules");
    const [uri] = await client.openAndWait("top.cppm");
    const diags = client.diagnostics.get(uri) ?? [];
    expect(diags.length, `Expected no diagnostics, got: ${JSON.stringify(diags)}`).toBe(0);
});

test("dotted module name", async ({ session }) => {
    const { client } = await session("modules/dotted_module_name");
    const [uri] = await client.openAndWait("app.cppm");
    const diags = client.diagnostics.get(uri) ?? [];
    expect(diags.length, `Expected no diagnostics, got: ${JSON.stringify(diags)}`).toBe(0);
});

/// Implementation unit (module M; without export) should compile using the
/// interface PCM.
test("module implementation unit", async ({ session }) => {
    const { client } = await session("modules/module_implementation_unit");
    const [uri] = await client.openAndWait("greeter_impl.cpp");
    const diags = client.diagnostics.get(uri) ?? [];
    expect(diags.length, `Expected no diagnostics, got: ${JSON.stringify(diags)}`).toBe(0);
});

/// A regular .cpp that imports a module should get PCM deps compiled first.
test("consumer imports module", async ({ session }) => {
    const { client } = await session("modules/consumer_imports_module");
    const [uri] = await client.openAndWait("main.cpp");
    const diags = client.diagnostics.get(uri) ?? [];
    expect(diags.length, `Expected no diagnostics, got: ${JSON.stringify(diags)}`).toBe(0);
});

/// Partitions should be compiled in correct dependency order.
test("module partitions", async ({ session }) => {
    const { client } = await session("modules/module_partitions");
    const [uri] = await client.openAndWait("lib.cppm");
    const diags = client.diagnostics.get(uri) ?? [];
    expect(diags.length, `Expected no diagnostics, got: ${JSON.stringify(diags)}`).toBe(0);
});

test("partition interface", async ({ session }) => {
    const { client } = await session("modules/partition_interface");
    const [uri] = await client.openAndWait("primary.cppm");
    const diags = client.diagnostics.get(uri) ?? [];
    expect(diags.length, `Expected no diagnostics, got: ${JSON.stringify(diags)}`).toBe(0);
});

test("partition chain", async ({ session }) => {
    const { client } = await session("modules/partition_chain");
    const [uri] = await client.openAndWait("sys.cppm");
    const diags = client.diagnostics.get(uri) ?? [];
    expect(diags.length, `Expected no diagnostics, got: ${JSON.stringify(diags)}`).toBe(0);
});

/// Re-exported symbols (export import) should be accessible through the wrapper.
test("re export", async ({ session }) => {
    const { client } = await session("modules/re_export");
    const [uri] = await client.openAndWait("user.cppm");
    const diags = client.diagnostics.get(uri) ?? [];
    expect(diags.length, `Expected no diagnostics, got: ${JSON.stringify(diags)}`).toBe(0);
});

test("export block", async ({ session }) => {
    const { client } = await session("modules/export_block");
    const [uri] = await client.openAndWait("consumer.cppm");
    const diags = client.diagnostics.get(uri) ?? [];
    expect(diags.length, `Expected no diagnostics, got: ${JSON.stringify(diags)}`).toBe(0);
});

test("global module fragment", async ({ session }) => {
    const { client } = await session("modules/global_module_fragment");
    const [uri] = await client.openAndWait("gmf.cppm");
    const diags = client.diagnostics.get(uri) ?? [];
    expect(diags.length, `Expected no diagnostics, got: ${JSON.stringify(diags)}`).toBe(0);
});

test("private module fragment", async ({ session }) => {
    const { client } = await session("modules/private_module_fragment");
    const [uri] = await client.openAndWait("priv.cppm");
    const diags = client.diagnostics.get(uri) ?? [];
    expect(diags.length, `Expected no diagnostics, got: ${JSON.stringify(diags)}`).toBe(0);
});

test("export namespace", async ({ session }) => {
    const { client } = await session("modules/export_namespace");
    const [uri] = await client.openAndWait("calc.cppm");
    const diags = client.diagnostics.get(uri) ?? [];
    expect(diags.length, `Expected no diagnostics, got: ${JSON.stringify(diags)}`).toBe(0);
});

test("gmf with import", async ({ session }) => {
    const { client } = await session("modules/gmf_with_import");
    const [uri] = await client.openAndWait("combined.cppm");
    const diags = client.diagnostics.get(uri) ?? [];
    expect(diags.length, `Expected no diagnostics, got: ${JSON.stringify(diags)}`).toBe(0);
});

test("independent modules", async ({ session }) => {
    const { client } = await session("modules/independent_modules");
    const [uriX] = await client.openAndWait("x.cppm");
    const diagsX = client.diagnostics.get(uriX) ?? [];
    expect(diagsX.length, `Expected no diagnostics for X, got: ${JSON.stringify(diagsX)}`).toBe(0);

    const [uriY] = await client.openAndWait("y.cppm");
    const diagsY = client.diagnostics.get(uriY) ?? [];
    expect(diagsY.length, `Expected no diagnostics for Y, got: ${JSON.stringify(diagsY)}`).toBe(0);
});

test("template export", async ({ session }) => {
    const { client } = await session("modules/template_export");
    const [uri] = await client.openAndWait("use_tmpl.cppm");
    const diags = client.diagnostics.get(uri) ?? [];
    expect(diags.length, `Expected no diagnostics, got: ${JSON.stringify(diags)}`).toBe(0);
});

test("class export and inheritance", async ({ session }) => {
    const { client } = await session("modules/class_export_and_inheritance");
    const [uri] = await client.openAndWait("circle.cppm");
    const diags = client.diagnostics.get(uri) ?? [];
    expect(diags.length, `Expected no diagnostics, got: ${JSON.stringify(diags)}`).toBe(0);
});

/// Closing and reopening a modified module file should recompile without errors.
test("save recompile", async ({ session }) => {
    const { client, workspace } = session.tmp();
    const src = path.join(DATA_DIR, "modules", "save_recompile");
    for (const f of fs.readdirSync(src)) {
        const full = path.join(src, f);
        if (fs.statSync(full).isFile()) {
            fs.copyFileSync(full, workspace.path(f));
        }
    }

    workspace.generateCDB();
    await client.initialize(workspace);

    // Open and compile Mid (which triggers Leaf PCM build).
    const [midUri] = await client.openAndWait("mid.cppm");
    let diags = client.diagnostics.get(midUri) ?? [];
    expect(diags.length).toBe(0);

    // Open Leaf and trigger compilation via hover.
    const [leafUri] = client.open("leaf.cppm");
    await client.hoverAt(leafUri, 0, 0);

    // Close Leaf, modify on disk, and reopen with new content.
    client.close(leafUri);

    const newContent = "export module Leaf;\nexport int leaf() { return 100; }\n";
    workspace.write("leaf.cppm", newContent);

    client.open("leaf.cppm", 1, { text: newContent });
    // Send hover to trigger recompilation via pull-based model.
    const arrived = client.armDiagnostics(leafUri);
    await client.hoverAt(leafUri, 0, 0);
    await withTimeout(arrived, 30_000, "diagnostics");

    diags = client.diagnostics.get(leafUri) ?? [];
    expect(diags.length, `Expected no diagnostics after save, got: ${JSON.stringify(diags)}`).toBe(
        0,
    );
});

test("module compile error", async ({ session }) => {
    const { client } = await session("modules/module_compile_error");
    const [uri] = await client.openAndWait("bad.cppm");
    const diags = client.diagnostics.get(uri) ?? [];
    expect(diags.length, "Expected diagnostics for undefined symbol").toBeGreaterThan(0);
    expect(
        diags.some(
            (d) => d.range.start.line === 4 && d.severity === proto.DiagnosticSeverity.Error,
        ),
        `Expected an error diagnostic on line 4, got: ${JSON.stringify(diags)}`,
    ).toBe(true);
});

/// A 5-level module chain (m1->m2->...->m5) should compile correctly.
test("deep chain", async ({ session }) => {
    const { client } = await session("modules/deep_chain");
    const [uri] = await client.openAndWait("m5.cppm");
    const diags = client.diagnostics.get(uri) ?? [];
    expect(diags.length, `Expected no diagnostics, got: ${JSON.stringify(diags)}`).toBe(0);
});

test("partition with gmf", async ({ session }) => {
    const { client } = await session("modules/partition_with_gmf");
    const [uri] = await client.openAndWait("cfg.cppm");
    const diags = client.diagnostics.get(uri) ?? [];
    expect(diags.length, `Expected no diagnostics, got: ${JSON.stringify(diags)}`).toBe(0);
});

test("partition with external import", async ({ session }) => {
    const { client } = await session("modules/partition_with_external_import");
    const [uri] = await client.openAndWait("app.cppm");
    const diags = client.diagnostics.get(uri) ?? [];
    expect(diags.length, `Expected no diagnostics, got: ${JSON.stringify(diags)}`).toBe(0);
});

/// Hover on a symbol imported from a module should return type info.
test("hover on imported symbol", async ({ session }) => {
    const { client } = await session("modules/hover_on_imported_symbol");
    const [uri] = await client.openAndWait("use.cpp");
    const diags = client.diagnostics.get(uri) ?? [];
    expect(diags.length, `Expected no diagnostics, got: ${JSON.stringify(diags)}`).toBe(0);

    const hover = await client.hoverAt(uri, 3, 11);
    expect(hover, "Hover on imported symbol should return info").not.toBeNull();
    expect(hover!.contents).not.toBeNull();
});

/// Plain .cpp with no modules should compile normally (CompileGraph null path).
test("no modules plain cpp", async ({ session }) => {
    const { client } = await session("modules/no_modules_plain_cpp");
    const [uri] = await client.openAndWait("plain.cpp");
    const diags = client.diagnostics.get(uri) ?? [];
    expect(diags.length, `Expected no diagnostics, got: ${JSON.stringify(diags)}`).toBe(0);
});

/// Circular module imports should not hang the server.
///
/// The CompileGraph's cycle detection should prevent deadlock. We verify
/// the server remains responsive by opening a non-cyclic file afterwards.
test("circular module dependency", async ({ session }) => {
    const { client } = await session("modules/circular_module_dependency");
    client.open("cycle_a.cppm");
    await sleep(IDLE_TIMEOUT);

    const [uriOk] = await client.openAndWait("ok.cppm");
    const diags = client.diagnostics.get(uriOk) ?? [];
    expect(
        diags.length,
        `Non-cyclic module should compile fine after cycle attempt, got: ${JSON.stringify(diags)}`,
    ).toBe(0);
});

test("import definition", async ({ session }) => {
    const { client } = await session("modules/consumer_imports_module");
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri, "Math"), "Index not ready").toBe(true);
    let locs = await definitionUris(client, uri, 0, 8);
    expect(
        locs.some((u) => u.endsWith("math.cppm")),
        JSON.stringify(locs),
    ).toBe(true);

    // Cursor on the `import` keyword itself must not navigate to the module.
    locs = await definitionUris(client, uri, 0, 2);
    expect(
        locs.some((u) => u.endsWith("math.cppm")),
        JSON.stringify(locs),
    ).toBe(false);
});

test("module decl definition", async ({ session }) => {
    const { client } = await session("modules/module_implementation_unit");
    // `module Greeter;` in the implementation unit navigates to the interface.
    const [uri] = await client.openAndWait("greeter_impl.cpp");
    expect(await client.waitForIndex(uri, "Greeter"), "Index not ready").toBe(true);
    const locs = await definitionUris(client, uri, 0, 8);
    expect(
        locs.some((u) => u.endsWith("greeter.cppm")),
        JSON.stringify(locs),
    ).toBe(true);
});

test("dotted import definition", async ({ session }) => {
    const { client } = await session("modules/dotted_module_name");
    // `import my.io;` on line 1 of app.cppm.
    const [uri] = await client.openAndWait("app.cppm");
    expect(await client.waitForIndex(uri, "my.io"), "Index not ready").toBe(true);
    const locs = await definitionUris(client, uri, 1, 9);
    expect(
        locs.some((u) => u.endsWith("io.cppm")),
        JSON.stringify(locs),
    ).toBe(true);
});

test("partition import definition", async ({ session }) => {
    const { client } = await session("modules/module_partitions");
    const [uri] = await client.openAndWait("lib.cppm");
    expect(await client.waitForIndex(uri, "Lib:A"), "Index not ready").toBe(true);
    // `export import :A;` on line 1; the partition name resolves through
    // the enclosing module.
    const locs = await definitionUris(client, uri, 1, 15);
    expect(
        locs.some((u) => u.endsWith("part_a.cppm")),
        JSON.stringify(locs),
    ).toBe(true);
});

test("macro import definition", async ({ session }) => {
    const { client } = await session("modules/macro_import");
    // `import MATH_MODULE;` where the name comes from a macro: the index
    // anchors the occurrence at the expansion site.
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri, "Math"), "Index not ready").toBe(true);
    const locs = await definitionUris(client, uri, 1, 9);
    expect(
        locs.some((u) => u.endsWith("math.cppm")),
        JSON.stringify(locs),
    ).toBe(true);
});
