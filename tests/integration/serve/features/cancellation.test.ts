/// Client $/cancelRequest ends a request in flight with RequestCancelled, and
/// the server goes on serving.

import * as proto from "vscode-languageserver-protocol";
import type { Serve } from "@clice/tools/actions";
import { at, expect, serve } from "../../fixtures.ts";

// Without an index, a request waits for the compile alone: rows of the
// file's disk text would answer it while the compile is in flight.
const NO_INDEX = { config: { project: { enable_indexing: false } } };

const FMT: proto.FormattingOptions = { tabSize: 4, insertSpaces: true };

/// Send `method` with a token of its own: its reply — the error it is
/// answered with — and the cancel, which vscode-jsonrpc sends as
/// $/cancelRequest.
function cancellable(s: Serve, method: string, params: object) {
    const source = new proto.CancellationTokenSource();
    const reply = s.client.sendRequest(method, params, source.token).then(
        () => new Error(`${method} was not cancelled`),
        (error: unknown) => error,
    );
    return {
        reply,
        cancel: () => {
            source.cancel();
        },
    };
}

const CANCELLED = { code: proto.LSPErrorCodes.RequestCancelled };

/// A completion or signature help waits for the PCH of its preamble: the
/// parked PCH keeps it in flight while it is cancelled; the server then
/// still answers.
async function cancelWhileThePchBuilds(s: Serve, method: string, position: proto.Position) {
    const { reply } = await s.inFlight(
        "pch",
        "main.cpp",
        () => {
            s.open("main.cpp");
        },
        async () => {
            const request = cancellable(s, method, {
                textDocument: { uri: s.uri("main.cpp") },
                position,
            });
            // Any later reply: the server took the request up before.
            await s.counts();
            request.cancel();
            return { reply: request.reply };
        },
    );
    expect(await reply).toMatchObject(CANCELLED);

    s.open("tiny.cpp");
    expect(await s.hover(at("tiny.cpp", "va|lue"))).not.toBeNull();
}

serve.files(
    {
        "pre.h": "#pragma once\n",
        "main.cpp": '#include "pre.h"\nint value = 42;\nint probe = val',
        "tiny.cpp": "int value = 42;\n",
    },
    NO_INDEX,
)("cancelled completion replies", async ({ s }) => {
    await cancelWhileThePchBuilds(s, "textDocument/completion", { line: 2, character: 15 });
});

serve.files(
    {
        "pre.h": "#pragma once\n",
        "main.cpp":
            '#include "pre.h"\nvoid take(int a, int b);\nint use() { return take(1, 2); }\n',
        "tiny.cpp": "int value = 42;\n",
    },
    NO_INDEX,
)("cancelled signature help", async ({ s }) => {
    await cancelWhileThePchBuilds(s, "textDocument/signatureHelp", { line: 2, character: 26 });
});

const BASE = "int value = 1;\n";

serve.files({ "main.cpp": BASE }, NO_INDEX)("cancelled requests while compiling", async ({ s }) => {
    await s.compiled("main.cpp");
    const td = { uri: s.uri("main.cpp") };
    const head: proto.Range = {
        start: { line: 0, character: 0 },
        end: { line: 10, character: 0 },
    };
    const pulling: [string, object][] = [
        ["textDocument/hover", { textDocument: td, position: { line: 0, character: 4 } }],
        ["textDocument/definition", { textDocument: td, position: { line: 0, character: 4 } }],
        ["textDocument/documentSymbol", { textDocument: td }],
        ["textDocument/semanticTokens/full", { textDocument: td }],
        ["textDocument/foldingRange", { textDocument: td }],
        ["textDocument/inlayHint", { textDocument: td, range: head }],
        [
            "textDocument/codeAction",
            { textDocument: td, range: head, context: { diagnostics: [] } },
        ],
        ["textDocument/documentLink", { textDocument: td }],
    ];

    // Each request is cancelled while the compile of an edit it waits
    // for is parked; the format pair, which pulls no AST, is cancelled
    // while the last such compile is, which must outlive every cancel
    // and serve the closing hover.
    for (const [index, [method, params]] of pulling.entries()) {
        let request: ReturnType<typeof cancellable> | undefined;
        await s.inFlight(
            "compile",
            "main.cpp",
            () => {
                s.edit("main.cpp", { text: BASE + `int extra${index};\n` });
                request = cancellable(s, method, params);
            },
            async () => {
                request?.cancel();
                expect(await request?.reply, method).toMatchObject(CANCELLED);
                if (index === pulling.length - 1) {
                    for (const [format, extra] of [
                        ["textDocument/formatting", { options: FMT }],
                        ["textDocument/rangeFormatting", { range: head, options: FMT }],
                    ] as const) {
                        const pair = cancellable(s, format, { textDocument: td, ...extra });
                        pair.cancel();
                        expect(await pair.reply, format).toMatchObject(CANCELLED);
                    }
                }
            },
        );
    }

    expect(await s.hover(at("main.cpp", "int va|lue"))).not.toBeNull();
});

serve.files({ "main.cpp": BASE }, NO_INDEX)("edit supersedes compile", async ({ s }) => {
    // An edit mid-compile abandons the stale parse end-to-end: the request
    // that launched it rejects with ContentModified (the editor keeps what
    // it has and re-queries), and the next request answers on the new
    // content.
    const { first } = await s.inFlight(
        "compile",
        "main.cpp",
        () => {
            s.open("main.cpp");
        },
        async () => {
            const first = s.hover(at("main.cpp", "int va|lue")).then(
                () => null,
                (error: unknown) => error,
            );
            // Any later reply: the server took the hover up before.
            await s.counts();
            s.edit("main.cpp", { text: "int fixed;\n" });
            return { first };
        },
    );
    expect(await first).toMatchObject({ code: proto.LSPErrorCodes.ContentModified });

    expect(await s.hover(at("main.cpp", "int fi|xed"))).not.toBeNull();
});
