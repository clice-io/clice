import * as proto from "vscode-languageserver-protocol";
import { test, type Serve } from "@clice/tools/testing";

// A close read with a request takes the document away before the
// request's work starts: the request belongs to a buffer that no longer
// exists, and answers ContentModified.

const VALUE = { path: "main.cpp", anchor: "int |value" };

for (const [method, params] of [
    [
        "textDocument/inlayHint",
        (s: Serve) => ({ range: s.range({ path: "main.cpp", anchor: "int value = 1;\n" }) }),
    ],
    [
        "textDocument/codeAction",
        (s: Serve) => {
            const { position } = s.position(VALUE);
            return { range: { start: position, end: position }, context: { diagnostics: [] } };
        },
    ],
    ["textDocument/signatureHelp", (s: Serve) => ({ position: s.position(VALUE).position })],
] as const) {
    test(method, async (t) => {
        const s = await t.start();
        await s.diagnostics("main.cpp");
        const uri = s.uri("main.cpp");
        // Raw messages: their arrival in one read is the point.
        const replies = await s.client.sendTogether([
            { jsonrpc: "2.0", id: method, method, params: { textDocument: { uri }, ...params(s) } },
            {
                jsonrpc: "2.0",
                method: proto.DidCloseTextDocumentNotification.method,
                params: { textDocument: { uri } },
            },
        ]);
        const error = replies.get(method)?.error;
        await t.show("reply", { code: error?.code ?? 0, message: error?.message ?? "" });
    });
}
