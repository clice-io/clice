/// Reordering definitions on the server path: the reordered file is the
/// expected text and still compiles.

import type { Serve } from "@clice/tools/actions";
import { actionsOf } from "@clice/tools/client/edits";
import type * as proto from "vscode-languageserver-protocol";
import { at, expect, serve, type Loc } from "../../fixtures.ts";

const TITLE = "Reorder definitions of 'S' by declaration order";

const MANIFEST = { cxx: ["-std=c++17"], units: { "main.cpp": [] } };

/// Applies the reorder offered at `loc` to main.cpp: the recompiled
/// buffer, undefined when the action is not offered.
async function reorder(
    s: Serve,
    loc: Loc,
): Promise<{ text: string; diagnostics: proto.Diagnostic[] } | undefined> {
    await s.compiled("main.cpp");
    const action = actionsOf(await s.codeActions(loc)).find((item) => item.title === TITLE);
    if (action === undefined) {
        return undefined;
    }
    const texts = await s.apply(action.edit ?? {});
    return { text: texts["main.cpp"]!, diagnostics: await s.compiled("main.cpp") };
}

const STRUCT_NAME = at("main.cpp", "struct |S {");

serve.files(
    {
        "main.cpp":
            "struct S {\n  void a();\n  void b();\n};\n\nvoid S::b() {}\nvoid S::a() {}  // last",
    },
    { manifest: MANIFEST },
)("last line without newline", async ({ s }) => {
    const result = await reorder(s, STRUCT_NAME);
    expect(result?.text).toBe(
        "struct S {\n  void a();\n  void b();\n};\n\nvoid S::a() {}  // last\nvoid S::b() {}\n",
    );
    expect(result?.diagnostics).toEqual([]);
});

serve.files(
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
    { manifest: MANIFEST },
)("definitions stay after what they use", async ({ s }) => {
    const result = await reorder(s, STRUCT_NAME);
    expect(result?.text).toBe(
        "struct S {\n  int a();\n  int b();\n  int c();\n  int d();\n};\n\n" +
            "int S::a() { return 1; }\n\n" +
            "static int counter = 0;\n\n" +
            "int S::c() { return counter; }\n\n" +
            "#define LIMIT 2\n\n" +
            "int S::b() { return LIMIT; }\n\n" +
            "int S::d() { return 4; }\n",
    );
    expect(result?.diagnostics).toEqual([]);
});

serve.files(
    {
        "main.cpp":
            "struct S {\n  int b();\n  auto a();\n};\n\n" +
            "auto S::a() { return 1; }\n\n" +
            "int S::b() { return a(); }\n",
    },
    { manifest: MANIFEST },
)("deduced return types keep their order", async ({ s }) => {
    expect(await reorder(s, STRUCT_NAME)).toBeUndefined();
});

serve.files(
    {
        "main.cpp":
            "struct S {\n  void a();\n  void b();\n};\n\n" +
            "void S::b() {}\n\n" +
            "[[deprecated]]\nvoid S::a() {}\n",
    },
    { manifest: MANIFEST },
)("attribute lines move with definitions", async ({ s }) => {
    const result = await reorder(s, STRUCT_NAME);
    expect(result?.text).toBe(
        "struct S {\n  void a();\n  void b();\n};\n\n" +
            "[[deprecated]]\nvoid S::a() {}\n\n" +
            "void S::b() {}\n",
    );
    expect(result?.diagnostics).toEqual([]);
});

serve.files(
    {
        "s.h": "#pragma once\nstruct S {\n  void a();\n  void b();\n  void c();\n};\n",
        "main.cpp":
            '#include "s.h"\n#if !defined(FEATURE_OFF)\n' +
            "void S::c() {}\nvoid S::b() {}\n#endif\n\nvoid S::a() {}\n",
    },
    { manifest: MANIFEST },
)("preamble conditionals bound moves", async ({ s }) => {
    const result = await reorder(s, at("main.cpp", "void S::|c()"));
    expect(result?.text).toBe(
        '#include "s.h"\n#if !defined(FEATURE_OFF)\n' +
            "void S::b() {}\nvoid S::c() {}\n#endif\n\nvoid S::a() {}\n",
    );
    expect(result?.diagnostics).toEqual([]);
});
