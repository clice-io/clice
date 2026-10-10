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

/// A completion or signature help is cancelled while its worker works on
/// it: the cancel reaches the worker, which stops instead of answering, and
/// the server goes on serving another unit.
async function cancelInItsWorker(s: Serve, request: string, method: string, loc: Loc, next: Loc) {
    await s.compiled(loc.file);
    let sent: ReturnType<Serve["send"]> | undefined;
    await s.inWorker(
        request,
        loc.file,
        () => {
            sent = s.send(method, loc);
        },
        () => {
            sent?.cancel();
        },
    );
    await expect(sent?.reply).rejects.toMatchObject(CANCELLED);

    s.open(next.file);
    expect(await s.hover(next)).not.toBeNull();
}

const test = serve("shapes/headers", NO_INDEX);

test("cancelled completion replies", async ({ s }) => {
    await cancelInItsWorker(
        s,
        "completion",
        "textDocument/completion",
        at(s.file("main"), "shapes::ar|ea(c)"),
        at(s.file("registry"), "int registry_co|unt() {"),
    );
});

test("cancelled signature help", async ({ s }) => {
    await cancelInItsWorker(
        s,
        "signatureHelp",
        "textDocument/signatureHelp",
        at(s.file("main"), "shapes::area(|c)"),
        at(s.file("registry"), "int registry_co|unt() {"),
    );
});

const tiny = serve("tiny", NO_INDEX);

tiny("cancelled requests while compiling", async ({ s }) => {
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
    // for is parked; the format pair, which pulls no AST, is cancelled in
    // its worker while the last such compile is parked, which must outlive
    // every cancel and serve the closing hover.
    for (const [index, [method, where, extra]] of pulling.entries()) {
        let request: ReturnType<Serve["send"]> | undefined;
        let cancelled: Promise<void> | undefined;
        await s.inFlight(
            "compile",
            "main.cpp",
            () => {
                s.edit("main.cpp", { before: "int main() {", insert: `int extra${index};\n` });
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
                        let pair: ReturnType<Serve["send"]> | undefined;
                        await s.inWorker(
                            "format",
                            "main.cpp",
                            () => {
                                pair = s.send(format, "main.cpp", params);
                            },
                            () => {
                                pair?.cancel();
                            },
                        );
                        await expect(pair?.reply, format).rejects.toMatchObject(CANCELLED);
                    }
                }
            },
        );
    }

    expect(await s.hover(at("main.cpp", "int va|lue"))).not.toBeNull();
});

tiny("edit supersedes compile", async ({ s }) => {
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
            s.edit("main.cpp", { replace: "int value", with: "long value" });
            return { first };
        },
    );
    expect(await first).toMatchObject({ code: proto.LSPErrorCodes.ContentModified });

    expect(await s.hover(at("main.cpp", "va|lue = add"))).not.toBeNull();
});

tiny("edit interrupts the parse", async ({ s }) => {
    // The edit lands while the worker parses the old text: the master
    // interrupts the parse, the request that launched it rejects with
    // ContentModified, and the next request answers on the new content.
    const { first } = await s.inWorker(
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
            s.edit("main.cpp", { replace: "int value", with: "long value" });
            return { first };
        },
    );
    expect(await first).toMatchObject({ code: proto.LSPErrorCodes.ContentModified });
    expect(await s.hover(at("main.cpp", "va|lue = add"))).not.toBeNull();
});
