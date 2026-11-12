import { expect } from "vitest";
import * as proto from "vscode-languageserver-protocol";
import { test } from "@clice/tools/testing";

// What an editor sends while the user types a statement, a key at a time:
// after each edit hover, completion, code actions, inlay hints, the
// outline, folding ranges and semantic tokens, every other one cancelled.
// Every request is answered, or told it was cancelled or that its text
// moved on; then the last text is answered.
test(async (t) => {
    const s = await t.start();
    await s.diagnostics("main.cpp");
    const typed = "result = add(result, 3);";
    const after = "int result = add(1, 2);\n";
    const outcomes: Promise<unknown>[] = [];
    let cancel = false;
    for (let length = 1; length <= typed.length; length++) {
        s.edit("main.cpp", {
            text: t.disk
                .read("main.cpp")
                .replace(after, `${after}    ${typed.slice(0, length)}\n`),
        });
        const call = t.at("main.cpp", "|add(1, 2)");
        const range = s.range(t.at("main.cpp", "add(1, 2)"));
        const requests = [
            s.send("textDocument/hover", t.at("main.cpp", "int |add")),
            s.send("textDocument/completion", call),
            s.send("textDocument/codeAction", "main.cpp", {
                range,
                context: { diagnostics: [] },
            }),
            s.send("textDocument/inlayHint", "main.cpp", { range }),
            s.send("textDocument/documentSymbol", "main.cpp"),
            s.send("textDocument/foldingRange", "main.cpp"),
            s.send("textDocument/semanticTokens/full", "main.cpp"),
        ];
        for (const request of requests) {
            outcomes.push(
                request.reply.then(
                    () => null,
                    (error: unknown) => error,
                ),
            );
            if (cancel) {
                request.cancel();
            }
            cancel = !cancel;
        }
    }
    for (const outcome of await Promise.all(outcomes)) {
        if (outcome !== null) {
            expect([
                proto.LSPErrorCodes.ContentModified,
                proto.LSPErrorCodes.RequestCancelled,
            ]).toContain((outcome as proto.ResponseError).code);
        }
    }
    await t.show("hover", s.hover(t.at("main.cpp", "int |add")));
    await t.show("outline", s.documentSymbols("main.cpp"));
    await t.show("diagnostics", s.diagnostics("main.cpp"));
});
