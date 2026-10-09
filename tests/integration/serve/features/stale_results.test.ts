/// A request whose buffer moved on while the compile it waits for was in
/// flight answers ContentModified, never a result for the old text and
/// never null (see tests/integration/features/stale_results.test.ts for why
/// clients need the error).

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
        const again = await s.request(method, where, extra);
        expect(again).not.toBeNull();
        expect((again as { data?: unknown[] }).data?.length ?? 1).toBeGreaterThan(0);
    },
);
