/// A request whose buffer moved on mid-flight answers ContentModified, never
/// a result computed on the old text and never null.
///
/// Why an error and not null: to a client, null is a real answer — "this
/// document has nothing". VS Code's semantic-token pipeline keeps a full
/// request in flight across keystrokes (it does not cancel on edit; it
/// reconciles the reply with the edits made meanwhile), and on a null reply
/// it clears every semantic token of the document, so the whole file falls
/// back to TextMate colors until the next pull lands — the flicker users saw
/// while typing. On a ContentModified error the same pipeline keeps the
/// tokens it has and schedules a re-pull; the client even advertises this in
/// `staleRequestSupport.retryOnContentModified`. Inlay hints, folds and the
/// outline behave the same way: an empty reply is applied, an error is not.
///
/// Why not the old result either: whole-document replies carry positions of
/// the text they were computed on; the client would map them onto the
/// edited buffer at the wrong places (formatting edits would even corrupt
/// the file). The server has no AST for the old buffer anymore once the edit
/// superseded the compile, so the only honest answer is "changed, ask again".
///
/// Completion is the exception while the edits sit at or past its cursor:
/// VS Code neither cancels nor re-asks a completion the user keeps typing
/// into — it filters the reply by what was typed meanwhile, and treats
/// ContentModified as an empty list. An edit before the cursor moves the
/// reply's ranges, so that one still answers ContentModified.

import * as proto from "vscode-languageserver-protocol";
import { at, expect, serve, type Loc } from "../../fixtures.ts";

const FEATURES: { method: string; where: string | Loc; extra?: object }[] = [
    { method: "textDocument/hover", where: at("main.cpp", "add(1, 2)") },
    { method: "textDocument/semanticTokens/full", where: "main.cpp" },
    {
        method: "textDocument/inlayHint",
        where: "main.cpp",
        extra: { range: { start: { line: 0, character: 0 }, end: { line: 10, character: 0 } } },
    },
    { method: "textDocument/foldingRange", where: "main.cpp" },
    { method: "textDocument/documentSymbol", where: "main.cpp" },
    { method: "textDocument/documentLink", where: "main.cpp" },
    { method: "textDocument/definition", where: at("main.cpp", "add(1, 2)") },
];

// Without an index, a request is answered from the compile alone: rows of
// the file's disk text would answer it while the compile is in flight.
serve("tiny", { config: { project: { enable_indexing: false } } }).for(FEATURES)(
    "$method mid-compile answers ContentModified",
    async ({ method, where, extra }, { s }) => {
        const { reply } = await s.inFlight(
            "compile",
            "main.cpp",
            () => {
                s.open("main.cpp");
            },
            async () => {
                const reply = s.request(method, where, extra).then(
                    () => null,
                    (error: unknown) => error,
                );
                // Any later reply: the server took the request up before,
                // and it waits on the parked compile when the edit arrives.
                await s.counts();
                s.edit("main.cpp", { after: "int main() {", insert: " " });
                return { reply };
            },
        );
        expect(await reply).toMatchObject({ code: proto.LSPErrorCodes.ContentModified });

        // Asked again, the request waits for the compile of the text as it
        // stands and is answered.
        const again = await s.request<{ data?: unknown[] } | null>(method, where, extra);
        expect(again).not.toBeNull();
        expect(again?.data?.length ?? 1).toBeGreaterThan(0);
    },
);

function labels(list: proto.CompletionList | proto.CompletionItem[] | null | undefined): string[] {
    return (Array.isArray(list) ? list : (list?.items ?? [])).map((item) => item.label);
}

const PROBE = "int extra_value;\nint probe = extra_";

// A completion waits for the PCH of its preamble: the parked PCH keeps it in
// flight while the buffer moves on.
serve.files({
    "pre.h": "#pragma once\n",
    "more.h": "#pragma once\n",
    "main.cpp": `#include "pre.h"\n${PROBE}`,
})("edit mid-flight still completes", async ({ s }) => {
    const probe = at("main.cpp", "int probe = extra_|");
    const { served } = await s.inFlight(
        "pch",
        "main.cpp",
        () => {
            s.open("main.cpp");
        },
        async () => {
            const served = s.completion(probe);
            // Any later reply: the server took the completion up before.
            await s.counts();
            s.edit("main.cpp", { after: "int probe = extra_", insert: "v" });
            return { served };
        },
    );
    expect(labels(await served)).toContain("extra_value");

    let moved: Promise<unknown> = Promise.resolve();
    await s.inFlight(
        "pch",
        "main.cpp",
        () => {
            // A new preamble, whose PCH the completion waits for.
            s.edit("main.cpp", { after: '#include "pre.h"\n', insert: '#include "more.h"\n' });
            moved = s.completion(probe).then(
                () => null,
                (error: unknown) => error,
            );
        },
        () => {
            s.edit("main.cpp", { before: '#include "pre.h"', insert: "int moved;\n" });
        },
    );
    expect(await moved).toMatchObject({ code: proto.LSPErrorCodes.ContentModified });
});

// The messages below are written in one write, which the server reads
// together: a request still belongs to the text it was asked about, though
// the edit read with it is applied before its task starts.

function request(id: string, method: string, params: object): proto.RequestMessage {
    return { jsonrpc: "2.0", id, method, params };
}

function notification(method: string, params: object): proto.NotificationMessage {
    return { jsonrpc: "2.0", method, params };
}

function edit(uri: string, text: string): proto.NotificationMessage {
    return notification(proto.DidChangeTextDocumentNotification.method, {
        textDocument: { uri, version: 2 },
        contentChanges: [{ text }],
    });
}

const READ_WITH_AN_EDIT = [
    { method: "textDocument/hover", params: { position: { line: 0, character: 4 } } },
    {
        method: "textDocument/formatting",
        params: { options: { tabSize: 4, insertSpaces: true } },
    },
];

serve.files({ "main.cpp": "int value = 1;\n" }).for(READ_WITH_AN_EDIT)(
    "$method read with an edit answers ContentModified",
    async ({ method, params }, { s }) => {
        await s.compiled("main.cpp");
        const uri = s.uri("main.cpp");
        const replies = await s.client.sendTogether([
            request(method, method, { textDocument: { uri }, ...params }),
            edit(uri, "int  value = 2;\n"),
        ]);
        expect(replies.get(method)?.error?.code).toBe(proto.LSPErrorCodes.ContentModified);
    },
);

const completing = serve.files({ "main.cpp": PROBE });

completing("completion read with an edit is served", async ({ s }) => {
    await s.compiled("main.cpp");
    const uri = s.uri("main.cpp");
    const replies = await s.client.sendTogether([
        request("completion", "textDocument/completion", {
            textDocument: { uri },
            position: { line: 1, character: 18 },
        }),
        edit(uri, PROBE + "v"),
    ]);
    const served = replies.get("completion")?.result as proto.CompletionList | null;
    expect(labels(served)).toContain("extra_value");
});

completing("completion read with a reopen answers ContentModified", async ({ s }) => {
    await s.compiled("main.cpp");
    const uri = s.uri("main.cpp");
    const replies = await s.client.sendTogether([
        request("completion", "textDocument/completion", {
            textDocument: { uri },
            position: { line: 1, character: 18 },
        }),
        notification(proto.DidCloseTextDocumentNotification.method, { textDocument: { uri } }),
        notification(proto.DidOpenTextDocumentNotification.method, {
            textDocument: { uri, languageId: "cpp", version: 1, text: PROBE },
        }),
    ]);
    expect(replies.get("completion")?.error?.code).toBe(proto.LSPErrorCodes.ContentModified);
});
