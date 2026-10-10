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

const AUTO = { config: { project: { readonly: "auto" } } };
const ON = { config: { project: { readonly: "on" } } };
const OFF = { config: { project: { readonly: "off" } } };

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

/// The inlay hints over the whole of `file`.
function inlayHints(s: Serve, file: string): Promise<proto.InlayHint[] | null> {
    const end = { line: s.disk.read(file).split("\n").length, character: 0 };
    return s.request<proto.InlayHint[] | null>("textDocument/inlayHint", file, {
        range: { start: { line: 0, character: 0 }, end },
    });
}

/// The text of `file` that `range` covers.
function covered(s: Serve, file: string, range: proto.Range): string {
    const lines = s.disk.read(file).split("\n");
    const offset = (position: proto.Position) =>
        lines.slice(0, position.line).reduce((sum, line) => sum + line.length + 1, 0) +
        position.character;
    return lines.join("\n").slice(offset(range.start), offset(range.end));
}

/// The diagnostics push of a compile some read pulled has landed, and it
/// carries no error.
async function pushedNoErrors(s: Serve, file: string): Promise<void> {
    const pushed = await s.pushed(file);
    expect(pushed, "a compile was pushed").toBeDefined();
    expect(pushed!.filter((d) => d.severity === proto.DiagnosticSeverity.Error)).toEqual([]);
}

serve("shapes/headers", AUTO)("index serves unedited reads", async ({ s }) => {
    const main = s.file("main");
    const demo = s.file("demo");
    s.open(main);
    s.open(demo);
    await s.indexed();

    const tokens = await semanticTokens(s, main);
    expect(tokens?.data.length ?? 0).toBeGreaterThan(0);

    expect(await symbolNames(s, main)).toContain("main");

    const folds = await s.request<proto.FoldingRange[] | null>("textDocument/foldingRange", main);
    expect(folds?.length ?? 0).toBeGreaterThan(0);

    expect(await linksTo(s, main, "circle.h")).toBe(true);

    const circle = at(s.file("main"), "shapes::Ci|rcle c");
    expect(s.show(await s.hover(circle))).toContain("Circle");
    expect(s.show(await s.definition(circle))).toBe(
        "include/shapes/circle.h: class Circle : public Shape {",
    );

    const c = at(s.file("main"), "area(|c)");
    const highlights = await s.request<proto.DocumentHighlight[] | null>(
        "textDocument/documentHighlight",
        c,
    );
    expect(highlights?.map((h) => [h.range.start, h.kind])).toEqual([
        [
            s.position(at(s.file("main"), "Circle |c(2.0)")).position,
            proto.DocumentHighlightKind.Text,
        ],
        [s.position(c).position, proto.DocumentHighlightKind.Read],
    ]);

    // Without an AST, selection ranges come from the text alone.
    const unit = at(s.file("demo"), "static_cast<int>(u|nit.measure())");
    const [selection] =
        (await s.request<proto.SelectionRange[] | null>("textDocument/selectionRange", demo, {
            positions: [s.position(unit).position],
        })) ?? [];
    const steps: string[] = [];
    for (let step = selection; step; step = step.parent) {
        steps.push(covered(s, demo, step.range));
    }
    const text = s.disk.read(demo);
    const open = text.indexOf("{");
    const body = text.slice(open, text.indexOf("\n}", open) + 2);
    expect(steps).toEqual([
        "unit",
        "unit.measure()",
        "(unit.measure())",
        body.slice(1, -1).trim(),
        body,
    ]);

    // Pinned degradations of the read-only surface.
    expect((await inlayHints(s, main)) ?? []).toEqual([]);
    expect(await s.pushed(main)).toBeUndefined();

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
        expect(s.show(await s.hover(at("main.cpp", "add(x, x)")))).toContain("add");

        expect(await semanticTokens(s, "main.cpp")).toBeNull();
        expect(await s.request("textDocument/foldingRange", "main.cpp")).toEqual([]);
        expect(s.workspace.pchFiles()).toEqual([]);
    },
);

serve("shapes/headers", AUTO)("cold outline awaits the boost", async ({ s }) => {
    // No wait for the index: outline and links have no refresh request, so
    // the replies themselves await the didOpen boost instead of freezing an
    // empty result in the client's cache.
    s.open(s.file("main"), { pull: false });
    const [symbols, linked] = await Promise.all([
        symbolNames(s, s.file("main")),
        linksTo(s, s.file("main"), "circle.h"),
    ]);
    expect(symbols).toContain("main");
    expect(linked).toBe(true);
});

serve("shapes/headers", AUTO)("edit escalates to compile", async ({ s }) => {
    s.open(s.file("main"));
    await s.indexed();
    expect(await s.pushed(s.file("main"))).toBeUndefined();

    // The edit flips the mode; the build itself stays pull-driven, so
    // nothing lands until the next read pulls it.
    s.edit(s.file("main"), { after: "registry_count();\n}\n", insert: "// edited\n" });
    expect(await s.pushed(s.file("main"))).toBeUndefined();

    expect(s.show(await s.hover(at(s.file("main"), "shapes::Ci|rcle c")))).toContain("Circle");
    await pushedNoErrors(s, s.file("main"));
});

