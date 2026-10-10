/// The readonly serving modes: reads are answered from the index while
/// PCH/AST builds stay pull-driven — the mode only decides whether they
/// are a goal at all. Each degraded surface the design pins (empty inlay
/// hints, no diagnostics push before a pull) is asserted explicitly.
///
/// `s.open` pulls diagnostics, which is itself a read that pulls a compile
/// under "off"; a case pinning what the first read or the open itself
/// starts opens without that pull.

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

function labelsOf(list: proto.CompletionItem[] | proto.CompletionList | null): string[] {
    if (list === null) {
        return [];
    }
    const items = Array.isArray(list) ? list : list.items;
    return items.map((item) => item.label);
}

async function symbolNames(s: Serve, file: string): Promise<string[] | undefined> {
    const symbols = await s.request<proto.DocumentSymbol[] | null>(
        "textDocument/documentSymbol",
        file,
    );
    return symbols?.map((symbol) => symbol.name);
}

function links(s: Serve, file: string): Promise<proto.DocumentLink[] | null> {
    return s.request<proto.DocumentLink[] | null>("textDocument/documentLink", file);
}

async function linksTo(s: Serve, file: string, target: string): Promise<boolean | undefined> {
    return (await links(s, file))?.some((link) => link.target?.endsWith(target));
}

function semanticTokens(s: Serve, file: string): Promise<proto.SemanticTokens | null> {
    return s.request<proto.SemanticTokens | null>("textDocument/semanticTokens/full", file);
}

/// The diagnostics push of a compile some read pulled has landed, and it
/// carries no error.
async function pushedNoErrors(s: Serve, file: string): Promise<void> {
    const pushed = await s.pushed(file);
    expect(pushed, "a compile was pushed").toBeDefined();
    expect(pushed!.filter((d) => d.severity === proto.DiagnosticSeverity.Error)).toEqual([]);
}

serve.files(PROJECT, AUTO)("index serves unedited reads", async ({ s }) => {
    s.open("main.cpp");
    await s.indexed();

    const tokens = await semanticTokens(s, "main.cpp");
    expect(tokens?.data.length ?? 0).toBeGreaterThan(0);

    expect(await symbolNames(s, "main.cpp")).toContain("twice");

    const folds = await s.request<proto.FoldingRange[] | null>(
        "textDocument/foldingRange",
        "main.cpp",
    );
    expect(folds?.length ?? 0).toBeGreaterThan(0);

    expect(await linksTo(s, "main.cpp", "header.h")).toBe(true);

    expect(s.show(await s.hover(ADD))).toContain("add");
    expect(s.show(await s.definition(ADD))).toBe("header.h: int add(int a, int b);");

    const x = at("main.cpp", "add(|x, x)");
    const highlights = await s.request<proto.DocumentHighlight[] | null>(
        "textDocument/documentHighlight",
        x,
    );
    expect(highlights?.map((h) => [h.range.start.line, h.range.start.character, h.kind])).toEqual([
        [3, 14, proto.DocumentHighlightKind.Text],
        [4, 15, proto.DocumentHighlightKind.Read],
        [4, 18, proto.DocumentHighlightKind.Read],
    ]);

    // Without an AST, selection ranges come from the text alone.
    const [selection] =
        (await s.request<proto.SelectionRange[] | null>("textDocument/selectionRange", "main.cpp", {
            positions: [s.position(x).position],
        })) ?? [];
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
    expect(await s.pushed("main.cpp")).toBeUndefined();

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
        await s.request<proto.Location[] | null>("textDocument/typeDefinition", auto),
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

        expect(await symbolNames(s, "main.cpp")).toContain("twice");
        expect(await linksTo(s, "main.cpp", "header.h")).toBe(true);
        expect(s.show(await s.hover(ADD))).toContain("add");

        expect(await semanticTokens(s, "main.cpp")).toBeNull();
        expect(await s.request("textDocument/foldingRange", "main.cpp")).toEqual([]);
        expect(s.workspace.pchFiles()).toEqual([]);
    },
);

serve.files(PROJECT, AUTO)("cold outline awaits the boost", async ({ s }) => {
    // No wait for the index: outline and links have no refresh request, so
    // the replies themselves await the didOpen boost instead of freezing an
    // empty result in the client's cache.
    s.open("main.cpp", { pull: false });
    const [symbols, linked] = await Promise.all([
        symbolNames(s, "main.cpp"),
        linksTo(s, "main.cpp", "header.h"),
    ]);
    expect(symbols).toContain("twice");
    expect(linked).toBe(true);
});

