/// Integration tests for index-based LSP features: GoToDefinition,
/// FindReferences, CallHierarchy, TypeHierarchy, and WorkspaceSymbol.

import * as proto from "vscode-languageserver-protocol";
import type { Serve } from "@clice/tools/actions";
import { at, expect, serve } from "../../fixtures.ts";

const test = serve.data("index_features");

/// main.cpp open and compiled, every unit indexed.
async function indexed(s: Serve): Promise<void> {
    await s.compiled("main.cpp");
    await s.indexed();
}

function locations(reply: unknown): proto.Location[] {
    return (reply as proto.Location[] | null) ?? [];
}

async function prepareCalls(s: Serve, anchor: string): Promise<proto.CallHierarchyItem[]> {
    const items = (await s.request("textDocument/prepareCallHierarchy", at("main.cpp", anchor))) as
        | proto.CallHierarchyItem[]
        | null;
    expect(
        items && items.length > 0,
        `prepareCallHierarchy returned ${JSON.stringify(items)}`,
    ).toBe(true);
    return items!;
}

async function prepareTypes(s: Serve, anchor: string): Promise<proto.TypeHierarchyItem[]> {
    const items = (await s.request("textDocument/prepareTypeHierarchy", at("main.cpp", anchor))) as
        | proto.TypeHierarchyItem[]
        | null;
    expect(
        items && items.length > 0,
        `prepareTypeHierarchy returned ${JSON.stringify(items)}`,
    ).toBe(true);
    return items!;
}

const ADD_DEFINITION = at("main.cpp", "add(int a, int b)");

test("goto definition", async ({ s }) => {
    await indexed(s);
    expect(s.show(await s.definition(at("main.cpp", "add(1, 2)")))).toBe(
        "main.cpp: int add(int a, int b) {",
    );
});

test("find references", async ({ s }) => {
    await indexed(s);
    // global_var is declared once and used twice.
    expect(
        s
            .show(await s.references(at("main.cpp", "global_var = 42")))
            .split("\n")
            .sort(),
    ).toEqual([
        "main.cpp: int global_var = 42;",
        "main.cpp: return global_var * 2;",
        "main.cpp: return global_var + 1;",
    ]);
});

test("call hierarchy prepare", async ({ s }) => {
    await indexed(s);
    const [item] = await prepareCalls(s, "add(int a, int b)");
    expect(item!.name).toBe("add");
});

/// incomingCalls with an unresolvable item returns an error, not null.
test("call hierarchy bogus item", async ({ s }) => {
    await s.compiled("main.cpp");
    const bogus: proto.CallHierarchyItem = {
        name: "ghost",
        kind: proto.SymbolKind.Function,
        uri: "file:///nonexistent/ghost.cpp",
        range: { start: { line: 0, character: 0 }, end: { line: 0, character: 5 } },
        selectionRange: { start: { line: 0, character: 0 }, end: { line: 0, character: 5 } },
    };
    await expect(s.client.callHierarchyIncoming(bogus)).rejects.toThrow(
        "Failed to resolve call hierarchy item",
    );
});

test("call hierarchy incoming", async ({ s }) => {
    await indexed(s);
    const [item] = await prepareCalls(s, "add(int a, int b)");
    const incoming = await s.client.callHierarchyIncoming(item!);
    expect(incoming, "incomingCalls returned None").not.toBeNull();
    expect(incoming!.map((call) => call.from.name)).toContain("compute");
});

/// An item a client sends back without its data resolves at its name.
test("call hierarchy item without data", async ({ s }) => {
    await indexed(s);
    const items = await prepareCalls(s, "add(int a, int b)");
    expect(items.length).toBe(1);
    const stripped: proto.CallHierarchyItem = { ...items[0]! };
    delete stripped.data;
    const incoming = await s.client.callHierarchyIncoming(stripped);
    expect(incoming!.map((call) => call.from.name)).toContain("compute");
});

test("call hierarchy outgoing", async ({ s }) => {
    await indexed(s);
    const [item] = await prepareCalls(s, "compute() {");
    const outgoing = await s.client.callHierarchyOutgoing(item!);
    expect(outgoing, "outgoingCalls returned None").not.toBeNull();
    expect(outgoing!.map((call) => call.to.name)).toContain("add");
});