serve("tiny", AUTO)("diverged open buffer escalates", async ({ s }) => {
    // Warm the index, then restart: the second server starts with the
    // shard on disk and nothing compiled.
    s.open("main.cpp");
    await s.indexed();
    await s.stop();
    await s.start();

    // A restored unsaved buffer diverges from the indexed content: the
    // open itself escalates, so the first read pulls a compile instead
    // of answering empty from a withdrawn shard.
    s.open("main.cpp", { text: s.disk.read("main.cpp") + "// restored, unsaved\n", pull: false });
    expect(await symbolNames(s, "main.cpp")).toContain("add");
    await pushedNoErrors(s, "main.cpp");
});

// orphan.cpp is on disk but outside the CDB with no includer, so indexing
// refuses the guessed command; scratch.cpp has a CDB entry but exists only
// as the didOpen buffer, and indexing reads disk truth. Neither boost can
// deliver a shard: the failed attempt escalates, and the parked outline
// re-routes to a pulled compile.
serve("tiny", {
    ...AUTO,
    files: { "orphan.cpp": "int orphan() { return 1; }\n" },
    units: { "scratch.cpp": [] },
})("unservable boost escalates", async ({ s }) => {
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

serve("shapes/headers", ON)("readonly on builds no pch", async ({ s }) => {
    s.open(s.file("main"));
    await s.indexed();

    // Completion still answers — a full parse without a preamble.
    expect(labelsOf(await s.completion(at(s.file("main"), "shapes::ar|ea(c)")))).toContain("area");

    // The whole point of the profile.
    expect(s.workspace.pchFiles()).toEqual([]);
    expect(await s.pushed(s.file("main"))).toBeUndefined();
});

serve("shapes/headers", { ...ON, launch: { capabilities: { textDocument: { diagnostic: {} } } } })(
    "readonly on pull builds nothing",
    async ({ s }) => {
        s.open(s.file("main"), { pull: false });
        await s.indexed();
        expect(await s.diagnostics(s.file("main"))).toEqual([]);
        expect(s.workspace.pchFiles()).toEqual([]);
    },
);

serve("tiny", AUTO)("escalation upgrades inlay hints", async ({ s }) => {
    s.open("main.cpp");
    await s.indexed();

    expect((await inlayHints(s, "main.cpp")) ?? []).toEqual([]);

    // The edit flips the mode: the inlay re-pull rides the pulled compile
    // and answers from the AST (parameter names at call sites).
    s.edit("main.cpp", { after: "return value - 3;\n}\n", insert: "// edited\n" });
    expect((await inlayHints(s, "main.cpp"))?.length ?? 0).toBeGreaterThan(0);
});

// Under readonly "on" a diverged buffer cannot escalate: every index answer
// must withdraw rather than map stale manifest lines onto new text.
serve("shapes/headers", AUTO)("diverged buffer serves no links", async ({ s }) => {
    s.open(s.file("main"));
    await s.indexed();
    await s.stop();
    await s.start(ON);

    const diverged = s.disk
        .read(s.file("main"))
        .replace('"shapes/registry.h"', '"shapes/registry.hpp"');
    s.open(s.file("main"), { text: diverged, pull: false });
    expect((await links(s, s.file("main"))) ?? []).toEqual([]);
    const defs = await s.definition(at(s.file("main"), '"shapes/re|gistry.h'));
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

serve("shapes/headers", OFF)("off compiles on demand", async ({ s }) => {
    // didOpen alone starts nothing (the pre-readonly contract): the
    // diagnostics push rides the first read's pulled compile.
    s.open(s.file("main"), { pull: false });
    expect(await s.pushed(s.file("main"))).toBeUndefined();

    expect(s.show(await s.hover(at(s.file("main"), "shapes::Ci|rcle c")))).toContain("Circle");
    await pushedNoErrors(s, s.file("main"));
});

serve("tiny", AUTO)("index answers while pull compile runs", async ({ s }) => {
    // Warm the index, then restart: the second server starts with the
    // shard on disk and nothing compiled.
    await s.indexed();
    await s.stop();
    await s.start(OFF);

    // off: the first read pulls the compile, but must not block on it —
    // the warm shard answers instantly, and the detached pull still lands
    // the AST (diagnostics prove it).
    s.open("main.cpp", { pull: false });
    expect(await symbolNames(s, "main.cpp")).toContain("add");
    await pushedNoErrors(s, "main.cpp");
});

// The test's second include, shape.h, is guard-skipped: shapes.h already
// entered it.
serve("shapes/headers", AUTO)("index links guard-skipped includes", async ({ s }) => {
    s.open(s.file("test"));
    await s.indexed();

    const targets = ((await links(s, s.file("test"))) ?? [])
        .map((link) => link.target?.split("/").pop())
        .sort();
    expect(targets).toEqual(["shape.h", "shapes.h"]);
});
