import { expect } from "vitest";
import * as proto from "vscode-languageserver-protocol";
import { test, type Anchor, type Park, type Serve } from "@clice/tools/testing";

// A request the editor cancels ($/cancelRequest) while its work is under
// way answers RequestCancelled, wherever the work stands: in the worker
// that completes code, or waiting on the compile it needs. The server
// carries on, and the compile a cancelled request waited on still serves
// the next one.

const WHERE = { path: "main.cpp", anchor: "int |value" };
const HEAD = { start: { line: 0, character: 0 }, end: { line: 4, character: 0 } };

/// Send `method`, cancel it once its work is parked and the server took
/// it up, release the work, and say how the request ended.
async function cancelled(
    s: Serve,
    park: Park,
    method: string,
    where: string | Anchor,
    extra: object = {},
): Promise<{ code: number; message: string }> {
    const sent = s.send(method, where, extra);
    const ended = sent.reply.then(
        () => null,
        (error: unknown) => error,
    );
    await park.reached();
    // A later reply proves the server took the request up first.
    await s.counts();
    sent.cancel();
    await park.release();
    const error = await ended;
    expect(error).toBeInstanceOf(proto.ResponseError);
    const { code, message } = error as proto.ResponseError<unknown>;
    return { code, message };
}

for (const [request, method, at] of [
    ["completion", "textDocument/completion", "complete"],
    ["signatureHelp", "textDocument/signatureHelp", "argument"],
] as const) {
    test(`${request} in its worker`, async (t) => {
        const s = await t.start();
        await s.diagnostics("main.cpp");
        const park = await s.park(request, "main.cpp", { at: "worker" });
        await t.show("cancelled", await cancelled(s, park, method, t.marker(at)));
        await t.show("hover after", s.hover(t.marker("value")));
    });
}

test("waiting on the compile", async (t) => {
    const s = await t.start();
    await s.diagnostics("main.cpp");
    const requests: [string, string | typeof WHERE, object][] = [
        ["textDocument/hover", WHERE, {}],
        ["textDocument/definition", WHERE, {}],
        ["textDocument/documentSymbol", "main.cpp", {}],
        ["textDocument/semanticTokens/full", "main.cpp", {}],
        ["textDocument/foldingRange", "main.cpp", {}],
        ["textDocument/inlayHint", "main.cpp", { range: HEAD }],
        ["textDocument/codeAction", "main.cpp", { range: HEAD, context: { diagnostics: [] } }],
        ["textDocument/documentLink", "main.cpp", {}],
    ];
    const outcomes: Record<string, { code: number; message: string }> = {};
    let version = 0;
    for (const [method, where, extra] of requests) {
        // Each request waits on a compile of its own, held in its worker.
        const park = await s.park("compile", "main.cpp", { at: "worker" });
        version += 1;
        s.edit("main.cpp", { before: "int use()", insert: `int extra${String(version)};\n` });
        outcomes[method] = await cancelled(s, park, method, where, extra);
    }
    await t.show("cancelled", outcomes);
    await t.show("hover after", s.hover(t.at("main.cpp", "int |value")));
});

test("formatting", async (t) => {
    const s = await t.start();
    await s.diagnostics("main.cpp");
    const options = { options: { tabSize: 4, insertSpaces: true } };
    const outcomes: Record<string, { code: number; message: string }> = {};
    for (const [method, extra] of [
        ["textDocument/formatting", options],
        ["textDocument/rangeFormatting", { ...options, range: HEAD }],
    ] as const) {
        const park = await s.park("format", "main.cpp", { at: "worker" });
        outcomes[method] = await cancelled(s, park, method, "main.cpp", extra);
    }
    await t.show("cancelled", outcomes);
});
