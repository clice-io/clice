/// Integration tests for index-based LSP features: GoToDefinition,
/// FindReferences, CallHierarchy, TypeHierarchy, and WorkspaceSymbol.

import * as proto from "vscode-languageserver-protocol";
import type { Serve } from "@clice/tools/actions";
import { at, expect, serve, type Loc } from "../../fixtures.ts";

const tiny = serve("tiny");
const shapes = serve("shapes/headers");

/// `file` open and compiled, every unit indexed.
async function indexed(s: Serve, file: string): Promise<void> {
    await s.compiled(file);
    await s.indexed();
}

async function prepareCalls(s: Serve, loc: Loc): Promise<proto.CallHierarchyItem[]> {
    const items = await s.request<proto.CallHierarchyItem[] | null>(
        "textDocument/prepareCallHierarchy",
        loc,
    );
    expect(
        items && items.length > 0,
        `prepareCallHierarchy returned ${JSON.stringify(items)}`,
    ).toBe(true);
    return items!;
}

tiny("goto definition", async ({ s }) => {
    await indexed(s, "main.cpp");
    expect(s.show(await s.definition(at("main.cpp", "add(1, 2)")))).toBe(
        "main.cpp: int add(int lhs, int rhs) {",
    );
});

shapes("find references", async ({ s }) => {
    const registry = s.file("registry");
    await indexed(s, registry);
    // registered is defined once and used four times.
    expect(
        s
            .show(await s.references(at(s.file("registry"), "int |registered")))
            .split("\n")
            .sort(),
    ).toEqual([
        `${registry}: if(registered < registry_limit) {`,
        `${registry}: registered += 1;`,
        `${registry}: registered = 0;`,
        `${registry}: return registered;`,
        `${registry}: static int registered = 0;`,
    ]);
});

tiny("call hierarchy prepare", async ({ s }) => {
    await indexed(s, "main.cpp");
    const [item] = await prepareCalls(s, at("main.cpp", "add(int lhs, int rhs)"));
    expect(item!.name).toBe("add");
});

