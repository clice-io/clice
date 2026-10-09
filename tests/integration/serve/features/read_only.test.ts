/// The readonly serving modes: reads are answered from the index while
/// PCH/AST builds stay pull-driven — the mode only decides whether they
/// are a goal at all. Each degraded surface the design pins (empty inlay
/// hints, no diagnostics push before a pull) is asserted explicitly.
///
/// `s.open` pulls diagnostics, which is itself a read that pulls a compile
/// under "off"; a case pinning what the first read or the open itself
/// starts opens through `s.client` instead.

import * as proto from "vscode-languageserver-protocol";
import type { Serve } from "@clice/tools/actions";
import { at, expect, serve } from "../../fixtures.ts";

const HEADER = "#pragma once\nint add(int a, int b);\n";
const MAIN = [
    '#include "header.h"',
    "",
    "/// Doubles a value.",
    "int twice(int x) {",
    "    return add(x, x);",
    "}",
    "",
    "int main() { return twice(2); }",
    "",
].join("\n");
const PROJECT = { "header.h": HEADER, "main.cpp": MAIN };

const AUTO = { config: { project: { readonly: "auto" } } };
const ON = { config: { project: { readonly: "on" } } };
const OFF = { config: { project: { readonly: "off" } } };

const ADD = at("main.cpp", "add(x, x)");
const LINES_0_TO_8 = { start: { line: 0, character: 0 }, end: { line: 8, character: 0 } };

function labelsOf(result: unknown): string[] {
    const list = result as proto.CompletionItem[] | proto.CompletionList | null;
    if (list === null) {
        return [];
    }
    const items = Array.isArray(list) ? list : list.items;
    return items.map((item) => item.label);
}

function symbolNames(result: unknown): string[] | undefined {
    return (result as proto.DocumentSymbol[] | null)?.map((symbol) => symbol.name);
}

function linksTo(result: unknown, file: string): boolean | undefined {
    return (result as proto.DocumentLink[] | null)?.some((link) => link.target?.endsWith(file));
}

/// The pushes `file` received; none while it was never published.
async function publishes(s: Serve, file: string): Promise<number> {
    return (await s.counts()).files[file]?.publish ?? 0;
}

/// The diagnostics push of a compile some read pulled has landed, and it
/// carries no error.
async function pushedNoErrors(s: Serve, file: string): Promise<void> {
    await s.sync();
    expect(await publishes(s, file)).toBeGreaterThan(0);
    const pushed = s.client.lastPublish(s.uri(file))?.diagnostics ?? [];
    expect(pushed.filter((d) => d.severity === proto.DiagnosticSeverity.Error)).toEqual([]);
}

serve.files(PROJECT, AUTO)("index serves unedited reads", async ({ s }) => {
    s.open("main.cpp");
    await s.indexed();

    const tokens = (await s.request(
        "textDocument/semanticTokens/full",
        "main.cpp",
    )) as proto.SemanticTokens | null;
    expect(tokens?.data.length ?? 0).toBeGreaterThan(0);

    expect(symbolNames(await s.request("textDocument/documentSymbol", "main.cpp"))).toContain(
        "twice",
    );

    const folds = (await s.request("textDocument/foldingRange", "main.cpp")) as
        proto.FoldingRange[] | null;
    expect(folds?.length ?? 0).toBeGreaterThan(0);

    expect(linksTo(await s.request("textDocument/documentLink", "main.cpp"), "header.h")).toBe(
        true,
    );

    expect(s.show(await s.hover(ADD))).toContain("add");
    expect(s.show(await s.definition(ADD))).toBe("header.h: int add(int a, int b);");

    const highlights = (await s.request(
        "textDocument/documentHighlight",
        at("main.cpp", "add(|x, x)"),
    )) as proto.DocumentHighlight[] | null;
    expect(highlights?.map((h) => [h.range.start.line, h.range.start.character, h.kind])).toEqual([
        [3, 14, proto.DocumentHighlightKind.Text],
        [4, 15, proto.DocumentHighlightKind.Read],
        [4, 18, proto.DocumentHighlightKind.Read],
    ]);

    // Without an AST, selection ranges come from the text alone.
    const [selection] =
        ((await s.request("textDocument/selectionRange", "main.cpp", {
            positions: [{ line: 4, character: 15 }],
        })) as proto.SelectionRange[] | null) ?? [];
    const steps: string[] = [];
    for (let step = selection; step; step = step.parent) {
        steps.push(
            `${step.range.start.line}:${step.range.start.character}-${step.range.end.line}:${step.range.end.character}`,
        );
    }
    expect(steps).toEqual(["4:15-4:16", "4:15-4:19", "4:14-4:20", "4:4-4:21", "3:17-5:1"]);

    // Pinned degradations of the read-only surface.
    const hints = await s.request("textDocument/inlayHint", "main.cpp", { range: LINES_0_TO_8 });
    expect(hints ?? []).toEqual([]);
    expect(await publishes(s, "main.cpp")).toBe(0);

    // Reading never builds a PCH.
    expect(s.workspace.pchFiles()).toEqual([]);
});

