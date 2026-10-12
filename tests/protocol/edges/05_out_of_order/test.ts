import * as proto from "vscode-languageserver-protocol";
import { test, type TestContext } from "@clice/tools/testing";

// Messages out of the order LSP gives them: requests before the handshake
// and after shutdown, methods the server does not know, documents opened
// twice or closed and saved without an open.

/// Show what `reply` answers, or how it was refused.
async function answer(t: TestContext, label: string, reply: Promise<unknown>): Promise<void> {
    const failure = await reply.then(
        () => null,
        (error: unknown) => error,
    );
    if (failure instanceof proto.ResponseError) {
        await t.show(label, { code: failure.code, message: failure.message });
    } else {
        await t.show(label, reply);
    }
}

test("requests before initialize", async (t) => {
    const s = await t.spawn();
    await answer(t, "hover", s.hover(t.marker("add")));
    await answer(t, "symbols", s.workspaceSymbols("add"));
    await s.initialize();
});

test("an unknown method", async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    // Methods no action sends.
    await answer(t, "request", s.client.sendRequest("textDocument/bogus", {}));
    await answer(t, "dollar request", s.client.sendRequest("$/bogus", {}));
    await s.client.sendNotification("textDocument/bogus", {});
    await s.client.sendNotification("$/bogus", {});
    await t.show("hover after", s.hover(t.marker("add")));
});

// A second open of an open document replaces the text the server holds.
test("an open twice", async (t) => {
    const s = await t.start();
    await t.show("diagnostics", s.diagnostics("main.cpp"));
    // A second didOpen, which no action sends.
    await s.client.sendNotification(proto.DidOpenTextDocumentNotification.type, {
        textDocument: {
            uri: s.uri("main.cpp"),
            languageId: "cpp",
            version: 3,
            text: `${t.disk.read("main.cpp")}int bad = nope;\n`,
        },
    });
    // Its positions are in the second text, which the test does not hold.
    const pulled = await s.client.pullDiagnostics(s.uri("main.cpp"));
    await t.show(
        "diagnostics after",
        pulled.map(({ message }) => (typeof message === "string" ? message : message.value)),
    );
});

test("a close and save never opened", async (t) => {
    const s = await t.start();
    await s.client.sendNotification(proto.DidCloseTextDocumentNotification.type, {
        textDocument: { uri: s.uri("main.cpp") },
    });
    s.save("main.cpp");
    await t.show("diagnostics", s.diagnostics("main.cpp"));
});

test("requests after shutdown", async (t) => {
    const s = await t.start();
    await s.diagnostics("main.cpp");
    await s.client.sendRequest(proto.ShutdownRequest.type);
    await answer(t, "hover", s.hover(t.marker("add")));
    await answer(t, "symbols", s.workspaceSymbols("add"));
});
