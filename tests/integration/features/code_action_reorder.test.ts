/// Reordering definitions on the server path: the reordered file is the
/// expected text and still compiles.

import type * as proto from "vscode-languageserver-protocol";
import type { CliceClient } from "@clice/tools/client";
import { applyTextEdits, editsFor } from "@clice/tools/client/edits";
import { expect, test, type SessionFactory } from "../fixtures.ts";

const TITLE = "Reorder definitions of 'S' by declaration order";

/// Opens `main.cpp` of a workspace holding `files` and applies the reorder
/// offered at `position`: the recompiled buffer, undefined when the action
/// is not offered.
async function reorder(
    session: SessionFactory,
    files: Record<string, string>,
    position: proto.Position,
): Promise<{ client: CliceClient; uri: string; text: string | undefined }> {
    const workspace = session.tmpdir();
    for (const [name, content] of Object.entries(files)) {
        workspace.write(name, content);
    }
    workspace.writeCDB(["main.cpp"]);
    const client = await session.spawn(workspace).initialize(workspace);
    const [uri, text] = await client.openAndWait("main.cpp");
    const action = ((await client.codeActions(uri, { start: position, end: position })) ?? []).find(
        (item): item is proto.CodeAction => "title" in item && item.title === TITLE,
    );
    if (action === undefined) {
        return { client, uri, text: undefined };
    }
    const applied = applyTextEdits(text, editsFor(action, uri));
    client.change(uri, 1, applied);
    await client.waitForRecompile(uri);
    return { client, uri, text: applied };
}

const STRUCT_NAME = { line: 0, character: 7 };

test("last line without newline", async ({ session }) => {
    const { client, uri, text } = await reorder(
        session,
        {
            "main.cpp":
                "struct S {\n  void a();\n  void b();\n};\n\nvoid S::b() {}\nvoid S::a() {}  // last",
        },
        STRUCT_NAME,
    );
    expect(text).toBe(
        "struct S {\n  void a();\n  void b();\n};\n\nvoid S::a() {}  // last\nvoid S::b() {}\n",
    );
    client.assertCleanCompile(uri);
});

test("definitions stay after what they use", async ({ session }) => {
    const { client, uri, text } = await reorder(
        session,
        {
            "main.cpp":
                "struct S {\n  int a();\n  int b();\n  int c();\n  int d();\n};\n\n" +
                "int S::d() { return 4; }\n\n" +
                "static int counter = 0;\n\n" +
                "int S::c() { return counter; }\n\n" +
                "#define LIMIT 2\n\n" +
                "int S::b() { return LIMIT; }\n\n" +
                "int S::a() { return 1; }\n",
        },
        STRUCT_NAME,
    );
    expect(text).toBe(
        "struct S {\n  int a();\n  int b();\n  int c();\n  int d();\n};\n\n" +
            "int S::a() { return 1; }\n\n" +
            "static int counter = 0;\n\n" +
            "int S::c() { return counter; }\n\n" +
            "#define LIMIT 2\n\n" +
            "int S::b() { return LIMIT; }\n\n" +
            "int S::d() { return 4; }\n",
    );
    client.assertCleanCompile(uri);
});

test("deduced return types keep their order", async ({ session }) => {
    const { text } = await reorder(
        session,
        {
            "main.cpp":
                "struct S {\n  int b();\n  auto a();\n};\n\n" +
                "auto S::a() { return 1; }\n\n" +
                "int S::b() { return a(); }\n",
        },
        STRUCT_NAME,
    );
    expect(text).toBeUndefined();
});

test("attribute lines move with definitions", async ({ session }) => {
    const { client, uri, text } = await reorder(
        session,
        {
            "main.cpp":
                "struct S {\n  void a();\n  void b();\n};\n\n" +
                "void S::b() {}\n\n" +
                "[[deprecated]]\nvoid S::a() {}\n",
        },
        STRUCT_NAME,
    );
    expect(text).toBe(
        "struct S {\n  void a();\n  void b();\n};\n\n" +
            "[[deprecated]]\nvoid S::a() {}\n\n" +
            "void S::b() {}\n",
    );
    client.assertCleanCompile(uri);
});

test("preamble conditionals bound moves", async ({ session }) => {
    const { client, uri, text } = await reorder(
        session,
        {
            "s.h": "#pragma once\nstruct S {\n  void a();\n  void b();\n  void c();\n};\n",
            "main.cpp":
                '#include "s.h"\n#if !defined(FEATURE_OFF)\n' +
                "void S::c() {}\nvoid S::b() {}\n#endif\n\nvoid S::a() {}\n",
        },
        { line: 2, character: 8 },
    );
    expect(text).toBe(
        '#include "s.h"\n#if !defined(FEATURE_OFF)\n' +
            "void S::b() {}\nvoid S::c() {}\n#endif\n\nvoid S::a() {}\n",
    );
    client.assertCleanCompile(uri);
});