serve.files(
    {
        "widget.h": "#pragma once\nstruct Widget {};\nWidget make();\n",
        "main.cpp": '#include "widget.h"\n\nvoid use() {\n    auto widget = make();\n}\n',
    },
    AUTO,
)("index navigates from auto", async ({ s }) => {
    s.open("main.cpp");
    await s.indexed();

    // `auto` stands for `Widget`, defined on line 1 of the header.
    const auto = at("main.cpp", "a|uto widget");
    for (const located of [
        await s.definition(auto),
        await s.request("textDocument/typeDefinition", auto),
    ]) {
        const [site] = (located ?? []) as proto.Location[];
        expect(site?.uri.endsWith("widget.h")).toBe(true);
        expect(site?.range.start).toEqual({ line: 1, character: 7 });
    }
    expect(s.show(await s.hover(auto))).toContain("Widget");
    expect(s.workspace.pchFiles()).toEqual([]);
});

// Past the 8 MiB full-lex cap only semantic tokens and folds follow the
// investment policy; row- and cursor-backed projections still serve from
// the shard.
serve.files({ "header.h": HEADER, "main.cpp": MAIN + "// padding\n".repeat(800_000) }, AUTO)(
    "oversized buffer keeps row answers",
    async ({ s }) => {
        s.open("main.cpp");
        await s.indexed();

        expect(symbolNames(await s.request("textDocument/documentSymbol", "main.cpp"))).toContain(
            "twice",
        );
        expect(linksTo(await s.request("textDocument/documentLink", "main.cpp"), "header.h")).toBe(
            true,
        );
        expect(s.show(await s.hover(ADD))).toContain("add");

        expect(await s.request("textDocument/semanticTokens/full", "main.cpp")).toBeNull();
        expect(await s.request("textDocument/foldingRange", "main.cpp")).toEqual([]);
        expect(s.workspace.pchFiles()).toEqual([]);
    },
);

serve.files(PROJECT, AUTO)("cold outline awaits the boost", async ({ s }) => {
    // No wait for the index: outline and links have no refresh request, so
    // the replies themselves await the didOpen boost instead of freezing an
    // empty result in the client's cache.
    s.client.open("main.cpp");
    const [symbols, links] = await Promise.all([
        s.request("textDocument/documentSymbol", "main.cpp"),
        s.request("textDocument/documentLink", "main.cpp"),
    ]);
    expect(symbolNames(symbols)).toContain("twice");
    expect(linksTo(links, "header.h")).toBe(true);
});

serve.files(PROJECT, AUTO)("edit escalates to compile", async ({ s }) => {
    s.open("main.cpp");
    await s.indexed();
    expect(await publishes(s, "main.cpp")).toBe(0);

    // The edit flips the mode; the build itself stays pull-driven, so
    // nothing lands until the next read pulls it.
    s.edit("main.cpp", { text: MAIN + "// edited\n" });
    await s.sync();
    expect(await publishes(s, "main.cpp")).toBe(0);

    expect(s.show(await s.hover(ADD))).toContain("add");
    await pushedNoErrors(s, "main.cpp");
});

