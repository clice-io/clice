import * as proto from "vscode-languageserver-protocol";
import { test } from "@clice/tools/testing";

// A close read with a request takes the document away before the
// request's work starts: the request belongs to a buffer that no longer
// exists, and answers ContentModified.

for (const [method, params] of [
    [
        "textDocument/inlayHint",
        { range: { start: { line: 0, character: 0 }, end: { line: 1, character: 0 } } },
    ],
    [
        "textDocument/codeAction",
        {
            range: { start: { line: 0, character: 4 }, end: { line: 0, character: 4 } },
            context: { diagnostics: [] },
        },
    ],
    ["textDocument/signatureHelp", { position: { line: 0, character: 4 } }],
] as const) {
    test(method, async (t) => {
        const s = await t.start();
        await s.diagnostics("main.cpp");
        const uri = s.uri("main.cpp");
        // Raw messages: their arrival in one read is the point.
        const replies = await s.client.sendTogether([
            { jsonrpc: "2.0", id: method, method, params: { textDocument: { uri }, ...params } },
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
