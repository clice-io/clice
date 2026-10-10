/// File operation tests for the clice LSP server.

import { at, expect, serve } from "../../fixtures.ts";

const test = serve("tiny");

test("did open", async ({ s }) => {
    s.open("main.cpp");
    await s.sync();
});

test("did change", async ({ s }) => {
    let text = s.disk.read("main.cpp");
    await s.inFlight(
        "compile",
        "main.cpp",
        () => {
            s.open("main.cpp");
        },
        () => {
            for (let i = 0; i < 20; i++) {
                text += "\n";
                s.edit("main.cpp", { text });
            }
        },
    );
    await s.clean("main.cpp");
});

serve("tiny", {
    files: {
        "clice.toml": "[diagnostics]\nclang_tidy = true\n",
        ".clang-tidy": 'Checks: "-*,bugprone-integer-division"\n',
        "main.cpp": "double ratio(int a, int b) {\n    return a / b;\n}\n",
    },
})("clang tidy", async ({ s }) => {
    s.open("main.cpp");
    const tidy = (await s.pushed("main.cpp"))?.filter((d) => d.source === "clang-tidy");
    expect(tidy?.map((d) => `${d.range.start.line}:${String(d.code)}`)).toEqual([
        "1:bugprone-integer-division",
    ]);
});

test("hover save close", async ({ s }) => {
    s.open("main.cpp");
    const hover = await s.hover(at("main.cpp", "int |add("));
    expect(hover).not.toBeNull();
    expect(hover!.contents).not.toBeNull();
    const start = at("main.cpp", "|int add(");
    await s.request("textDocument/completion", start);
    await s.request("textDocument/signatureHelp", start);
    s.close("main.cpp");
    await expect(s.hover(start)).rejects.toThrow("Document not open");
});
