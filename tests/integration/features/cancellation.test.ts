/// Client $/cancelRequest reaches the worker (end-to-end cancellation).

import * as proto from "vscode-languageserver-protocol";
import { sleep, withTimeout, type CliceClient } from "@clice/tools/client";
import { test, expect } from "../fixtures.ts";

// Two hundred thousand trivial declarations: slow to parse on any hardware,
// cheap to abandon (the worker polls the stop flag per declaration).
const SLOW = Array.from({ length: 200_000 }, (_, i) => `int v${i};`).join("\n") + "\n";
const LAST_LINE = 199_999;
const CANCELLATION_DELAY = 100;
const EDIT_SUPERSEDE_DELAY = 300;

const FMT: proto.FormattingOptions = { tabSize: 4, insertSpaces: true };

/// Send `method`, cancel it 0.1s later, require a RequestCancelled reply.
///
/// vscode-jsonrpc sends $/cancelRequest automatically when the token from a
/// CancellationTokenSource passed as the last argument is cancelled; the
/// request then rejects with the server's RequestCancelled reply.
async function cancelAndExpect(
    client: CliceClient,
    method: string,
    params: unknown,
    timeout = 30_000,
): Promise<void> {
    const source = new proto.CancellationTokenSource();
    const task = client.sendRequest(method, params, source.token);
    await sleep(CANCELLATION_DELAY);
    source.cancel();
    const err: unknown = await withTimeout(task, timeout, `${method} cancel`).then(
        () => {
            throw new Error(`${method} was not cancelled`);
        },
        (e: unknown) => e,
    );
    expect((err as { code?: number }).code).toBe(proto.LSPErrorCodes.RequestCancelled);
}

function doc(uri: string): proto.TextDocumentIdentifier {
    return { uri };
}

test("cancelled completion replies", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("slow.cpp", SLOW);
    workspace.write("tiny.cpp", "int value = 42;\n");
    workspace.writeCDB(["slow.cpp", "tiny.cpp"]);
    await client.initialize(workspace);

    const [uri] = client.open("slow.cpp");

    // Complete at the LAST line: clang truncates the parse at the
    // completion point, so a point at the top would skip the slow body
    // entirely and the request could finish before the cancel arrives.
    await cancelAndExpect(client, "textDocument/completion", {
        textDocument: doc(uri),
        position: { line: LAST_LINE, character: 4 },
    });

    // The server survives the cancellation and still answers. Hover a
    // small file: the slow one would recompile from scratch here and
    // can brush the 30s client timeout on a debug CI runner.
    const [tinyUri] = client.open("tiny.cpp");
    const hover = await client.hoverAt(tinyUri, 0, 5);
    expect(hover).not.toBeNull();
}, 120_000);

test("cancelled signature help", async ({ session }) => {
    // Same shape as completion (the other stateless build kind): the call
    // site sits past the slow body, so reaching it means a full parse.
    const text = SLOW + "void take(int a, int b);\nint use() { return take(1, 2); }\n";
    const { client, workspace } = session.tmp();
    workspace.write("sig.cpp", text);
    workspace.write("tiny.cpp", "int value = 42;\n");
    workspace.writeCDB(["sig.cpp", "tiny.cpp"]);
    await client.initialize(workspace);

    const [uri] = client.open("sig.cpp");

    await cancelAndExpect(client, "textDocument/signatureHelp", {
        textDocument: doc(uri),
        position: { line: LAST_LINE + 2, character: 26 },
    });

    const [tinyUri] = client.open("tiny.cpp");
    const hover = await client.hoverAt(tinyUri, 0, 5);
    expect(hover).not.toBeNull();
}, 120_000);

test("cancelled requests while compiling", async ({ session }) => {
    // Every forwarded feature cancelled while the compile it waits on is
    // still churning through the slow body. Each AST-backed request is
    // preceded by an edit, so it launches a FRESH 200k-declaration parse
    // and the 0.1s cancel window never depends on how much of a previous
    // parse is left (a fast runner finished the shared parse mid-sweep).
    const { client, workspace } = session.tmp();
    workspace.write("slow.cpp", SLOW);
    workspace.writeCDB(["slow.cpp"]);
    await client.initialize(workspace);

    const [uri] = client.open("slow.cpp");
    const td = doc(uri);
    const head: proto.Range = {
        start: { line: 0, character: 0 },
        end: { line: 10, character: 0 },
    };

    const pulling: [string, unknown][] = [
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
    let version = 0;
    for (const [method, params] of pulling) {
        version += 1;
        client.change(uri, version, SLOW + `int extra${version};\n`);
        await cancelAndExpect(client, method, params);
    }

    // The format pair never pulls an AST, so no edit: the last pulling
    // request's compile must survive these cancels too and serve the
    // closing hover — the shared compile outlives every client cancel.
    await cancelAndExpect(client, "textDocument/formatting", {
        textDocument: td,
        options: FMT,
    });
    await cancelAndExpect(client, "textDocument/rangeFormatting", {
        textDocument: td,
        range: head,
        options: FMT,
    });

    const hover = await client.hoverAt(uri, 0, 4);
    expect(hover).not.toBeNull();
}, 300_000);

test("edit supersedes compile", async ({ session }) => {
    // An edit mid-compile abandons the stale parse end-to-end: the request
    // that launched it resolves promptly (null — the editor re-queries
    // after an edit), and the next request answers on the new content.
    const { client, workspace } = session.tmp();
    workspace.write("edited.cpp", SLOW);
    workspace.writeCDB(["edited.cpp"]);
    await client.initialize(workspace);

    const [uri] = client.open("edited.cpp");

    const first = client.hoverAt(uri, 0, 4);
    await sleep(EDIT_SUPERSEDE_DELAY);
    client.change(uri, 1, "int fixed;\n");

    expect(await withTimeout(first, 10_000, "first hover")).toBeNull();

    const hover = await client.hoverAt(uri, 0, 4);
    expect(hover).not.toBeNull();
}, 120_000);
