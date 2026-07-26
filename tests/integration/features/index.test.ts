/// Integration tests for index-based LSP features: GoToDefinition,
/// FindReferences, CallHierarchy, TypeHierarchy, and WorkspaceSymbol.

import * as proto from "vscode-languageserver-protocol";
import { asLocations, locationsOf } from "@clice/tools/client";
import { cliceTest, expect } from "../../fixtures.ts";

const test = cliceTest("index_features");

function fileName(uri: string): string {
    return uri.split("/").pop() ?? uri;
}

/// Test GoToDefinition navigates from a call site to the function definition.
test("goto definition", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri), "Index not ready after 30s").toBe(true);

    // 'add' call on line 24 (0-indexed), column 12
    const locs = asLocations(await client.definitionAt(uri, 24, 12));
    expect(locs.length, `GoToDefinition returned empty list`).toBeGreaterThan(0);
    // Definition should point to line 18 where 'int add(...)' is declared
    expect(locs.some((loc) => loc.range.start.line === 18)).toBe(true);

    client.close(uri);
});

/// Test FindReferences returns all usages of global_var.
test("find references", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri), "Index not ready after 30s").toBe(true);

    // global_var definition on line 30 (0-indexed), column 4
    const result = await client.referencesAt(uri, 30, 4, { includeDeclaration: true });
    expect(result, "FindReferences returned None").not.toBeNull();
    // global_var is declared on line 30 and used on lines 33 and 37
    expect(result!.length).toBeGreaterThanOrEqual(3);

    client.close(uri);
});

/// Test prepareCallHierarchy returns a CallHierarchyItem for 'add'.
test("call hierarchy prepare", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri), "Index not ready after 30s").toBe(true);

    // 'add' definition at line 18 (0-indexed), column 4
    const result = await client.prepareCallHierarchy(uri, 18, 4);
    expect(result, "prepareCallHierarchy returned None").not.toBeNull();
    expect(result!.length).toBeGreaterThan(0);
    expect(result![0]!.name).toBe("add");

    client.close(uri);
});

/// incomingCalls with an unresolvable item returns an error, not null.
test("call hierarchy bogus item", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    const bogus: proto.CallHierarchyItem = {
        name: "ghost",
        kind: proto.SymbolKind.Function,
        uri: "file:///nonexistent/ghost.cpp",
        range: { start: { line: 0, character: 0 }, end: { line: 0, character: 5 } },
        selectionRange: { start: { line: 0, character: 0 }, end: { line: 0, character: 5 } },
    };
    await expect(client.callHierarchyIncoming(bogus)).rejects.toThrow(
        "Failed to resolve call hierarchy item",
    );
    client.close(uri);
});

/// Test incomingCalls shows compute() calls add().
test("call hierarchy incoming", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri), "Index not ready after 30s").toBe(true);

    // Prepare call hierarchy for 'add' at line 18 (0-indexed), column 4
    const items = await client.prepareCallHierarchy(uri, 18, 4);
    expect(
        items && items.length > 0,
        `prepareCallHierarchy returned ${JSON.stringify(items)}`,
    ).toBe(true);

    const incoming = await client.callHierarchyIncoming(items![0]!);
    expect(incoming, "incomingCalls returned None").not.toBeNull();
    const callerNames = incoming!.map((call) => call.from.name);
    expect(callerNames).toContain("compute");

    client.close(uri);
});

/// Test outgoingCalls shows compute() calls add().
test("call hierarchy outgoing", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri), "Index not ready after 30s").toBe(true);

    // Prepare call hierarchy for 'compute' at line 23 (0-indexed), column 4
    const items = await client.prepareCallHierarchy(uri, 23, 4);
    expect(
        items && items.length > 0,
        `prepareCallHierarchy returned ${JSON.stringify(items)}`,
    ).toBe(true);

    const outgoing = await client.callHierarchyOutgoing(items![0]!);
    expect(outgoing, "outgoingCalls returned None").not.toBeNull();
    const calleeNames = outgoing!.map((call) => call.to.name);
    expect(calleeNames).toContain("add");

    client.close(uri);
});

/// Test prepareTypeHierarchy returns a TypeHierarchyItem for 'Dog'.
test("type hierarchy prepare", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri), "Index not ready after 30s").toBe(true);

    // 'Dog' at line 8 (0-indexed), column 7
    const result = await client.prepareTypeHierarchy(uri, 8, 7);
    expect(result, "prepareTypeHierarchy returned None").not.toBeNull();
    expect(result!.length, "prepareTypeHierarchy returned empty").toBeGreaterThan(0);
    expect(result![0]!.name).toBe("Dog");

    client.close(uri);
});

