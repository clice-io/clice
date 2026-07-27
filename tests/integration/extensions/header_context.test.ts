/// Integration tests for header context LSP extension commands.
///
/// Tests the clice/queryContext, clice/currentContext, and clice/switchContext
/// extension commands that allow switching the compilation context for header
/// files.
///
/// utils.h uses Point without including types.h itself -- it depends on
/// main.cpp to provide that include. Without header context resolution, the
/// server cannot compile utils.h at all.

import * as proto from "vscode-languageserver-protocol";
import { withTimeout } from "@clice/tools/client";
import { expect, test } from "../fixtures.ts";

/// clice/queryContext on a header should return source files that include it.
test("query context returns host sources", async ({ session }) => {
    const { client } = await session("header_context");
    await client.openAndWait("main.cpp");

    const [utilsUri] = client.open("utils.h");

    const result = await client.queryContext(utilsUri);
    expect(result).not.toBeNull();
    const { total, contexts } = result;
    expect(
        total,
        `Should find at least main.cpp as context, got total=${total}`,
    ).toBeGreaterThanOrEqual(1);
    // Check that main.cpp is among the contexts.
    const uris = contexts.map((c) => c.uri);
    expect(
        uris.some((u) => u.includes("main.cpp")),
        `main.cpp should be listed as a context option, got: ${uris.join(", ")}`,
    ).toBe(true);
});

/// clice/queryContext on a source file should return its CDB entries.
test("query context source file returns cdb entries", async ({ session }) => {
    const { client } = await session("header_context");
    const [mainUri] = await client.openAndWait("main.cpp");

    const result = await client.queryContext(mainUri);
    expect(result).not.toBeNull();
    // header_context workspace has exactly 1 CDB entry for main.cpp.
    expect(result.total).toBe(1);
    expect(result.contexts.length).toBe(1);
});

/// clice/currentContext should return null context by default.
test("current context default null", async ({ session }) => {
    const { client } = await session("header_context");
    await client.openAndWait("main.cpp");

    const [utilsUri] = client.open("utils.h");

    const result = await client.currentContext(utilsUri);
    expect(result).not.toBeNull();
    expect(result.context, "Default context should be null (no explicit override)").toBeNull();
});

/// switchContext should set the active context, currentContext should reflect it.
test("switch context and current context", async ({ session }) => {
    const { client } = await session("header_context");
    const [mainUri] = await client.openAndWait("main.cpp");

    const [utilsUri] = client.open("utils.h");

    // Switch context to main.cpp.
    const switchResult = await client.switchContext(utilsUri, mainUri);
    expect(switchResult).not.toBeNull();
    expect(switchResult.success).toBe(true);

    // Verify currentContext now returns main.cpp.
    const current = await client.currentContext(utilsUri);
    expect(current).not.toBeNull();
    const ctx = current.context;
    expect(
        ctx,
        "After switchContext, currentContext should return the active context",
    ).not.toBeNull();
    expect(ctx!.uri).toContain("main.cpp");
});

/// Full flow: open, query, switch, verify hover works in header context.
test("full context flow", async ({ session }) => {
    const { client } = await session("header_context");
    // 1. Open main.cpp, wait for initial compile.
    const [mainUri] = await client.openAndWait("main.cpp");

    // 2. Open utils.h (non self-contained header using Point from types.h).
    const [utilsUri] = client.open("utils.h");

    // 3. queryContext on utils.h -> should return main.cpp as a context option.
    const query = await client.queryContext(utilsUri);
    expect(query.total).toBeGreaterThanOrEqual(1);
    const contextUris = query.contexts.map((c) => c.uri);
    expect(contextUris.some((u) => u.includes("main.cpp"))).toBe(true);

    // 4. currentContext on utils.h -> should be null (default).
    const current = await client.currentContext(utilsUri);
    expect(current.context).toBeNull();

    // 5. switchContext on utils.h to main.cpp.
    const switched = await client.switchContext(utilsUri, mainUri);
    expect(switched.success).toBe(true);

    // 6. currentContext on utils.h -> should now be main.cpp.
    const current2 = await client.currentContext(utilsUri);
    const ctx = current2.context;
    expect(ctx).not.toBeNull();
    expect(ctx!.uri).toContain("main.cpp");

    // 7. Hover on 'calc' function in utils.h -> should work (proves header compiled).
    const arrived = client.armDiagnostics(utilsUri);
    const hover = await withTimeout(client.hoverAt(utilsUri, 6, 12), 30_000, "hover"); // 'calc' function
    expect(hover, "Hover on 'calc' in header should work after switchContext").not.toBeNull();

    // 8. Check diagnostics on utils.h -> should have 0 errors.
    await withTimeout(arrived, 30_000, "diagnostics");
    const diags = client.diagnostics.get(utilsUri) ?? [];
    const errors = diags.filter((d) => d.severity === proto.DiagnosticSeverity.Error);
    expect(
        errors.length,
        `Header should have no errors after switchContext, got: ${JSON.stringify(errors)}`,
    ).toBe(0);
});

