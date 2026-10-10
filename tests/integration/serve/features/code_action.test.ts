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

serve("shapes/headers", { files: { ".clang-format": LLVM_STYLE } })(
    "edits apply to the buffer they were computed for",
    async ({ s }) => {
        await s.compiled(s.file("circle"));
        const actions = actionsOf(await s.codeActions(at(s.file("circle"), "double |diameter()")));
        const outline = actions.find(
            (action) => action.title === "Define 'Circle::diameter' out of line",
        );
        expect(outline).toBeDefined();
        expect(outline!.kind).toBe("refactor.rewrite.define.outOfLine");
        // Documents open at version 0; an edit stamps that version so a client
        // refuses it once the buffer moved on.
        const change = outline!.edit!.documentChanges![0]!;
        expect("textDocument" in change && change.textDocument.version).toBe(0);

        const before = s.disk.read(s.file("circle"));
        expect((await s.apply(outline!.edit!))[s.file("circle")]).toBe(
            before.replace(
                "};\n\nclass UnitCircle",
                "};\n\ninline double Circle::diameter() const {}\n\nclass UnitCircle",
            ),
        );
        s.close(s.file("circle"));
    },
);

serve("shapes/headers")("only filters by kind", async ({ s }) => {
    await s.compiled(s.file("circle"));
    const loc = at(s.file("circle"), "double |diameter()");

    const only = async (kind: string) =>
        actionsOf(await s.codeActions(loc, { only: [kind] })).map((action) => action.title);

    const all = actionsOf(await s.codeActions(loc)).map((action) => action.title);
    expect(all).toEqual([
        "Define 'diameter' inline",
        "Define 'Circle::diameter' out of line",
        "Define 'shapes::Circle::diameter' in circle.cpp",
    ]);
    expect(await only("quickfix")).toEqual([]);
    expect(await only("refactor")).toEqual(all);
    expect(await only("refactor.rewrite.define")).toEqual(all);
    expect(await only("refactor.rewrite.define.outOfLine")).toEqual([
        "Define 'Circle::diameter' out of line",
        "Define 'shapes::Circle::diameter' in circle.cpp",
    ]);
    expect(await only("refactor.rewrite.def")).toEqual([]);
    expect(await only("refactor.rewrite.populateSwitch")).toEqual([]);
    s.close(s.file("circle"));
});

serve("shapes/headers", { config: { project: { enable_indexing: true } } })(
    "definitions vetted and placed through the index",
    async ({ s }) => {
        await s.compiled(s.file("circle_impl"));
        // The open session already knows the declaration of circumference;
        // the vetting needs the definition, which only measure.cpp's
        // indexing provides.
        await expectIndexed(s, "circumference", "/src/measure.cpp");
        await s.compiled(s.file("circle"));

        const actions = actionsOf(await s.codeActions(at(s.file("circle"), "class |Circle :")));
        const host = actions.find(
            (action) => action.title === "Define missing members of 'Circle' in circle.cpp",
        );
        expect(host).toBeDefined();
        // circumference is defined in measure.cpp, which this TU never sees:
        // the index drops it with the members circle.cpp defines, leaving
        // diameter, placed after circle.cpp's last definition of a member.
        const uri = s.uri(s.file("circle_impl"));
        const change = host!.edit!.documentChanges![0]!;
        expect("textDocument" in change && change.textDocument.uri).toBe(uri);
        const [edit] = editsFor(host!, uri);
        expect(edit!.range.start).toEqual(
            s.range(at(s.file("circle_impl"), 'return "circle";\n}')).end,
        );
        expect(edit!.newText).toBe("\n\ndouble shapes::Circle::diameter() const {\n}\n");
        s.close(s.file("circle"));
        s.close(s.file("circle_impl"));
    },
);

// A symlink needs privileges on Windows.
const linked = serve.files(
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
);

linked.skipIf(process.platform === "win32")(
    "closed host formats by the database's name",
    async ({ s }) => {
        await s.compiled("widget.h");

        const actions = actionsOf(await s.codeActions(at("widget.h", "struct |Widget")));
        const host = actions.find(
            (action) => action.title === "Define missing members of 'Widget' in main.cpp",
        );
        expect(host).toBeDefined();
        const [edit] = editsFor(host!, s.uri("vendor/real/main.cpp"));
        expect(edit!.newText).toBe("\nvoid Widget::a() {}\n");
        s.close("widget.h");
    },
);

serve("shapes/headers", { launch: { capabilities: {} } })(
    "plain changes for a client without versioned edits",
    async ({ s }) => {
        await s.compiled(s.file("circle"));
        const actions = actionsOf(await s.codeActions(at(s.file("circle"), "double |diameter()")));
        expect(actions.length).toBeGreaterThan(0);
        for (const action of actions) {
            expect(action.edit!.documentChanges).toBeUndefined();
            const target = action.title.endsWith(" in circle.cpp") ? "circle_impl" : "circle";
            expect(Object.keys(action.edit!.changes!)).toEqual([s.uri(s.file(target))]);
        }
        s.close(s.file("circle"));
    },
);

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
