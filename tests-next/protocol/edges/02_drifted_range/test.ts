import * as proto from "vscode-languageserver-protocol";
import { test } from "@clice/tools/testing";

// An incremental change whose range lies past the end of the buffer: the
// editor's view and the server's drifted apart. The range is clamped, as
// LSP 3.17 asks, so the text lands at the true end instead of being
// dropped, and requests are served on the clamped buffer.

test(async (t) => {
    const s = await t.start();
    await s.diagnostics("main.cpp");
    const uri = s.uri("main.cpp");
    // A range the test's own copy of the buffer cannot hold.
    await s.client.sendNotification(proto.DidChangeTextDocumentNotification.type, {
        textDocument: { uri, version: 2 },
        contentChanges: [
            {
                range: { start: { line: 999, character: 0 }, end: { line: 999, character: 5 } },
                text: "oops",
            },
        ],
    });
    await t.show("hover", s.hover(t.marker("foo")));
    // Its positions are in the server's buffer, which the test does not
    // hold: the messages say where the text landed.
    const pulled = await s.client.pullDiagnostics(uri);
    await t.show(
        "diagnostics",
        pulled.map(({ message }) => (typeof message === "string" ? message : message.value)),
    );
});