test("type hierarchy prepare", async ({ s }) => {
    await indexed(s);
    const [item] = await prepareTypes(s, "Dog : public");
    expect(item!.name).toBe("Dog");
});

test("type hierarchy supertypes", async ({ s }) => {
    await indexed(s);
    const [item] = await prepareTypes(s, "Dog : public");
    const supertypes = await s.client.typeHierarchySupertypes(item!);
    expect(supertypes, "supertypes returned None").not.toBeNull();
    expect(supertypes!.map((t) => t.name)).toContain("Animal");
});

test("type hierarchy subtypes", async ({ s }) => {
    await indexed(s);
    const [item] = await prepareTypes(s, "struct |Animal {");
    const subtypes = await s.client.typeHierarchySubtypes(item!);
    expect(subtypes, "subtypes returned None").not.toBeNull();
    const names = subtypes!.map((t) => t.name);
    expect(names).toContain("Dog");
    expect(names).toContain("Cat");
});

test("workspace symbol", async ({ s }) => {
    await indexed(s);
    const result = await s.workspaceSymbols("add");
    expect(result).not.toBeNull();
    expect(result!.map((symbol) => symbol.name)).toContain("add");
});

test("workspace symbol class", async ({ s }) => {
    await indexed(s);
    const result = await s.workspaceSymbols("Animal");
    expect(result).not.toBeNull();
    expect(result!.map((symbol) => symbol.name)).toContain("Animal");
});

/// 'area' is defined in nav.cpp and declared in nav.h, both closed: nav.h's
/// shard only exists while nav.cpp stays closed (background indexing skips
/// open files; recorded index gap).
const AREA_DEFINITION = at("nav.cpp", "area(const Shape& s) {");
const AREA_DECLARATION = at("nav.h", "area(const Shape& s);");

test("goto declaration cross file", async ({ s }) => {
    await indexed(s);
    const shown = s.show(await s.request("textDocument/declaration", AREA_DEFINITION)).split("\n");
    expect(shown).toContain("nav.h: int area(const Shape& s);");
    // The cursor stands on the definition site; it is not an answer.
    expect(shown).not.toContain("nav.cpp: int area(const Shape& s) {");
});

/// Standing on a definition or declaration navigates to the other site.
test("goto definition alternate", async ({ s }) => {
    await indexed(s);
    expect(s.show(await s.definition(AREA_DEFINITION))).toBe("nav.h: int area(const Shape& s);");
    expect(s.show(await s.definition(AREA_DECLARATION))).toBe(
        "nav.cpp: int area(const Shape& s) {",
    );
});

/// A closed file answers from the index alone: no symbol under the cursor
/// is an empty answer, not an error.
test("goto definition closed blank", async ({ s }) => {
    await indexed(s);
    expect(locations(await s.definition(at("nav.cpp", '"nav.h"\n|\nint area')))).toEqual([]);
});

/// A symbol with no definition anywhere navigates to its declaration
/// instead of returning empty.
test("goto definition declaration only", async ({ s }) => {
    await indexed(s);
    expect(s.show(await s.definition(at("nav.cpp", "area_scale(s)")))).toBe(
        "nav.h: int area_scale(const Shape& s);",
    );
});

/// An inline-defined symbol has nowhere else to point: the definition
/// site stays the answer (clients render self-navigation as a peek).
test("goto definition inline definition", async ({ s }) => {
    await indexed(s);
    expect(s.show(await s.definition(ADD_DEFINITION))).toBe("main.cpp: int add(int a, int b) {");
});

test("goto declaration inline definition", async ({ s }) => {
    await indexed(s);
    // 'add' is defined inline with no separate declaration; declaration
    // must still navigate to the definition, not return empty.
    expect(
        s.show(await s.request("textDocument/declaration", ADD_DEFINITION)).split("\n"),
    ).toContain("main.cpp: int add(int a, int b) {");
});

test("goto implementation", async ({ s }) => {
    await indexed(s);
    // Animal::speak is overridden by Dog::speak and Cat::speak, the only
    // two lines spelling an override.
    const sites = locations(
        await s.request("textDocument/implementation", at("main.cpp", "speak() {}")),
    );
    expect(s.show(sites)).toBe(
        "main.cpp: void speak() override {}\nmain.cpp: void speak() override {}",
    );
    expect(new Set(sites.map((site) => site.range.start.line)).size).toBe(2);
});