serve.files(PROJECT, AUTO)("diverged open buffer escalates", async ({ s }) => {
    // Warm the index, then restart: the second server starts with the
    // shard on disk and nothing compiled.
    s.open("main.cpp");
    await s.indexed();
    await s.stop();
    await s.start();

    // A restored unsaved buffer diverges from the indexed content: the
    // open itself escalates, so the first read pulls a compile instead
    // of answering empty from a withdrawn shard.
    s.client.open("main.cpp", 0, { text: MAIN + "// restored, unsaved\n" });
    expect(symbolNames(await s.request("textDocument/documentSymbol", "main.cpp"))).toContain(
        "twice",
    );
    await pushedNoErrors(s, "main.cpp");
});

// orphan.cpp is on disk but outside the CDB with no includer, so indexing
// refuses the guessed command; scratch.cpp has a CDB entry but exists only
// as the didOpen buffer, and indexing reads disk truth. Neither boost can
// deliver a shard: the failed attempt escalates, and the parked outline
// re-routes to a pulled compile.
serve.files(
    { ...PROJECT, "orphan.cpp": "int orphan() { return 1; }\n" },
    { ...AUTO, manifest: { cxx: ["-std=c++17"], units: { "main.cpp": [], "scratch.cpp": [] } } },
)("unservable boost escalates", async ({ s }) => {
    s.client.open("orphan.cpp");
    expect(symbolNames(await s.request("textDocument/documentSymbol", "orphan.cpp"))).toContain(
        "orphan",
    );
    await pushedNoErrors(s, "orphan.cpp");

    s.client.open("scratch.cpp", 0, { text: "int scratch() { return 2; }\n" });
    expect(symbolNames(await s.request("textDocument/documentSymbol", "scratch.cpp"))).toContain(
        "scratch",
    );
    await pushedNoErrors(s, "scratch.cpp");
});

// C++ code behind a C suffix, forced by the CDB's -x: the index projection
// must lex with the forced dialect — under the C keyword table `class`
// would go unpainted and the first token would start at `Widget` instead.
serve.files(
    { "legacy.c": "class Widget { public: int value; };\n" },
    { ...AUTO, manifest: { units: { "legacy.c": ["-x", "c++"] } } },
)("explicit -x beats the suffix", async ({ s }) => {
    s.open("legacy.c");
    await s.indexed();
    const tokens = (await s.request(
        "textDocument/semanticTokens/full",
        "legacy.c",
    )) as proto.SemanticTokens | null;
    expect(tokens?.data.slice(0, 2)).toEqual([0, 0]);
    expect(s.workspace.pchFiles()).toEqual([]);
});

// Under the pinned C entry the index projection lexes with the C keyword
// table: `class` goes unpainted.
serve.files({ "dual.c": "class Widget { public: int value; };\n" }, ON)(
    "pinned entry picks the dialect",
    async ({ s }) => {
        // The database names the file twice, which a manifest cannot.
        await s.offline(() => {
            s.workspace.writeEntries([
                ["dual.c", ["-x", "c++"]],
                ["dual.c", ["-x", "c", "-DDIALECT_C"]],
            ]);
        });
        s.open("dual.c");
        await s.indexed();
        const tokens = async () =>
            (
                (await s.request(
                    "textDocument/semanticTokens/full",
                    "dual.c",
                )) as proto.SemanticTokens | null
            )?.data.slice(0, 2);
        expect(await tokens()).toEqual([0, 0]);

        const uri = s.uri("dual.c");
        const { contexts } = await s.client.queryContext(uri);
        const c = contexts.find((context) => context.label.includes("DIALECT_C"))!.commandHash!;
        expect((await s.client.switchContext(uri, uri, { commandHash: c })).success).toBe(true);
        expect(await tokens()).toEqual([0, 6]);
    },
);

serve.files(PROJECT, ON)("readonly on builds no pch", async ({ s }) => {
    s.open("main.cpp");
    await s.indexed();

    // Completion still answers — a full parse without a preamble.
    expect(
        labelsOf(await s.request("textDocument/completion", at("main.cpp", "tw|ice(2)"))),
    ).toContain("twice");

    // The whole point of the profile.
    expect(s.workspace.pchFiles()).toEqual([]);
    expect(await publishes(s, "main.cpp")).toBe(0);
});

