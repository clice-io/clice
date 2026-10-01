/// Behavioral code action tests: the snap suite pins what each action
/// renders, these pin the reply's contract — versioned edits a client can
/// apply, the kind filter, the index-backed actions that only exist with a
/// project index — and that the code an action writes compiles.

import * as fs from "node:fs";
import type * as proto from "vscode-languageserver-protocol";
import { type CliceClient, SETTLE_TIME, sleep } from "@clice/tools/client";
import { applyTextEdits, editsFor } from "@clice/tools/client/edits";
import { expect, test } from "../fixtures.ts";

function actionsOf(reply: (proto.Command | proto.CodeAction)[] | null): proto.CodeAction[] {
    return (reply ?? []).filter((item): item is proto.CodeAction => "title" in item);
}

function positionOf(text: string, needle: string): proto.Position {
    const offset = text.indexOf(needle);
    expect(offset, needle).toBeGreaterThanOrEqual(0);
    const before = text.slice(0, offset);
    const line = before.split("\n").length - 1;
    return { line, character: offset - before.lastIndexOf("\n") - 1 };
}

/// Apply the action titled `title` offered at the start of `needle` to
/// the open buffer and recompile it under `version`: the text returned is
/// what the server's diagnostics now describe.
async function applyAction(
    client: CliceClient,
    uri: string,
    text: string,
    version: number,
    needle: string,
    title: string,
): Promise<string> {
    const position = positionOf(text, needle);
    const reply = await client.codeActions(uri, { start: position, end: position });
    const action = actionsOf(reply).find((candidate) => candidate.title === title);
    expect(action, title).toBeDefined();
    const edited = applyTextEdits(text, editsFor(action!, uri));
    client.change(uri, version, edited);
    await client.waitForRecompile(uri);
    return edited;
}

test("edits apply to the buffer they were computed for", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.write(".clang-format", "BasedOnStyle: LLVM\n");
    workspace.write("main.cpp", "struct S {\n  int f(int x = 3) const;\n};\n");
    workspace.writeCDB(["main.cpp"]);
    const client = await session.spawn(workspace).initialize(workspace);
    const [uri, text] = await client.openAndWait("main.cpp");

    const actions = actionsOf(
        await client.codeActions(uri, {
            start: { line: 1, character: 6 },
            end: { line: 1, character: 6 },
        }),
    );
    const outline = actions.find((action) => action.title === "Define 'S::f' out of line");
    expect(outline).toBeDefined();
    expect(outline!.kind).toBe("refactor.rewrite");
    // The harness opens documents at version 0; an edit stamps that
    // version so a client refuses it once the buffer moved on.
    const change = outline!.edit!.documentChanges![0]!;
    expect("textDocument" in change && change.textDocument.version).toBe(0);

    expect(applyTextEdits(text, editsFor(outline!, uri))).toBe(
        "struct S {\n  int f(int x = 3) const;\n};\n\nint S::f(int x) const {}\n",
    );
    client.close(uri);
});

test("only filters by kind", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.write("main.cpp", "struct S {\n  int f();\n};\n");
    workspace.writeCDB(["main.cpp"]);
    const client = await session.spawn(workspace).initialize(workspace);
    const [uri] = await client.openAndWait("main.cpp");
    const range = { start: { line: 1, character: 6 }, end: { line: 1, character: 6 } };

    const all = actionsOf(await client.codeActions(uri, range));
    expect(all.length).toBeGreaterThan(0);
    const quickfix = await client.sendRequest("textDocument/codeAction", {
        textDocument: { uri },
        range,
        context: { diagnostics: [], only: ["quickfix"] },
    });
    expect(actionsOf(quickfix as proto.CodeAction[]).length).toBe(0);
    const refactor = await client.sendRequest("textDocument/codeAction", {
        textDocument: { uri },
        range,
        context: { diagnostics: [], only: ["refactor"] },
    });
    expect(actionsOf(refactor as proto.CodeAction[]).length).toBe(all.length);
    client.close(uri);
});

