/// A request whose buffer moved on mid-flight answers ContentModified:
/// clients apply a null or empty reply (VS Code drops every semantic token
/// on one) but keep what they have and ask again on this error.

import * as proto from "vscode-languageserver-protocol";
import { expect } from "vitest";
import { test, type Serve } from "@clice/tools/testing";

const CALL = { path: "main.cpp", anchor: "add(1, 2)" };
const HEAD = {
    path: "main.cpp",
    anchor: "int add(int lhs, int rhs) {\n    return lhs + rhs;\n}\n\n",
};

interface Request {
    method: string;
    where: string | typeof CALL;
    extra?: (s: Serve) => object;
}

const REQUESTS: readonly Request[] = [
    { method: "textDocument/hover", where: CALL },
    { method: "textDocument/semanticTokens/full", where: "main.cpp" },
    { method: "textDocument/foldingRange", where: "main.cpp" },
    {
        method: "textDocument/inlayHint",
        where: "main.cpp",
        extra: (s) => ({ range: s.range(HEAD) }),
    },
    { method: "textDocument/documentSymbol", where: "main.cpp" },
    { method: "textDocument/documentLink", where: "main.cpp" },
    { method: "textDocument/definition", where: CALL },
];

const PHASES = [
    ["reply", "reply"],
    ["parse", "worker"],
] as const;

/// What a reply says, but the id a semantic tokens reply numbers itself
/// with.
function answer(reply: unknown): unknown {
    if (typeof reply !== "object" || reply === null || Array.isArray(reply)) {
        return reply;
    }
    return Object.fromEntries(Object.entries(reply).filter(([key]) => key !== "resultId"));
}

/// Ask `request` of main.cpp, opened with `text`, while its compile is
/// parked at `at`, and edit the buffer before the compile finishes: the
/// request answers ContentModified, and asked again, what the server
/// answers once idle.
async function interrupted(
    s: Serve,
    request: Request,
    at: "reply" | "worker",
    text: string,
): Promise<void> {
    const { method, where } = request;
    const park = await s.park("compile", "main.cpp", { at });
    s.open("main.cpp", { text });
    const extra = request.extra?.(s) ?? {};
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
    const retried = answer(await s.request(method, where, extra));
    await s.sync();
    expect(retried).toEqual(answer(await s.request(method, where, extra)));
}

for (const [phase, at] of PHASES) {
    for (const request of REQUESTS) {
        test(`${request.method} mid-${phase}`, async (t) => {
            // Without an index the compile alone answers: entries of the disk
            // text would answer while the compile is in flight.
            const s = await t.start({ config: { project: { enable_indexing: false } } });
            await interrupted(s, request, at, t.disk.read("main.cpp"));
        });
    }
}

// Indexed, the entries of the disk text answer while the compile of a
// buffer equal to it is in flight; a buffer the disk does not hold waits on
// its compile all the same.
for (const [phase, at] of PHASES) {
    for (const request of REQUESTS) {
        test(`${request.method} mid-${phase}, indexed`, async (t) => {
            const s = await t.start();
            await s.indexed();
            const unsaved = t.disk.read("main.cpp").replace("return value - 3;", "return value;");
            await interrupted(s, request, at, unsaved);
        });
    }
}
