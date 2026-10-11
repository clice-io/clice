import * as proto from "vscode-languageserver-protocol";
import { test } from "@clice/tools/testing";

// Messages written in one write are read together: a request still belongs
// to the text it was asked about, though the change read with it applies
// before its work starts. Positions and edits computed on the new text
// would land in the wrong places, so it answers ContentModified.

for (const [method, params] of [
    ["textDocument/hover", { position: { line: 0, character: 4 } }],
    ["textDocument/formatting", { options: { tabSize: 4, insertSpaces: true } }],
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
                method: proto.DidChangeTextDocumentNotification.method,
                params: {
                    textDocument: { uri, version: 2 },
                    contentChanges: [{ text: "int  value = 2;\n" }],
                },
            },
        ]);
        const error = replies.get(method)?.error;
        await t.show("reply", { code: error?.code ?? 0, message: error?.message ?? "" });
    });
}