/// Test supertypes of Dog includes Animal.
test("type hierarchy supertypes", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri), "Index not ready after 30s").toBe(true);

    // 'Dog' at line 8 (0-indexed), column 7
    const items = await client.prepareTypeHierarchy(uri, 8, 7);
    expect(
        items && items.length > 0,
        `prepareTypeHierarchy returned ${JSON.stringify(items)}`,
    ).toBe(true);

    const supertypes = await client.typeHierarchySupertypes(items![0]!);
    expect(supertypes, "supertypes returned None").not.toBeNull();
    const supertypeNames = supertypes!.map((t) => t.name);
    expect(supertypeNames).toContain("Animal");

    client.close(uri);
});

/// Test subtypes of Animal includes Dog and Cat.
test("type hierarchy subtypes", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri), "Index not ready after 30s").toBe(true);

    // 'Animal' at line 1, column 7
    const items = await client.prepareTypeHierarchy(uri, 1, 7);
    expect(
        items && items.length > 0,
        `prepareTypeHierarchy returned ${JSON.stringify(items)}`,
    ).toBe(true);

    const subtypes = await client.typeHierarchySubtypes(items![0]!);
    expect(subtypes, "subtypes returned None").not.toBeNull();
    const subtypeNames = subtypes!.map((t) => t.name);
    expect(subtypeNames).toContain("Dog");
    expect(subtypeNames).toContain("Cat");

    client.close(uri);
});

/// Test workspace/symbol finds symbols by query string.
test("workspace symbol", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri), "Index not ready after 30s").toBe(true);

    const result = await client.workspaceSymbols("add");
    expect(result).not.toBeNull();
    const names = result!.map((s) => s.name);
    expect(names).toContain("add");

    client.close(uri);
});

/// Test workspace/symbol finds class symbols.
test("workspace symbol class", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri), "Index not ready after 30s").toBe(true);

    const result = await client.workspaceSymbols("Animal");
    expect(result).not.toBeNull();
    const names = result!.map((s) => s.name);
    expect(names).toContain("Animal");

    client.close(uri);
});

test("goto declaration cross file", async ({ client, workspace }) => {
    // Query a closed file: background indexing skips open files, so nav.h's
    // shard only exists while nav.cpp stays closed (recorded index gap).
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri, "area"), "Index not ready after 30s").toBe(true);
    const navUri = workspace.uri("nav.cpp");

    // 'area' definition in nav.cpp line 2; declaration in nav.h line 4.
    const locs = asLocations(await client.declarationAt(navUri, 2, 4));
    const lines = locs.map((loc) => [fileName(loc.uri), loc.range.start.line]);
    expect(lines, `expected nav.h:4 declaration, got ${JSON.stringify(lines)}`).toContainEqual([
        "nav.h",
        4,
    ]);
    // Union semantics: the definition itself is also listed.
    expect(lines, `expected nav.cpp:2 definition, got ${JSON.stringify(lines)}`).toContainEqual([
        "nav.cpp",
        2,
    ]);

    client.close(uri);
});

test("goto declaration inline definition", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri), "Index not ready after 30s").toBe(true);

    // 'add' (line 18) is defined inline with no separate declaration;
    // declaration must still navigate to the definition, not return empty.
    const locs = asLocations(await client.declarationAt(uri, 18, 4));
    expect(locs.some((loc) => loc.range.start.line === 18)).toBe(true);

    client.close(uri);
});

test("goto implementation", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri), "Index not ready after 30s").toBe(true);

    // Animal::speak (line 2) is overridden by Dog::speak (9) and Cat::speak (14).
    const locs = asLocations(await client.implementationAt(uri, 2, 17));
    const lines = locs.map((loc) => loc.range.start.line).sort((a, b) => a - b);
    expect(lines).toEqual([9, 14]);

    client.close(uri);
});

test("goto type definition", async ({ client, workspace }) => {
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri, "area"), "Index not ready after 30s").toBe(true);
    const navUri = workspace.uri("nav.cpp");

    // global_shape (line 6) has type Shape, defined in nav.h line 6.
    const locs = asLocations(await client.typeDefinitionAt(navUri, 6, 6));
    const lines = locs.map((loc) => [fileName(loc.uri), loc.range.start.line]);
    expect(lines, `expected Shape definition nav.h:6, got ${JSON.stringify(lines)}`).toContainEqual(
        ["nav.h", 6],
    );

    client.close(uri);
});

