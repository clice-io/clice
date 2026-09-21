/// Behavioral code action tests: the snap suite pins what each action
/// renders, these pin the reply's contract — versioned edits a client can
/// apply, the kind filter, and the index-backed actions that only exist
/// with a project index.

import type * as proto from "vscode-languageserver-protocol";
import { SETTLE_TIME, sleep } from "@clice/tools/client";
import { applyTextEdits, editsFor } from "@clice/tools/client/edits";
import { expect, test } from "../fixtures.ts";

function actionsOf(reply: (proto.Command | proto.CodeAction)[] | null): proto.CodeAction[] {
    return (reply ?? []).filter((item): item is proto.CodeAction => "title" in item);
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
