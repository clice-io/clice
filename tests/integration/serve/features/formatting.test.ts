import * as fs from "node:fs";
import type * as proto from "vscode-languageserver-protocol";
import type { Serve } from "@clice/tools/actions";
import { expect, serve } from "../../fixtures.ts";

const UNFORMATTED = "int    add(   int   a  ,  int   b  ) {\nreturn   a+b ;\n}\n";
const FORMATTED = "int add(int a, int b) { return a + b; }\n";

const OPTIONS = { options: { tabSize: 4, insertSpaces: true } };

function format(s: Serve, file: string): Promise<proto.TextEdit[] | null> {
    return s.request<proto.TextEdit[] | null>("textDocument/formatting", file, OPTIONS);
}

const test = serve.data("formatting");

test("format document", async ({ s }) => {
    await s.compiled("main.cpp");

    s.edit("main.cpp", { text: UNFORMATTED });
    const edits = await format(s, "main.cpp");

    expect(edits).not.toBeNull();
    expect(edits!.length).toBeGreaterThan(0);
    const texts = await s.apply({ changes: { [s.uri("main.cpp")]: edits! } });
    expect(texts["main.cpp"]).toBe(FORMATTED);

    s.close("main.cpp");
});

test("format range", async ({ s }) => {
    await s.compiled("main.cpp");

    s.edit("main.cpp", { text: UNFORMATTED });
    const edits = await s.request<proto.TextEdit[] | null>(
        "textDocument/rangeFormatting",
        "main.cpp",
        {
            range: { start: { line: 1, character: 0 }, end: { line: 2, character: 0 } },
            ...OPTIONS,
        },
    );

    expect(edits).not.toBeNull();
    expect(edits!.length).toBeGreaterThan(0);

    s.close("main.cpp");
});

test("format already formatted", async ({ s }) => {
    await s.compiled("main.cpp");

    s.edit("main.cpp", { text: FORMATTED });
    const edits = await format(s, "main.cpp");

    expect(edits).not.toBeNull();
    expect(edits!.length).toBe(0);

    s.close("main.cpp");
});

// Creating a symbolic link takes a privilege Windows users lack by default.
serve
    .files(
        {
            "third_party/lib.cpp": UNFORMATTED,
            "third_party/.clang-format": "BasedOnStyle: LLVM\n",
            "src/.clang-format":
                "BasedOnStyle: LLVM\nIndentWidth: 8\nAllowShortFunctionsOnASingleLine: None\n",
        },
        {
            manifest: { cxx: ["-std=c++17"], units: { "src/lib.cpp": [] } },
            setup: (workspace) => {
                fs.symlinkSync(
                    workspace.path("third_party/lib.cpp"),
                    workspace.path("src/lib.cpp"),
                );
            },
        },
    )
    .skipIf(process.platform === "win32")("style found beside the opened name", async ({ s }) => {
    await s.compiled("src/lib.cpp");
    const edits = await format(s, "src/lib.cpp");
    const texts = await s.apply({ changes: { [s.uri("src/lib.cpp")]: edits ?? [] } });
    expect(texts["src/lib.cpp"]).toBe("int add(int a, int b) {\n        return a + b;\n}\n");
});
