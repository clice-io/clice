/// Client $/cancelRequest ends a request in flight with RequestCancelled, and
/// the server goes on serving.

import * as proto from "vscode-languageserver-protocol";
import type { Loc, Serve } from "@clice/tools/actions";
import { at, expect, serve } from "../../fixtures.ts";

// Without an index, a request waits for the compile alone: rows of the
// file's disk text would answer it while the compile is in flight.
const NO_INDEX = { config: { project: { enable_indexing: false } } };

const FMT: proto.FormattingOptions = { tabSize: 4, insertSpaces: true };

const CANCELLED = { code: proto.LSPErrorCodes.RequestCancelled };

/// A completion or signature help waits for the PCH of its preamble: the
/// parked PCH keeps it in flight while it is cancelled; the server then
/// still answers.
async function cancelWhileThePchBuilds(s: Serve, method: string, loc: Loc) {
    const { cancelled } = await s.inFlight(
        "pch",
        "main.cpp",
        () => {
            s.open("main.cpp");
        },
        async () => {
            const request = s.send(method, loc);
            const cancelled = expect(request.reply).rejects.toMatchObject(CANCELLED);
            // Any later reply: the server took the request up before.
            await s.counts();
            request.cancel();
            return { cancelled };
        },
    );
    await cancelled;

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
    await cancelWhileThePchBuilds(s, "textDocument/completion", at("main.cpp", "probe = val|"));
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
    await cancelWhileThePchBuilds(s, "textDocument/signatureHelp", at("main.cpp", "take(1,| 2)"));
});

const BASE = "int value = 1;\n";

serve.files({ "main.cpp": BASE }, NO_INDEX)("cancelled requests while compiling", async ({ s }) => {
    await s.compiled("main.cpp");
    const value = at("main.cpp", "int |value");
    const head: proto.Range = {
        start: { line: 0, character: 0 },
        end: { line: 10, character: 0 },
    };
    const pulling: [string, string | Loc, object?][] = [
        ["textDocument/hover", value],
        ["textDocument/definition", value],
        ["textDocument/documentSymbol", "main.cpp"],
        ["textDocument/semanticTokens/full", "main.cpp"],
        ["textDocument/foldingRange", "main.cpp"],
        ["textDocument/inlayHint", "main.cpp", { range: head }],
        ["textDocument/codeAction", "main.cpp", { range: head, context: { diagnostics: [] } }],
        ["textDocument/documentLink", "main.cpp"],
    ];

    // Each request is cancelled while the compile of an edit it waits
    // for is parked; the format pair, which pulls no AST, is cancelled
    // while the last such compile is, which must outlive every cancel
    // and serve the closing hover.
    for (const [index, [method, where, extra]] of pulling.entries()) {
        let request: ReturnType<Serve["send"]> | undefined;
        let cancelled: Promise<void> | undefined;
        await s.inFlight(
            "compile",
            "main.cpp",
            () => {
                s.edit("main.cpp", { text: BASE + `int extra${index};\n` });
                request = s.send(method, where, extra);
                cancelled = expect(request.reply, method).rejects.toMatchObject(CANCELLED);
            },
            async () => {
                request?.cancel();
                await cancelled;
                if (index === pulling.length - 1) {
                    for (const [format, params] of [
                        ["textDocument/formatting", { options: FMT }],
                        ["textDocument/rangeFormatting", { range: head, options: FMT }],
                    ] as const) {
                        const pair = s.send(format, "main.cpp", params);
                        pair.cancel();
                        await expect(pair.reply, format).rejects.toMatchObject(CANCELLED);
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
