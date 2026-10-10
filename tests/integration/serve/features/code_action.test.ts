/// Behavioral code action tests: the snap suite pins what each action
/// renders, these pin the reply's contract — versioned edits a client can
/// apply, the kind filter, the index-backed actions that only exist with a
/// project index — and that the code an action writes compiles.

import * as fs from "node:fs";
import type { Serve } from "@clice/tools/actions";
import { actionsOf, editsFor } from "@clice/tools/client/edits";
import { at, expect, serve } from "../../fixtures.ts";

const LLVM_STYLE = "BasedOnStyle: LLVM\n";

function cxx17(...units: string[]) {
    return { cxx: ["-std=c++17"], units: Object.fromEntries(units.map((unit) => [unit, []])) };
}

/// The index answers `name` with a symbol of `file`.
async function expectIndexed(s: Serve, name: string, file: string): Promise<void> {
    await s.indexed();
    const symbols = (await s.workspaceSymbols(name)) ?? [];
    expect(
        symbols.some((symbol) => "uri" in symbol.location && symbol.location.uri.endsWith(file)),
        `${name} indexed from ${file}`,
    ).toBe(true);
}

serve.files(
    { ".clang-format": LLVM_STYLE, "main.cpp": "struct S {\n  int f(int x = 3) const;\n};\n" },
    { manifest: cxx17("main.cpp") },
)("edits apply to the buffer they were computed for", async ({ s }) => {
    await s.compiled("main.cpp");
    const actions = actionsOf(await s.codeActions(at("main.cpp", "int |f(")));
    const outline = actions.find((action) => action.title === "Define 'S::f' out of line");
    expect(outline).toBeDefined();
    expect(outline!.kind).toBe("refactor.rewrite.define.outOfLine");
    // Documents open at version 0; an edit stamps that version so a client
    // refuses it once the buffer moved on.
    const change = outline!.edit!.documentChanges![0]!;
    expect("textDocument" in change && change.textDocument.version).toBe(0);

    expect((await s.apply(outline!.edit!))["main.cpp"]).toBe(
        "struct S {\n  int f(int x = 3) const;\n};\n\nint S::f(int x) const {}\n",
    );
    s.close("main.cpp");
});

serve.files({ "main.cpp": "struct S {\n  int f();\n};\n" }, { manifest: cxx17("main.cpp") })(
    "only filters by kind",
    async ({ s }) => {
        await s.compiled("main.cpp");
        const loc = at("main.cpp", "int |f()");

        const only = async (kind: string) =>
            actionsOf(await s.codeActions(loc, { only: [kind] })).map((action) => action.title);

        const all = actionsOf(await s.codeActions(loc)).map((action) => action.title);
        expect(all).toEqual(["Define 'f' inline", "Define 'S::f' out of line"]);
        expect(await only("quickfix")).toEqual([]);
        expect(await only("refactor")).toEqual(all);
        expect(await only("refactor.rewrite.define")).toEqual(all);
        expect(await only("refactor.rewrite.define.outOfLine")).toEqual([
            "Define 'S::f' out of line",
        ]);
        expect(await only("refactor.rewrite.def")).toEqual([]);
        expect(await only("refactor.rewrite.populateSwitch")).toEqual([]);
        s.close("main.cpp");
    },
);

serve.files(
    {
        "widget.h": "#pragma once\nstruct Widget {\n  void a();\n  void b();\n  void c();\n};\n",
        "main.cpp": '#include "widget.h"\nvoid Widget::a() {}\n',
        "other.cpp": '#include "widget.h"\nvoid Widget::b() {}\n',
    },
    { config: { project: { enable_indexing: true } }, manifest: cxx17("main.cpp", "other.cpp") },
)("definitions vetted and placed through the index", async ({ s }) => {
    await s.compiled("main.cpp");
    // The open session already knows the declaration of b; the vetting
    // needs the definition, which only other.cpp's indexing provides.
    await expectIndexed(s, "b", "/other.cpp");
    await s.compiled("widget.h");

    const actions = actionsOf(await s.codeActions(at("widget.h", "struct |Widget")));
    const host = actions.find(
        (action) => action.title === "Define missing members of 'Widget' in main.cpp",
    );
    expect(host).toBeDefined();
    // b is defined in other.cpp, which this TU never sees: the index
    // drops it, leaving c, placed after main.cpp's definition of a.
    const uri = s.uri("main.cpp");
    const change = host!.edit!.documentChanges![0]!;
    expect("textDocument" in change && change.textDocument.uri).toBe(uri);
    const [edit] = editsFor(host!, uri);
    expect(edit!.range.start).toEqual({ line: 1, character: 19 });
    expect(edit!.newText).toBe("\n\nvoid Widget::c() {\n}\n");
    s.close("widget.h");
    s.close("main.cpp");
});