test("definitions vetted and placed through the index", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.write(
        "widget.h",
        "#pragma once\nstruct Widget {\n  void a();\n  void b();\n  void c();\n};\n",
    );
    workspace.write("main.cpp", '#include "widget.h"\nvoid Widget::a() {}\n');
    workspace.write("other.cpp", '#include "widget.h"\nvoid Widget::b() {}\n');
    workspace.writeCDB(["main.cpp", "other.cpp"]);
    const client = await session
        .spawn(workspace)
        .initialize(workspace, { initializationOptions: { project: { enable_indexing: true } } });
    const [uri] = await client.openAndWait("main.cpp");
    // The open session already knows the declaration of b; the vetting
    // needs the definition, which only other.cpp's indexing provides.
    let indexed = false;
    for (let i = 0; i < 60 && !indexed; i++) {
        const symbols = (await client.workspaceSymbols("b")) ?? [];
        indexed = symbols.some((symbol) => symbol.location.uri.endsWith("/other.cpp"));
        if (!indexed) {
            await sleep(SETTLE_TIME);
        }
    }
    expect(indexed).toBe(true);
    const [header] = await client.openAndWait("widget.h");

    const actions = actionsOf(
        await client.codeActions(header, {
            start: { line: 1, character: 7 },
            end: { line: 1, character: 7 },
        }),
    );
    const host = actions.find(
        (action) => action.title === "Define missing members of 'Widget' in main.cpp",
    );
    expect(host).toBeDefined();
    // b is defined in other.cpp, which this TU never sees: the index
    // drops it, leaving c, placed after main.cpp's definition of a.
    const change = host!.edit!.documentChanges![0]!;
    expect("textDocument" in change && change.textDocument.uri).toBe(uri);
    const [edit] = editsFor(host!, uri);
    expect(edit!.range.start).toEqual({ line: 1, character: 19 });
    expect(edit!.newText).toBe("\n\nvoid Widget::c() {}\n");
    client.close(header);
    client.close(uri);
});

test.skipIf(process.platform === "win32")(
    "closed host formats by the database's name",
    async ({ session }) => {
        const workspace = session.tmpdir();
        workspace.write(".clang-format", "BasedOnStyle: LLVM\n");
        workspace.write(
            "vendor/.clang-format",
            "BasedOnStyle: LLVM\nAllowShortFunctionsOnASingleLine: None\n",
        );
        workspace.write("widget.h", "#pragma once\nstruct Widget {\n  void a();\n};\n");
        workspace.write("vendor/real/main.cpp", '#include "widget.h"\nint main() { return 0; }\n');
        fs.symlinkSync(workspace.path("vendor/real"), workspace.path("src"));
        workspace.writeCDB(["src/main.cpp"], { extraArgs: [`-I${workspace.root}`] });
        const client = await session.spawn(workspace).initialize(workspace);
        const [header] = await client.openAndWait("widget.h");

        const actions = actionsOf(
            await client.codeActions(header, {
                start: { line: 1, character: 7 },
                end: { line: 1, character: 7 },
            }),
        );
        const host = actions.find(
            (action) => action.title === "Define missing members of 'Widget' in main.cpp",
        );
        expect(host).toBeDefined();
        const [edit] = editsFor(host!, workspace.uri("vendor/real/main.cpp"));
        expect(edit!.newText).toBe("\nvoid Widget::a() {}\n");
        client.close(header);
    },
);

test("plain changes for a client without versioned edits", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.write("main.cpp", "struct S {\n  int f();\n};\n");
    workspace.writeCDB(["main.cpp"]);
    const client = await session.spawn(workspace).initialize(workspace, { capabilities: {} });
    const [uri] = await client.openAndWait("main.cpp");

    const actions = actionsOf(
        await client.codeActions(uri, {
            start: { line: 1, character: 6 },
            end: { line: 1, character: 6 },
        }),
    );
    expect(actions.length).toBeGreaterThan(0);
    for (const action of actions) {
        expect(action.edit!.documentChanges).toBeUndefined();
        expect(Object.keys(action.edit!.changes!)).toEqual([uri]);
    }
    client.close(uri);
});