test("goto type definition", async ({ s }) => {
    await indexed(s);
    // global_shape has type Shape, defined in nav.h.
    expect(
        s
            .show(await s.request("textDocument/typeDefinition", at("nav.cpp", "global_shape;")))
            .split("\n"),
    ).toContain("nav.h: struct Shape {");
});

test("references declaration flag", async ({ s }) => {
    await indexed(s);
    // 'area': declaration in nav.h, definition and call in nav.cpp.
    const withDecl = s
        .show(await s.references(AREA_DEFINITION))
        .split("\n")
        .sort();
    const withoutDecl = s.show(
        await s.request("textDocument/references", AREA_DEFINITION, {
            context: { includeDeclaration: false },
        }),
    );
    expect(withDecl).toEqual([
        "nav.cpp: int area(const Shape& s) {",
        "nav.cpp: return area(global_shape);",
        "nav.h: int area(const Shape& s);",
    ]);
    expect(withoutDecl).toBe("nav.cpp: return area(global_shape);");
});

test("goto implementation pure virtual", async ({ s }) => {
    await indexed(s);
    // Renderer::render is pure virtual; only the direct override's definition
    // is returned (DebugGLRenderer::render belongs to GLRenderer::render).
    expect(
        s.show(await s.request("textDocument/implementation", at("nav.h", "render() = 0"))),
    ).toBe("nav.cpp: void GLRenderer::render() {}");
});

test("goto implementation chain", async ({ s }) => {
    await indexed(s);
    // Intermediate override navigates to its own overriders.
    expect(
        s.show(
            await s.request(
                "textDocument/implementation",
                at("nav.h", "GLRenderer : Renderer {\n    void |render() override;"),
            ),
        ),
    ).toBe("nav.cpp: void DebugGLRenderer::render() {}");
});

test("goto type definition return value", async ({ s }) => {
    await indexed(s);
    // Known index gap: functions carry no TypeDefinition relation for their
    // return type, so this currently yields no results.
    const result = await s.request(
        "textDocument/typeDefinition",
        at("nav.h", "Shape |make_unit_shape();"),
    );
    expect(result).not.toBeNull();
    expect(locations(result).length).toBe(0);
});

test("goto declaration forward declared", async ({ s }) => {
    await indexed(s);
    // 'Shape' at the global_shape declaration: forward declaration and
    // definition are both listed.
    const shown = s
        .show(await s.request("textDocument/declaration", at("nav.cpp", "Shape global_shape;")))
        .split("\n");
    expect(shown).toContain("nav.h: struct Shape;");
    expect(shown).toContain("nav.h: struct Shape {");
});

test("navigation empty open document", async ({ s }) => {
    await indexed(s);
    // 'add' has no implementations: open documents get [] back, not an error.
    const result = await s.request("textDocument/implementation", ADD_DEFINITION);
    expect(result).not.toBeNull();
    expect(locations(result).length).toBe(0);
});

test("navigation closed document empty", async ({ s }) => {
    await indexed(s);
    // Index-only navigation serves closed documents; an empty result is a
    // real answer, not an error.
    const result = await s.request("textDocument/implementation", AREA_DEFINITION);
    expect(result).not.toBeNull();
    expect(locations(result).length).toBe(0);
});

test("definition on include", async ({ s }) => {
    await s.compiled("nav.cpp");

    // Preamble include: served master-side from the PCH's links.
    expect(s.show(await s.definition(at("nav.cpp", '"na|v.h"')))).toBe("nav.h: #pragma once");

    // Non-preamble include: served by the worker's AST.
    expect(s.show(await s.definition(at("nav.cpp", '"na|v_late.h"')))).toBe(
        "nav_late.h: #pragma once",
    );
});

test("document links include preamble", async ({ s }) => {
    await s.compiled("nav.cpp");
    const links = (await s.request("textDocument/documentLink", "nav.cpp")) as
        | proto.DocumentLink[]
        | null;
    const targets = (links ?? []).map((link) => link.target ?? "");
    expect(targets.some((target) => target.includes("nav.h"))).toBe(true);
    expect(targets.some((target) => target.includes("nav_late.h"))).toBe(true);
});