/// incomingCalls with an unresolvable item returns an error, not null.
tiny("call hierarchy bogus item", async ({ s }) => {
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

tiny("call hierarchy incoming", async ({ s }) => {
    await indexed(s, "main.cpp");
    const incoming = await s.incomingCalls(at("main.cpp", "add(int lhs, int rhs)"));
    expect(incoming, "incomingCalls returned None").not.toBeNull();
    expect(incoming!.map((call) => call.from.name)).toContain("main");
});

/// An item a client sends back without its data resolves at its name.
tiny("call hierarchy item without data", async ({ s }) => {
    await indexed(s, "main.cpp");
    const items = await prepareCalls(s, at("main.cpp", "add(int lhs, int rhs)"));
    expect(items.length).toBe(1);
    const stripped: proto.CallHierarchyItem = { ...items[0]! };
    delete stripped.data;
    const incoming = await s.client.callHierarchyIncoming(stripped);
    expect(incoming!.map((call) => call.from.name)).toContain("main");
});

tiny("call hierarchy outgoing", async ({ s }) => {
    await indexed(s, "main.cpp");
    const outgoing = await s.outgoingCalls(at("main.cpp", "main() {"));
    expect(outgoing, "outgoingCalls returned None").not.toBeNull();
    expect(outgoing!.map((call) => call.to.name)).toContain("add");
});

shapes("type hierarchy prepare", async ({ s }) => {
    await indexed(s, s.file("main"));
    const items = await s.request<proto.TypeHierarchyItem[] | null>(
        "textDocument/prepareTypeHierarchy",
        at(s.file("circle"), "class |Circle : public"),
    );
    expect(items?.[0]?.name, `prepareTypeHierarchy returned ${JSON.stringify(items)}`).toBe(
        "Circle",
    );
});

shapes("type hierarchy supertypes", async ({ s }) => {
    await indexed(s, s.file("main"));
    const supertypes = await s.supertypes(at(s.file("circle"), "class |Circle : public"));
    expect(supertypes, "supertypes returned None").not.toBeNull();
    expect(supertypes!.map((t) => t.name)).toContain("Shape");
});

shapes("type hierarchy subtypes", async ({ s }) => {
    await indexed(s, s.file("main"));
    const subtypes = await s.subtypes(at(s.file("shape"), "class SHAPES_API |Shape {"));
    expect(subtypes, "subtypes returned None").not.toBeNull();
    const names = subtypes!.map((t) => t.name);
    expect(names).toContain("Circle");
    expect(names).toContain("Polygon");
});

tiny("workspace symbol", async ({ s }) => {
    await indexed(s, "main.cpp");
    const result = await s.workspaceSymbols("add");
    expect(result).not.toBeNull();
    expect(result!.map((symbol) => symbol.name)).toContain("add");
});

shapes("workspace symbol class", async ({ s }) => {
    await indexed(s, s.file("main"));
    const result = await s.workspaceSymbols("Circle");
    expect(result).not.toBeNull();
    expect(result!.map((symbol) => symbol.name)).toContain("Circle");
});

/// 'area' is defined in the circle source and declared in its header, both
/// closed: the header's shard only exists while the source stays closed
/// (background indexing skips open files; recorded index gap).
shapes("goto declaration cross file", async ({ s }) => {
    await indexed(s, s.file("main"));
    const definition = at(s.file("circle_impl"), "area(const Circle& circle) {");
    const shown = s.show(await s.request("textDocument/declaration", definition)).split("\n");
    expect(shown).toContain(`${s.file("circle")}: double area(const Circle& circle);`);
    // The cursor stands on the definition site; it is not an answer.
    expect(shown).not.toContain(`${s.file("circle_impl")}: double area(const Circle& circle) {`);
});

/// Standing on a definition or declaration navigates to the other site.
shapes("goto definition alternate", async ({ s }) => {
    await indexed(s, s.file("main"));
    const definition = at(s.file("circle_impl"), "area(const Circle& circle) {");
    const declaration = at(s.file("circle"), "area(const Circle& circle);");
    expect(s.show(await s.definition(definition))).toBe(
        `${s.file("circle")}: double area(const Circle& circle);`,
    );
    expect(s.show(await s.definition(declaration))).toBe(
        `${s.file("circle_impl")}: double area(const Circle& circle) {`,
    );
});

/// A closed file answers from the index alone: no symbol under the cursor
/// is an empty answer, not an error.
shapes("goto definition closed blank", async ({ s }) => {
    await indexed(s, s.file("main"));
    const blank = at(s.file("circle_impl"), '"shapes/detail/math.h"\n|\nnamespace shapes {');
    expect((await s.definition(blank)) ?? []).toEqual([]);
});

/// A symbol with no definition anywhere navigates to its declaration
/// instead of returning empty.
shapes("goto definition declaration only", async ({ s }) => {
    await s.indexed();
    expect(s.show(await s.definition(at(s.file("demo"), "registry_capacity()")))).toBe(
        "include/shapes/registry.h: int registry_capacity();",
    );
});

/// An inline-defined symbol has nowhere else to point: the definition
/// site stays the answer (clients render self-navigation as a peek).
tiny("goto definition inline definition", async ({ s }) => {
    await indexed(s, "main.cpp");
    expect(s.show(await s.definition(at("main.cpp", "add(int lhs, int rhs)")))).toBe(
        "main.cpp: int add(int lhs, int rhs) {",
    );
});

tiny("goto declaration inline definition", async ({ s }) => {
    await indexed(s, "main.cpp");
    // 'add' is defined inline with no separate declaration; declaration
    // must still navigate to the definition, not return empty.
    const definition = at("main.cpp", "add(int lhs, int rhs)");
    expect(s.show(await s.request("textDocument/declaration", definition)).split("\n")).toContain(
        "main.cpp: int add(int lhs, int rhs) {",
    );
});

shapes("goto implementation", async ({ s }) => {
    await indexed(s, s.file("main"));
    // Shape::name is overridden by Circle, the Polygon template and its
    // Polygon<3> specialization.
    const sites =
        (await s.request<proto.Location[] | null>(
            "textDocument/implementation",
            at(s.file("shape"), "name() const = 0"),
        )) ?? [];
    expect(s.show(sites).split("\n").sort()).toEqual(
        [
            `${s.file("circle_impl")}: const char* Circle::name() const {`,
            `${s.file("polygon")}: const char* name() const override {`,
            `${s.file("polygon_impl")}: const char* Polygon<3>::name() const {`,
        ].sort(),
    );
});

shapes("goto type definition", async ({ s }) => {
    await indexed(s, s.file("main"));
    // c has type Circle, defined in the circle header.
    expect(
        s
            .show(await s.request("textDocument/typeDefinition", at(s.file("test"), "c(1.0)")))
            .split("\n"),
    ).toContain(`${s.file("circle")}: class Circle : public Shape {`);
});

shapes("references declaration flag", async ({ s }) => {
    await indexed(s, s.file("main"));
    // 'area': declaration in the circle header, definition and a call in
    // its source, calls in the program and the test program.
    const definition = at(s.file("circle_impl"), "area(const Circle& circle) {");
    const withDecl = s
        .show(await s.references(definition))
        .split("\n")
        .sort();
    const withoutDecl = s
        .show(
            await s.request("textDocument/references", definition, {
                context: { includeDeclaration: false },
            }),
        )
        .split("\n")
        .sort();
    const calls = [
        `${s.file("main")}: double total = shapes::area(c) + triangle.measure() + shapes_circle_area(1.0);`,
        `${s.file("circle_impl")}: return area(*this);`,
        `${s.file("test")}: return shapes::area(c) > shapes_precision ? 0 : 1;`,
    ];
    expect(withDecl).toEqual(
        [
            ...calls,
            `${s.file("circle")}: double area(const Circle& circle);`,
            `${s.file("circle_impl")}: double area(const Circle& circle) {`,
        ].sort(),
    );
    expect(withoutDecl).toEqual(calls.sort());
});

shapes("goto implementation pure virtual", async ({ s }) => {
    await indexed(s, s.file("main"));
    // Shape::measure is pure virtual; only the direct overrides are
    // returned (UnitCircle::measure belongs to Circle::measure).
    expect(
        s
            .show(
                await s.request(
                    "textDocument/implementation",
                    at(s.file("shape"), "measure() const = 0"),
                ),
            )
            .split("\n")
            .sort(),
    ).toEqual(
        [
            `${s.file("circle_impl")}: double Circle::measure() const {`,
            `${s.file("polygon")}: double measure() const override {`,
            `${s.file("polygon_impl")}: double Polygon<3>::measure() const {`,
        ].sort(),
    );
});

shapes("goto implementation chain", async ({ s }) => {
    await indexed(s, s.file("main"));
    // Intermediate override navigates to its own overriders.
    expect(
        s.show(
            await s.request(
                "textDocument/implementation",
                at(s.file("circle"), "double |measure() const override;\n\n    const char*"),
            ),
        ),
    ).toBe(`${s.file("measure")}: double UnitCircle::measure() const {`);
});

shapes("goto type definition return value", async ({ s }) => {
    await s.indexed();
    // Known index gap: functions carry no TypeDefinition relation for their
    // return type, so this currently yields no results.
    const result = await s.request<proto.Location[] | null>(
        "textDocument/typeDefinition",
        at(s.file("circle"), "Circle |make_circle("),
    );
    expect(result).not.toBeNull();
    expect(result!.length).toBe(0);
});

shapes("goto declaration forward declared", async ({ s }) => {
    await s.indexed();
    // 'Shape' in demo: the registry header's forward declaration and the
    // definition are both listed.
    const shown = s
        .show(
            await s.request(
                "textDocument/declaration",
                at(s.file("demo"), "const shapes::|Shape& shape"),
            ),
        )
        .split("\n");
    expect(shown).toContain("include/shapes/registry.h: class Shape;");
    expect(shown).toContain(`${s.file("shape")}: class SHAPES_API Shape {`);
});

tiny("navigation empty open document", async ({ s }) => {
    await indexed(s, "main.cpp");
    // 'add' has no implementations: open documents get [] back, not an error.
    const result = await s.request<proto.Location[] | null>(
        "textDocument/implementation",
        at("main.cpp", "add(int lhs, int rhs)"),
    );
    expect(result).not.toBeNull();
    expect(result!.length).toBe(0);
});

shapes("navigation closed document empty", async ({ s }) => {
    await indexed(s, s.file("main"));
    // Index-only navigation serves closed documents; an empty result is a
    // real answer, not an error.
    const result = await s.request<proto.Location[] | null>(
        "textDocument/implementation",
        at(s.file("circle_impl"), "area(const Circle& circle) {"),
    );
    expect(result).not.toBeNull();
    expect(result!.length).toBe(0);
});

shapes("definition on include", async ({ s }) => {
    const demo = s.file("demo");
    await s.compiled(demo);

    // Preamble include: served master-side from the PCH's links.
    expect(s.show(await s.definition(at(s.file("demo"), '"shapes/cir|cle.h"')))).toBe(
        `${s.file("circle")}: #pragma once`,
    );

    // Non-preamble include: served by the worker's AST.
    expect(s.show(await s.definition(at(s.file("demo"), '"shapes/detail/ma|th.h"')))).toBe(
        `${s.file("detail")}: #pragma once`,
    );
});

shapes("document links include preamble", async ({ s }) => {
    const demo = s.file("demo");
    await s.compiled(demo);
    const links = await s.request<proto.DocumentLink[] | null>("textDocument/documentLink", demo);
    const targets = (links ?? []).map((link) => s.relative(link.target ?? ""));
    expect(targets).toContain(s.file("circle"));
    expect(targets).toContain(s.file("detail"));
});