// A symlink needs privileges on Windows.
const linked = serve
    .files(
        {
            ".clang-format": LLVM_STYLE,
            "vendor/.clang-format": "BasedOnStyle: LLVM\nAllowShortFunctionsOnASingleLine: None\n",
            "widget.h": "#pragma once\nstruct Widget {\n  void a();\n};\n",
            "vendor/real/main.cpp": '#include "widget.h"\nint main() { return 0; }\n',
        },
        {
            manifest: { cxx: ["-std=c++17"], units: { "src/main.cpp": ["-I${workspace}"] } },
            setup: (workspace) => {
                fs.symlinkSync(workspace.path("vendor/real"), workspace.path("src"));
            },
        },
    )
    .skipIf(process.platform === "win32");

linked("closed host formats by the database's name", async ({ s }) => {
    await s.compiled("widget.h");

    const actions = actionsOf(await s.codeActions(at("widget.h", "struct |Widget")));
    const host = actions.find(
        (action) => action.title === "Define missing members of 'Widget' in main.cpp",
    );
    expect(host).toBeDefined();
    const [edit] = editsFor(host!, s.uri("vendor/real/main.cpp"));
    expect(edit!.newText).toBe("\nvoid Widget::a() {}\n");
    s.close("widget.h");
});

serve.files(
    { "main.cpp": "struct S {\n  int f();\n};\n" },
    { manifest: cxx17("main.cpp"), launch: { capabilities: {} } },
)("plain changes for a client without versioned edits", async ({ s }) => {
    await s.compiled("main.cpp");
    const actions = actionsOf(await s.codeActions(at("main.cpp", "int |f()")));
    expect(actions.length).toBeGreaterThan(0);
    for (const action of actions) {
        expect(action.edit!.documentChanges).toBeUndefined();
        expect(Object.keys(action.edit!.changes!)).toEqual([s.uri("main.cpp")]);
    }
    s.close("main.cpp");
});

serve.files(
    {
        "first/util.h": "#pragma once\nint unrelated();\n",
        "second/util.h": "#pragma once\nint shadowed();\n",
        "other.cpp": '#include "second/util.h"\nint shadowed() { return 1; }\n',
        "main.cpp": "int main() {\n  return shadowed();\n}\n",
    },
    {
        config: { project: { enable_indexing: true } },
        manifest: {
            cxx: ["-std=c++17", "-Ifirst", "-Isecond"],
            units: { "main.cpp": [], "other.cpp": [] },
        },
    },
)("include spelling resolves to the declaring header", async ({ s }) => {
    await s.compiled("main.cpp");
    await expectIndexed(s, "shadowed", "/other.cpp");

    // "util.h" would find first/util.h through -Ifirst; the spelling must
    // resolve to the header that declares the name.
    const actions = actionsOf(await s.codeActions(at("main.cpp", "shadowed()")));
    const titles = actions.map((action) => action.title);
    expect(titles).toContain('Add #include "second/util.h"');
    expect(titles).not.toContain('Add #include "util.h"');
    s.close("main.cpp");
});