serve.files(PROJECT, ON)("readonly on pull builds nothing", async ({ s }) => {
    // The serve fixture's server pushes; a pulling client is a server of
    // its own.
    await s.stop();
    const client = await s.session.spawn(s.workspace).initialize(s.workspace, {
        initializationOptions: ON.config,
        capabilities: { textDocument: { diagnostic: {} } },
    });
    const [uri] = client.open("main.cpp");
    expect(await client.sync()).toMatchObject({ failed: [], pending: [] });
    expect(await client.pullDiagnostics(uri)).toEqual([]);
    expect(s.workspace.pchFiles()).toEqual([]);
});

serve.files(PROJECT, AUTO)("escalation upgrades inlay hints", async ({ s }) => {
    s.open("main.cpp");
    await s.indexed();

    const hints = () => s.request("textDocument/inlayHint", "main.cpp", { range: LINES_0_TO_8 });
    expect((await hints()) ?? []).toEqual([]);

    // The edit flips the mode: the inlay re-pull rides the pulled compile
    // and answers from the AST (parameter names at call sites).
    s.edit("main.cpp", { text: MAIN + "// edited\n" });
    const upgraded = (await hints()) as proto.InlayHint[] | null;
    expect(upgraded?.length ?? 0).toBeGreaterThan(0);
});

// Under readonly "on" a diverged buffer cannot escalate: every index answer
// must withdraw rather than map stale manifest lines onto new text.
serve.files(PROJECT, ON)("diverged buffer serves no links", async ({ s }) => {
    s.open("main.cpp");
    await s.indexed();
    await s.stop();
    await s.start();

    const diverged = '#include "renamed.h"\n' + MAIN.split("\n").slice(1).join("\n");
    s.client.open("main.cpp", 0, { text: diverged });
    expect((await s.request("textDocument/documentLink", "main.cpp")) ?? []).toEqual([]);
    const defs = await s.request("textDocument/definition", "main.cpp", {
        position: { line: 0, character: 12 },
    });
    expect(defs === null || (Array.isArray(defs) && defs.length === 0)).toBe(true);
});

serve.files({ "header.h": HEADER, "main.cpp": "#define LIMIT 10\n" + MAIN }, OFF)(
    "preamble define hovers under pch",
    async ({ s }) => {
        // The define is compiled into the PCH and has no AST node; the null
        // from the worker falls back to the index card for the preamble
        // region (and only there).
        await s.compiled("main.cpp");
        expect(s.show(await s.hover(at("main.cpp", "LIMIT 10")))).toContain("LIMIT");
    },
);

serve.files(PROJECT, OFF)("off compiles on demand", async ({ s }) => {
    // didOpen alone starts nothing (the pre-readonly contract): the
    // diagnostics push rides the first read's pulled compile.
    s.client.open("main.cpp");
    await s.sync();
    expect(await publishes(s, "main.cpp")).toBe(0);

    expect(s.show(await s.hover(ADD))).toContain("add");
    await pushedNoErrors(s, "main.cpp");
});

serve.files(PROJECT, OFF)("index answers while pull compile runs", async ({ s }) => {
    // Warm the index, then restart: the second server starts with the
    // shard on disk and nothing compiled.
    await s.indexed();
    await s.stop();
    await s.start();

    // off: the first read pulls the compile, but must not block on it —
    // the warm shard answers instantly, and the detached pull still lands
    // the AST (diagnostics prove it).
    s.client.open("main.cpp");
    expect(symbolNames(await s.request("textDocument/documentSymbol", "main.cpp"))).toContain(
        "twice",
    );
    await pushedNoErrors(s, "main.cpp");
});

serve.files(
    {
        "a.h": "#pragma once\nint alpha();\n",
        "b.h": '#pragma once\n#include "a.h"\nint beta();\n',
        "main.cpp": '#include "b.h"\n#include "a.h"\nint main() { return alpha() + beta(); }\n',
    },
    AUTO,
)("index links guard-skipped includes", async ({ s }) => {
    s.open("main.cpp");
    await s.indexed();

    const links = (await s.request("textDocument/documentLink", "main.cpp")) as
        proto.DocumentLink[] | null;
    const targets = (links ?? []).map((link) => link.target?.split("/").pop()).sort();
    expect(targets).toEqual(["a.h", "b.h"]);
});
