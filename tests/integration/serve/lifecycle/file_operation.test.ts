/// File operation tests for the clice LSP server.

import { at, expect, serve } from "../../fixtures.ts";

const test = serve.data("hello_world");

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

serve.data("clang_tidy")("clang tidy", async ({ s }) => {
    s.open("main.cpp");
    await s.sync();
});

test("hover save close", async ({ s }) => {
    s.open("main.cpp");
    const hover = await s.hover(at("main.cpp", "int |add("));
    expect(hover).not.toBeNull();
    expect(hover!.contents).not.toBeNull();
    const start = at("main.cpp", "|#include");
    await s.request("textDocument/completion", start);
    await s.request("textDocument/signatureHelp", start);
    s.close("main.cpp");
    await expect(s.hover(start)).rejects.toThrow("Document not open");
});
