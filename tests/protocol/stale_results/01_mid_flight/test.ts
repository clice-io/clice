/// A request whose buffer moved on mid-flight answers ContentModified:
/// clients apply a null or empty reply (VS Code drops every semantic token
/// on one) but keep what they have and ask again on this error.

import * as proto from "vscode-languageserver-protocol";
import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const HEAD = { start: { line: 0, character: 0 }, end: { line: 4, character: 0 } };

const REQUESTS = [
    { method: "textDocument/hover", where: { path: "main.cpp", anchor: "add(1, 2)" } },
    { method: "textDocument/semanticTokens/full", where: "main.cpp" },
    { method: "textDocument/foldingRange", where: "main.cpp" },
    { method: "textDocument/inlayHint", where: "main.cpp", extra: { range: HEAD } },
    { method: "textDocument/documentSymbol", where: "main.cpp" },
    { method: "textDocument/documentLink", where: "main.cpp" },
    { method: "textDocument/definition", where: { path: "main.cpp", anchor: "add(1, 2)" } },
] as const;

for (const [phase, at] of [
    ["reply", "reply"],
    ["parse", "worker"],
] as const) {
    for (const request of REQUESTS) {
        const { method, where } = request;
        const extra = "extra" in request ? request.extra : {};
        test(`${method} mid-${phase}`, async (t) => {
            // Without an index the compile alone answers: entries of the disk
            // text would answer while the compile is in flight.
            const s = await t.start({ config: { project: { enable_indexing: false } } });
            const park = await s.park("compile", "main.cpp", { at });
            s.open("main.cpp");
            await park.reached();
            const reply = s.request(method, where, extra).then(
                () => null,
                (error: unknown) => error,
            );
            // A later reply proves the server took the request up first.
            await s.counts();
            s.edit("main.cpp", { after: "int main() {", insert: " " });
            await park.release();
            expect(await reply).toMatchObject({ code: proto.LSPErrorCodes.ContentModified });
            expect(await s.request(method, where, extra)).not.toBeNull();
        });
    }
}