serve.files(
    {
        ".clang-format": LLVM_STYLE,
        "main.cpp": [
            "struct Handle {",
            "  Handle() = default;",
            "  Handle(Handle &&) = default;",
            "};",
            "",
            "struct Owner {",
            "  Handle handle;",
            "  int &&pending;",
            "  int count;",
            "};",
            "",
            "Owner make(int &&n) { return Owner(Handle(), static_cast<int &&>(n), 1); }",
            "",
        ].join("\n"),
        "library.cpp": [
            "#include <memory>",
            "#include <string>",
            "",
            "struct Node {",
            "  std::unique_ptr<Node> next;",
            "  std::string name;",
            "};",
            "",
        ].join("\n"),
    },
    { manifest: cxx17("main.cpp", "library.cpp") },
)("memberwise constructors move what only moves", async ({ s }) => {
    await s.compiled("main.cpp");
    const owner = await s.applyAction(
        at("main.cpp", "Owner {"),
        "Generate a memberwise constructor for 'Owner'",
    );
    expect(owner.diagnostics).toEqual([]);
    expect(owner.text.startsWith("#include <utility>\n")).toBe(true);
    expect(owner.text.replace(/\s+/g, " ")).toContain(
        "Owner(Handle handle, int &&pending, int count) : handle(std::move(handle)), pending(std::move(pending)), count(count) {}",
    );

    await s.compiled("library.cpp");
    const node = await s.applyAction(
        at("library.cpp", "Node {"),
        "Generate a memberwise constructor for 'Node'",
    );
    expect(node.diagnostics).toEqual([]);
    expect(node.text).not.toContain("<utility>");
    expect(node.text.replace(/\s+/g, " ")).toContain(
        "Node(std::unique_ptr<Node> next, const std::string &name) : next(std::move(next)), name(name) {}",
    );
    s.close("main.cpp");
    s.close("library.cpp");
});

serve.files(
    {
        ".clang-format": LLVM_STYLE,
        "main.cpp": [
            "enum class Wide : __int128 { Low = 0, High = (__int128)1 << 64, Mid = 5 };",
            "",
            "int level(Wide wide) {",
            "  switch (wide) {",
            "  case Wide::Low:",
            "    return 0;",
            "  }",
            "  return 1;",
            "}",
            "",
            "enum class Shape { Circle, Square, Triangle };",
            "",
            "constexpr int sides(Shape shape) {",
            "  int extra = 0;",
            "  switch (shape) {",
            "  case Shape::Circle:",
            "    extra = 1;",
            "    [[fallthrough]];",
            "  case Shape::Square:",
            "    int count = 4;",
            "    return count + extra;",
            "  }",
            "  return 3;",
            "}",
            "",
            "static_assert(sides(Shape::Circle) == 5);",
            "static_assert(sides(Shape::Triangle) == 3);",
            "",
        ].join("\n"),
    },
    { manifest: cxx17("main.cpp") },
)("missing enum cases compile", async ({ s }) => {
    await s.compiled("main.cpp");
    // A value past 64 bits is its own enumerator, not a truncated Low;
    // the Circle section must still fall through into Square.
    await s.applyAction(at("main.cpp", "switch (wide)"), "Add 2 missing enum cases to switch");
    const shape = await s.applyAction(
        at("main.cpp", "switch (shape)"),
        "Add 1 missing enum case to switch",
    );
    expect(shape.diagnostics).toEqual([]);
    expect(shape.text).toContain(
        "  switch (shape) {\n  case Shape::Triangle:\n    break;\n  case Shape::Circle:\n",
    );
    s.close("main.cpp");
});

serve.files(
    {
        ".clang-format": "DisableFormat: true\n",
        "main.cpp": [
            "#define NEG -",
            "#define DEREF(p) *p",
            "#define PICK(c) (c ? 1 : ::fallback())",
            "#define NOTHING",
            "",
            "constexpr int fallback() { return 2; }",
            "constexpr int value = 4;",
            "constexpr const int* pointer = &value;",
            "",
            "static_assert(NEG-value == 4);",
            "static_assert(12/DEREF(pointer) == 3);",
            "static_assert(PICK(false) == 2);",
            "static_assert(12/NOTHING*pointer == 3);",
            "",
        ].join("\n"),
    },
    { manifest: cxx17("main.cpp") },
)("expanded macros keep their tokens apart", async ({ s }) => {
    await s.clean("main.cpp");

    const expansions: [string, string][] = [
        ["NEG-value", "NEG"],
        ["DEREF(pointer)", "DEREF"],
        ["PICK(false)", "PICK"],
        ["NOTHING*pointer", "NOTHING"],
    ];
    let text = "";
    for (const [needle, name] of expansions) {
        const applied = await s.applyAction(at("main.cpp", needle), `Expand macro '${name}'`);
        expect(applied.diagnostics).toEqual([]);
        text = applied.text;
    }
    expect(text.slice(text.indexOf("static_assert"))).toBe(
        [
            "static_assert(- -value == 4);",
            "static_assert(12/ *pointer == 3);",
            "static_assert((false ? 1 : ::fallback()) == 2);",
            "static_assert(12/ *pointer == 3);",
            "",
        ].join("\n"),
    );
    s.close("main.cpp");
});