serve.files(PROJECT, AUTO)("edit escalates to compile", async ({ s }) => {
    s.open("main.cpp");
    await s.indexed();
    expect(await s.pushed("main.cpp")).toBeUndefined();

    // The edit flips the mode; the build itself stays pull-driven, so
    // nothing lands until the next read pulls it.
    s.edit("main.cpp", { text: MAIN + "// edited\n" });
    expect(await s.pushed("main.cpp")).toBeUndefined();

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
    s.open("main.cpp", { text: MAIN + "// restored, unsaved\n", pull: false });
    expect(await symbolNames(s, "main.cpp")).toContain("twice");
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
    s.open("orphan.cpp", { pull: false });
    expect(await symbolNames(s, "orphan.cpp")).toContain("orphan");
    await pushedNoErrors(s, "orphan.cpp");

    s.open("scratch.cpp", { text: "int scratch() { return 2; }\n", pull: false });
    expect(await symbolNames(s, "scratch.cpp")).toContain("scratch");
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
    expect((await semanticTokens(s, "legacy.c"))?.data.slice(0, 2)).toEqual([0, 0]);
    expect(s.workspace.pchFiles()).toEqual([]);
});

// Under the pinned C entry the index projection lexes with the C keyword
// table: `class` goes unpainted.
serve.files(
    { "dual.c": "class Widget { public: int value; };\n" },
    {
        ...ON,
        manifest: {
            units: {
                "dual.c": [
                    ["-x", "c++"],
                    ["-x", "c", "-DDIALECT_C"],
                ],
            },
        },
    },
)("pinned entry picks the dialect", async ({ s }) => {
    s.open("dual.c");
    await s.indexed();
    const tokens = async () => (await semanticTokens(s, "dual.c"))?.data.slice(0, 2);
    expect(await tokens()).toEqual([0, 0]);

    const { contexts } = await s.contexts("dual.c");
    const c = contexts.find((context) => context.label.includes("DIALECT_C"))!.commandHash!;
    expect((await s.switchContext("dual.c", "dual.c", { commandHash: c })).success).toBe(true);
    expect(await tokens()).toEqual([0, 6]);
});

serve.files(PROJECT, ON)("readonly on builds no pch", async ({ s }) => {
    s.open("main.cpp");
    await s.indexed();

    // Completion still answers — a full parse without a preamble.
    expect(labelsOf(await s.completion(at("main.cpp", "tw|ice(2)")))).toContain("twice");

    // The whole point of the profile.
    expect(s.workspace.pchFiles()).toEqual([]);
    expect(await s.pushed("main.cpp")).toBeUndefined();
});

serve.files(PROJECT, { ...ON, launch: { capabilities: { textDocument: { diagnostic: {} } } } })(
    "readonly on pull builds nothing",
    async ({ s }) => {
        s.open("main.cpp", { pull: false });
        await s.indexed();
        expect(await s.diagnostics("main.cpp")).toEqual([]);
        expect(s.workspace.pchFiles()).toEqual([]);
    },
);

serve.files(PROJECT, AUTO)("escalation upgrades inlay hints", async ({ s }) => {
    s.open("main.cpp");
    await s.indexed();

    const hints = () =>
        s.request<proto.InlayHint[] | null>("textDocument/inlayHint", "main.cpp", {
            range: LINES_0_TO_8,
        });
    expect((await hints()) ?? []).toEqual([]);

    // The edit flips the mode: the inlay re-pull rides the pulled compile
    // and answers from the AST (parameter names at call sites).
    s.edit("main.cpp", { text: MAIN + "// edited\n" });
    expect((await hints())?.length ?? 0).toBeGreaterThan(0);
});

// Under readonly "on" a diverged buffer cannot escalate: every index answer
// must withdraw rather than map stale manifest lines onto new text.
serve.files(PROJECT, AUTO)("diverged buffer serves no links", async ({ s }) => {
    s.open("main.cpp");
    await s.indexed();
    await s.stop();
    await s.start(ON);

    const diverged = '#include "renamed.h"\n' + MAIN.split("\n").slice(1).join("\n");
    s.open("main.cpp", { text: diverged, pull: false });
    expect((await links(s, "main.cpp")) ?? []).toEqual([]);
    const defs = await s.definition(at("main.cpp", '"re|named.h"'));
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
    s.open("main.cpp", { pull: false });
    expect(await s.pushed("main.cpp")).toBeUndefined();

    expect(s.show(await s.hover(ADD))).toContain("add");
    await pushedNoErrors(s, "main.cpp");
});

serve.files(PROJECT, AUTO)("index answers while pull compile runs", async ({ s }) => {
    // Warm the index, then restart: the second server starts with the
    // shard on disk and nothing compiled.
    await s.indexed();
    await s.stop();
    await s.start(OFF);

    // off: the first read pulls the compile, but must not block on it —
    // the warm shard answers instantly, and the detached pull still lands
    // the AST (diagnostics prove it).
    s.open("main.cpp", { pull: false });
    expect(await symbolNames(s, "main.cpp")).toContain("twice");
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

    const targets = ((await links(s, "main.cpp")) ?? [])
        .map((link) => link.target?.split("/").pop())
        .sort();
    expect(targets).toEqual(["a.h", "b.h"]);
});