test("references declaration flag", async ({ client, workspace }) => {
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri, "area"), "Index not ready after 30s").toBe(true);
    const navUri = workspace.uri("nav.cpp");

    // 'area': declaration nav.h:4, definition nav.cpp:2, call nav.cpp:9.
    const withDecl = locationsOf(await client.referencesAt(navUri, 2, 4));
    const withoutDecl = locationsOf(
        await client.referencesAt(navUri, 2, 4, { includeDeclaration: false }),
    );
    expect(withDecl.length, JSON.stringify(withDecl)).toBe(3);
    const withSet = new Set(withDecl.map((loc) => `${fileName(loc.uri)}:${loc.range.start.line}`));
    const withoutSet = new Set(
        withoutDecl.map((loc) => `${fileName(loc.uri)}:${loc.range.start.line}`),
    );
    expect(withSet).toEqual(new Set(["nav.h:4", "nav.cpp:2", "nav.cpp:9"]));
    expect(withoutSet).toEqual(new Set(["nav.cpp:9"]));

    client.close(uri);
});

test("goto implementation pure virtual", async ({ client, workspace }) => {
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri, "area"), "Index not ready after 30s").toBe(true);
    const navH = workspace.uri("nav.h");

    // Renderer::render is pure virtual; only the direct override's definition
    // is returned (DebugGLRenderer::render belongs to GLRenderer::render).
    const locs = asLocations(await client.implementationAt(navH, 12, 17));
    const lines = locs.map((loc) => [fileName(loc.uri), loc.range.start.line]);
    expect(lines).toEqual([["nav.cpp", 12]]);
});

test("goto implementation chain", async ({ client, workspace }) => {
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri, "area"), "Index not ready after 30s").toBe(true);
    const navH = workspace.uri("nav.h");

    // Intermediate override navigates to its own overriders.
    const locs = asLocations(await client.implementationAt(navH, 18, 9));
    const lines = locs.map((loc) => [fileName(loc.uri), loc.range.start.line]);
    expect(lines).toEqual([["nav.cpp", 14]]);
});

test("goto type definition return value", async ({ client, workspace }) => {
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri, "area"), "Index not ready after 30s").toBe(true);
    const navH = workspace.uri("nav.h");

    // Known index gap: functions carry no TypeDefinition relation for their
    // return type, so this currently yields no results.
    const result = await client.typeDefinitionAt(navH, 25, 6);
    expect(result).not.toBeNull();
    expect(asLocations(result).length).toBe(0);
});

test("goto declaration forward declared", async ({ client, workspace }) => {
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri, "area"), "Index not ready after 30s").toBe(true);
    const navUri = workspace.uri("nav.cpp");

    // 'Shape' at the global_shape declaration: forward declaration and
    // definition are both listed.
    const locs = asLocations(await client.declarationAt(navUri, 6, 0));
    const lines = locs.map((loc) => [fileName(loc.uri), loc.range.start.line]);
    expect(lines).toContainEqual(["nav.h", 2]);
    expect(lines).toContainEqual(["nav.h", 6]);
});

test("navigation empty open document", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri), "Index not ready after 30s").toBe(true);

    // 'add' has no implementations: open documents get [] back, not an error.
    const result = await client.implementationAt(uri, 18, 4);
    expect(result).not.toBeNull();
    expect(asLocations(result).length).toBe(0);
});

test("navigation closed document empty", async ({ client, workspace }) => {
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri, "area"), "Index not ready after 30s").toBe(true);
    const navUri = workspace.uri("nav.cpp");

    // Index-only navigation serves closed documents; an empty result is a
    // real answer, not an error.
    const result = await client.implementationAt(navUri, 2, 4);
    expect(result).not.toBeNull();
    expect(asLocations(result).length).toBe(0);
});

test("definition on include", async ({ client }) => {
    const [uri] = await client.openAndWait("nav.cpp");

    // Preamble include (line 0): served master-side from the PCH's links.
    let locs = asLocations(await client.definitionAt(uri, 0, 12));
    expect(locs.some((loc) => loc.uri.endsWith("nav.h"))).toBe(true);

    // Non-preamble include (line 20): served by the worker's AST.
    locs = asLocations(await client.definitionAt(uri, 20, 12));
    expect(locs.some((loc) => loc.uri.endsWith("nav_late.h"))).toBe(true);
});

test("document links include preamble", async ({ client }) => {
    const [uri] = await client.openAndWait("nav.cpp");
    const links = await client.documentLinks(uri);
    const targets = (links ?? []).map((link) => link.target ?? "");
    expect(targets.some((target) => target.includes("nav.h"))).toBe(true);
    expect(targets.some((target) => target.includes("nav_late.h"))).toBe(true);
});