/// queryContext on a deeply nested header (main.cpp -> utils.h -> inner.h)
/// should still find main.cpp as the host source.
test("deep nested header context", async ({ session }) => {
    const { client } = await session("header_context");
    await client.openAndWait("main.cpp");

    const [innerUri] = client.open("inner.h");

    // queryContext on inner.h should find main.cpp through the chain.
    const result = await client.queryContext(innerUri);
    expect(result).not.toBeNull();
    const total = result.total;
    expect(
        total,
        `Deep nested header should find host sources, got total=${total}`,
    ).toBeGreaterThanOrEqual(1);
    const uris = result.contexts.map((c) => c.uri);
    expect(
        uris.some((u) => u.includes("main.cpp")),
        `main.cpp should be a context for inner.h, got: ${uris.join(", ")}`,
    ).toBe(true);
});

/// switchContext + hover on deeply nested header (main.cpp -> utils.h -> inner.h).
test("deep nested switch context and hover", async ({ session }) => {
    const { client } = await session("header_context");
    const [mainUri] = await client.openAndWait("main.cpp");

    const [innerUri] = client.open("inner.h");

    // Switch inner.h context to main.cpp.
    const switched = await client.switchContext(innerUri, mainUri);
    expect(switched.success).toBe(true);

    // Hover on 'inner_origin' in inner.h should work (Point available via preamble).
    const hover = await withTimeout(client.hoverAt(innerUri, 3, 14), 30_000, "hover"); // 'inner_origin'
    expect(hover, "Hover on inner_origin should work after switchContext").not.toBeNull();
});

/// queryContext on a source file with multiple CDB entries should return all.
test("query context multiple cdb entries", async ({ session }) => {
    const { client } = await session("multi_context");
    const [mainUri] = await client.openAndWait("main.cpp");

    const result = await client.queryContext(mainUri);
    expect(result).not.toBeNull();
    const total = result.total;
    expect(total, `Should find at least 2 CDB entries, got total=${total}`).toBeGreaterThanOrEqual(
        2,
    );
    const labels = result.contexts.map((c) => c.label);
    // Each entry should have distinguishing flags in the label.
    expect(
        labels.some((l) => l.includes("CONFIG_A")),
        `Should find CONFIG_A, got: ${labels.join(", ")}`,
    ).toBe(true);
    expect(
        labels.some((l) => l.includes("CONFIG_B")),
        `Should find CONFIG_B, got: ${labels.join(", ")}`,
    ).toBe(true);
});

/// Switching a header between two hosts with different macro setups must
/// recompile it under the new host's preamble.
test("switch between two hosts", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("shared.h", "VALUE_TYPE get_value();\n");
    workspace.write(
        "a.cpp",
        '#define VALUE_TYPE int\n#include "shared.h"\nint main() { return 0; }\n',
    );
    workspace.write(
        "b.cpp",
        '#define VALUE_TYPE float\n#include "shared.h"\nfloat f() { return 0; }\n',
    );
    workspace.writeEntries([
        ["a.cpp", []],
        ["b.cpp", []],
    ]);
    await client.initialize(workspace);

    const [aUri] = await client.openAndWait("a.cpp");
    const [bUri] = await client.openAndWait("b.cpp");
    const [sharedUri] = client.open("shared.h");

    const hoverGetValueContains = async (text: string): Promise<boolean> => {
        const hover = await client.hoverAt(sharedUri, 0, 12); // 'get_value'
        expect(hover, "Hover on get_value should work").not.toBeNull();
        return JSON.stringify(hover!.contents).includes(text);
    };

    // Host a.cpp: VALUE_TYPE is int.
    let switched = await client.switchContext(sharedUri, aUri);
    expect(switched.success).toBe(true);
    expect(await hoverGetValueContains("int"), "Expected int under host a.cpp").toBe(true);

    // Host b.cpp: VALUE_TYPE is float. This exercises the cached-context
    // invalidation branch (active context differs from cached host).
    switched = await client.switchContext(sharedUri, bUri);
    expect(switched.success).toBe(true);
    expect(await hoverGetValueContains("float"), "Expected float under host b.cpp").toBe(true);
});
