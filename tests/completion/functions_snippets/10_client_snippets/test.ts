import { test } from "@clice/tools/testing";

// Snippets reach only a client that declares snippet support; any other gets
// the plain text the same options insert.
const config = {
    code_completion: {
        bundle_overloads: false,
        enable_function_arguments_snippet: true,
        insert_paren_in_function_call: true,
    },
};

test("plain text client", async (t) => {
    const s = await t.start({ config });
    s.open("main.cpp");
    await t.show("call", s.completion(t.marker("call")), { parts: ["insert"] });
});

test("snippet client", async (t) => {
    const s = await t.start({
        config,
        capabilities: {
            textDocument: { completion: { completionItem: { snippetSupport: true } } },
        },
    });
    s.open("main.cpp");
    await t.show("call", s.completion(t.marker("call")), { parts: ["insert"] });
});