test("include spelling resolves to the declaring header", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.write("first/util.h", "#pragma once\nint unrelated();\n");
    workspace.write("second/util.h", "#pragma once\nint shadowed();\n");
    workspace.write("other.cpp", '#include "second/util.h"\nint shadowed() { return 1; }\n');
    workspace.write("main.cpp", "int main() {\n  return shadowed();\n}\n");
    workspace.writeCDB(["main.cpp", "other.cpp"], { extraArgs: ["-Ifirst", "-Isecond"] });
    const client = await session
        .spawn(workspace)
        .initialize(workspace, { initializationOptions: { project: { enable_indexing: true } } });
    const [uri] = await client.openAndWait("main.cpp");
    let indexed = false;
    for (let i = 0; i < 60 && !indexed; i++) {
        const symbols = (await client.workspaceSymbols("shadowed")) ?? [];
        indexed = symbols.some((symbol) => symbol.location.uri.endsWith("/other.cpp"));
        if (!indexed) {
            await sleep(SETTLE_TIME);
        }
    }
    expect(indexed).toBe(true);

    // "util.h" would find first/util.h through -Ifirst; the spelling must
    // resolve to the header that declares the name.
    const actions = actionsOf(
        await client.codeActions(uri, {
            start: { line: 1, character: 9 },
            end: { line: 1, character: 9 },
        }),
    );
    const titles = actions.map((action) => action.title);
    expect(titles).toContain('Add #include "second/util.h"');
    expect(titles).not.toContain('Add #include "util.h"');
    client.close(uri);
});

test("memberwise constructors move what only moves", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.write(".clang-format", "BasedOnStyle: LLVM\n");
    workspace.write(
        "main.cpp",
        [
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
    );
    workspace.write(
        "library.cpp",
        [
            "#include <memory>",
            "#include <string>",
            "",
            "struct Node {",
            "  std::unique_ptr<Node> next;",
            "  std::string name;",
            "};",
            "",
        ].join("\n"),
    );
    workspace.writeCDB(["main.cpp", "library.cpp"]);
    const client = await session.spawn(workspace).initialize(workspace);

    const [main, text] = await client.openAndWait("main.cpp");
    const owner = await applyAction(
        client,
        main,
        text,
        1,
        "Owner {",
        "Generate a memberwise constructor for 'Owner'",
    );
    client.assertCleanCompile(main);
    expect(owner.startsWith("#include <utility>\n")).toBe(true);
    expect(owner.replace(/\s+/g, " ")).toContain(
        "Owner(Handle handle, int &&pending, int count) : handle(std::move(handle)), pending(std::move(pending)), count(count) {}",
    );

    const [library, source] = await client.openAndWait("library.cpp");
    const node = await applyAction(
        client,
        library,
        source,
        1,
        "Node {",
        "Generate a memberwise constructor for 'Node'",
    );
    client.assertCleanCompile(library);
    expect(node).not.toContain("<utility>");
    expect(node.replace(/\s+/g, " ")).toContain(
        "Node(std::unique_ptr<Node> next, const std::string &name) : next(std::move(next)), name(name) {}",
    );
    client.close(main);
    client.close(library);
});

test("missing enum cases compile", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.write(".clang-format", "BasedOnStyle: LLVM\n");
    workspace.write(
        "main.cpp",
        [
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
    );
    workspace.writeCDB(["main.cpp"]);
    const client = await session.spawn(workspace).initialize(workspace);
    const [uri, text] = await client.openAndWait("main.cpp");

    // A value past 64 bits is its own enumerator, not a truncated Low;
    // the Circle section must still fall through into Square.
    const wide = await applyAction(
        client,
        uri,
        text,
        1,
        "switch (wide)",
        "Add 2 missing enum cases to switch",
    );
    const shape = await applyAction(
        client,
        uri,
        wide,
        2,
        "switch (shape)",
        "Add 1 missing enum case to switch",
    );
    client.assertCleanCompile(uri);
    expect(shape).toContain(
        "  switch (shape) {\n  case Shape::Triangle:\n    break;\n  case Shape::Circle:\n",
    );
    client.close(uri);
});

test("expanded macros keep their tokens apart", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.write(".clang-format", "DisableFormat: true\n");
    workspace.write(
        "main.cpp",
        [
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
    );
    workspace.writeCDB(["main.cpp"]);
    const client = await session.spawn(workspace).initialize(workspace);
    const [uri, original] = await client.openAndWait("main.cpp");
    client.assertCleanCompile(uri);

    const expansions: [string, string][] = [
        ["NEG-value", "NEG"],
        ["DEREF(pointer)", "DEREF"],
        ["PICK(false)", "PICK"],
        ["NOTHING*pointer", "NOTHING"],
    ];
    let text = original;
    for (const [index, [needle, name]] of expansions.entries()) {
        text = await applyAction(client, uri, text, index + 1, needle, `Expand macro '${name}'`);
        client.assertCleanCompile(uri);
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
    client.close(uri);
});
