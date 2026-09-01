/// A request whose buffer moved on mid-flight answers ContentModified, never
/// a result for text that no longer exists — and never null, which a client
/// reads as "there is nothing" and clears what it shows.

import * as proto from "vscode-languageserver-protocol";
import { sleep } from "@clice/tools/client";
import { test, expect } from "../fixtures.ts";

// Two hundred thousand trivial declarations: slow to parse on any hardware,
// so an edit reliably lands while the request still waits on the compile.
const SLOW = Array.from({ length: 200_000 }, (_, i) => `int v${i};`).join("\n") + "\n";
const EDIT_SUPERSEDE_DELAY = 300;

test("edit mid-flight answers ContentModified", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("slow.cpp", SLOW);
    workspace.writeCDB(["slow.cpp"]);
    await client.initialize(workspace);

    const [uri] = client.open("slow.cpp");
    const td: proto.TextDocumentIdentifier = { uri };
    const head: proto.Range = {
        start: { line: 0, character: 0 },
        end: { line: 10, character: 0 },
    };

    // Every AST-backed feature, each preceded by an edit so it launches a
    // fresh parse of the whole body: the second edit then lands while that
    // parse is still running.
    const pulling: [string, unknown][] = [
        ["textDocument/hover", { textDocument: td, position: { line: 0, character: 4 } }],
        ["textDocument/semanticTokens/full", { textDocument: td }],
        ["textDocument/inlayHint", { textDocument: td, range: head }],
        ["textDocument/foldingRange", { textDocument: td }],
        ["textDocument/documentSymbol", { textDocument: td }],
        ["textDocument/documentLink", { textDocument: td }],
        ["textDocument/definition", { textDocument: td, position: { line: 0, character: 4 } }],
    ];
    let version = 0;
    for (const [method, params] of pulling) {
        version += 1;
        client.change(uri, version, SLOW + `int extra${version};\n`);
        const pending = client.sendRequest(method, params);
        await sleep(EDIT_SUPERSEDE_DELAY);
        version += 1;
        client.change(uri, version, SLOW + `int extra${version};\n`);
        await expect(pending, method).rejects.toMatchObject({
            code: proto.LSPErrorCodes.ContentModified,
        });
    }

    // The buffer settled: the next pull waits for the fresh compile and
    // answers on the current content.
    const hover = await client.hoverAt(uri, 0, 4);
    expect(hover).not.toBeNull();
}, 300_000);
