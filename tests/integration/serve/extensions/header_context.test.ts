/// Integration tests for header context LSP extension commands.
///
/// Tests the clice/queryContext, clice/currentContext, and clice/switchContext
/// extension commands that allow switching the compilation context for header
/// files.
///
/// utils.h uses Point without including types.h itself -- it depends on
/// main.cpp to provide that include. Without header context resolution, the
/// server cannot compile utils.h at all.

import * as proto from "vscode-languageserver-protocol";
import { at, expect, serve } from "../../fixtures.ts";
import {
    wireKeys,
    type CurrentContextResult,
    type SwitchContextResult,
} from "@clice/tools/protocol";

const test = serve.data("header_context");

/// clice/queryContext on a header should return source files that include it.
test("query context returns host sources", async ({ s }) => {
    await s.compiled("main.cpp");
    s.open("utils.h");

    const result = await s.client.queryContext(s.uri("utils.h"));
    expect(result).not.toBeNull();
    const { total, contexts } = result;
    expect(
        total,
        `Should find at least main.cpp as context, got total=${total}`,
    ).toBeGreaterThanOrEqual(1);
    // Check that main.cpp is among the contexts.
    const uris = contexts.map((c) => c.uri);
    expect(
        uris.some((u) => u.includes("main.cpp")),
        `main.cpp should be listed as a context option, got: ${uris.join(", ")}`,
    ).toBe(true);
});

/// clice/queryContext on a source file should return its CDB entries.
test("query context source file returns cdb entries", async ({ s }) => {
    await s.compiled("main.cpp");

    const result = await s.client.queryContext(s.uri("main.cpp"));
    expect(result).not.toBeNull();
    // header_context workspace has exactly 1 CDB entry for main.cpp.
    expect(result.total).toBe(1);
    expect(result.contexts.length).toBe(1);
});

/// Without a choice, clice/currentContext names the host picked
/// automatically, as the listing does.
test("current context automatic", async ({ s }) => {
    await s.compiled("main.cpp");
    s.open("utils.h");
    const utils = s.uri("utils.h");

    const result = await s.client.currentContext(utils);
    expect(Object.keys(result).sort()).toEqual(
        [...wireKeys<CurrentContextResult>()(["automatic", "context", "epoch"])].sort(),
    );
    expect(result.automatic).toBe(true);
    const listed = await s.client.queryContext(utils);
    expect(result.context).toEqual(listed.contexts.find((c) => c.uri.includes("main.cpp")));
    expect(result.epoch).toBe(listed.epoch);
});

/// switchContext should set the active context, currentContext should reflect it.
test("switch context and current context", async ({ s }) => {
    await s.compiled("main.cpp");
    s.open("utils.h");
    const utils = s.uri("utils.h");

    // Switch context to main.cpp.
    const switchResult = await s.client.switchContext(utils, s.uri("main.cpp"));
    expect(switchResult).not.toBeNull();
    expect(switchResult.success).toBe(true);
    // The C++ and TS shapes of the reply are hand-written on both sides; a
    // field added to one and not the other shows up here.
    expect(Object.keys(switchResult).sort()).toEqual(
        [...wireKeys<SwitchContextResult>()(["stale", "success"])].sort(),
    );

    // Verify currentContext now returns main.cpp.
    const current = await s.client.currentContext(utils);
    expect(current).not.toBeNull();
    const ctx = current.context;
    expect(
        ctx,
        "After switchContext, currentContext should return the active context",
    ).not.toBeNull();
    expect(ctx!.uri).toContain("main.cpp");
    expect(current.automatic).toBe(false);
});

/// Full flow: open, query, switch, verify hover works in header context.
test("full context flow", async ({ s }) => {
    // 1. Open main.cpp, wait for initial compile.
    await s.compiled("main.cpp");

    // 2. Open utils.h (non self-contained header using Point from types.h).
    s.open("utils.h");
    const utils = s.uri("utils.h");

    // 3. queryContext on utils.h -> should return main.cpp as a context option.
    const query = await s.client.queryContext(utils);
    expect(query.total).toBeGreaterThanOrEqual(1);
    const contextUris = query.contexts.map((c) => c.uri);
    expect(contextUris.some((u) => u.includes("main.cpp"))).toBe(true);

    // 4. currentContext on utils.h -> picked automatically.
    const current = await s.client.currentContext(utils);
    expect(current.automatic).toBe(true);

    // 5. switchContext on utils.h to main.cpp.
    const switched = await s.client.switchContext(utils, s.uri("main.cpp"));
    expect(switched.success).toBe(true);

    // 6. currentContext on utils.h -> should now be main.cpp.
    const current2 = await s.client.currentContext(utils);
    expect(current2.automatic).toBe(false);
    const ctx = current2.context;
    expect(ctx).not.toBeNull();
    expect(ctx!.uri).toContain("main.cpp");

    // 7. Hover on 'calc' function in utils.h -> should work (proves header compiled).
    const hover = await s.hover(at("utils.h", "int c|alc"));
    expect(hover, "Hover on 'calc' in header should work after switchContext").not.toBeNull();

    // 8. Check diagnostics on utils.h -> should have 0 errors.
    const errors = await s.errors("utils.h");
    expect(
        errors.length,
        `Header should have no errors after switchContext, got: ${JSON.stringify(errors)}`,
    ).toBe(0);
});

/// Document highlights in a header name what its includer's context declares.
test("document highlight in context", async ({ s }) => {
    await s.compiled("main.cpp");
    await s.compiled("utils.h");

    const point = await s.request("textDocument/documentHighlight", at("utils.h", "calc(Po|int"));
    expect(point).toEqual([
        {
            range: { start: { line: 6, character: 16 }, end: { line: 6, character: 21 } },
            kind: proto.DocumentHighlightKind.Read,
        },
    ]);

    const param = (await s.request(
        "textDocument/documentHighlight",
        at("utils.h", "distance(|p,"),
    )) as proto.DocumentHighlight[] | null;
    expect(param?.map((h) => [h.range.start.line, h.range.start.character, h.kind])).toEqual([
        [6, 22, proto.DocumentHighlightKind.Text],
        [7, 20, proto.DocumentHighlightKind.Read],
    ]);
});

/// queryContext on a deeply nested header (main.cpp -> utils.h -> inner.h)
/// should still find main.cpp as the host source.
test("deep nested header context", async ({ s }) => {
    await s.compiled("main.cpp");
    s.open("inner.h");

    // queryContext on inner.h should find main.cpp through the chain.
    const result = await s.client.queryContext(s.uri("inner.h"));
    expect(result).not.toBeNull();
    const total = result.total;
    expect(
        total,
        `Deep nested header should find host sources, got total=${total}`,
    ).toBeGreaterThanOrEqual(1);
    const uris = result.contexts.map((c) => c.uri);
    expect(
        uris.some((u) => u.includes("main.cpp")),
        `main.cpp should be a context for inner.h, got: ${uris.join(", ")}`,
    ).toBe(true);
});

/// switchContext + hover on deeply nested header (main.cpp -> utils.h -> inner.h).
test("deep nested switch context and hover", async ({ s }) => {
    await s.compiled("main.cpp");
    s.open("inner.h");

    // Switch inner.h context to main.cpp.
    const switched = await s.client.switchContext(s.uri("inner.h"), s.uri("main.cpp"));
    expect(switched.success).toBe(true);

    // Hover on 'inner_origin' in inner.h should work (Point available via preamble).
    const hover = await s.hover(at("inner.h", "Point i|nner_origin"));
    expect(hover, "Hover on inner_origin should work after switchContext").not.toBeNull();
});

/// queryContext on a source file with multiple CDB entries should return all.
serve.data("multi_context")("query context multiple cdb entries", async ({ s }) => {
    await s.compiled("main.cpp");

    const result = await s.client.queryContext(s.uri("main.cpp"));
    expect(result).not.toBeNull();
    const total = result.total;
    expect(total, `Should find at least 2 CDB entries, got total=${total}`).toBeGreaterThanOrEqual(
        2,
    );
    const labels = result.contexts.map((c) => c.label);
    // Each entry should have distinguishing flags in the label.
    expect(
        labels.some((l) => l.includes("CONFIG_A")),
        `Should find CONFIG_A, got: ${labels.join(", ")}`,
    ).toBe(true);
    expect(
        labels.some((l) => l.includes("CONFIG_B")),
        `Should find CONFIG_B, got: ${labels.join(", ")}`,
    ).toBe(true);
});

/// Switching a header between two hosts with different macro setups must
/// recompile it under the new host's preamble.
serve.files({
    "shared.h": "VALUE_TYPE get_value();\n",
    "a.cpp": '#define VALUE_TYPE int\n#include "shared.h"\nint main() { return 0; }\n',
    "b.cpp": '#define VALUE_TYPE float\n#include "shared.h"\nfloat f() { return 0; }\n',
})("switch between two hosts", async ({ s }) => {
    const a = s.uri("a.cpp");
    const b = s.uri("b.cpp");
    const shared = s.uri("shared.h");
    await s.compiled("a.cpp");
    await s.compiled("b.cpp");
    s.open("shared.h");
    expect((await s.client.currentContext(shared)).context?.uri).toBe(a);

    /// switchContext only flips server state; a diagnostics pull waits for
    /// the recompile under the new host.
    const hoverGetValueShows = async (text: string) => {
        await s.diagnostics("shared.h");
        expect(s.show(await s.hover(at("shared.h", "get_|value")))).toContain(text);
    };

    // Host a.cpp: VALUE_TYPE is int.
    let switched = await s.client.switchContext(shared, a);
    expect(switched.success).toBe(true);
    await hoverGetValueShows("int");

    // Host b.cpp: VALUE_TYPE is float. This exercises the cached-context
    // invalidation branch (active context differs from cached host).
    switched = await s.client.switchContext(shared, b);
    expect(switched.success).toBe(true);
    await hoverGetValueShows("float");

    // The reset leaves the cached b.cpp context for the one picked
    // automatically.
    expect((await s.client.resetContext(shared)).success).toBe(true);
    await hoverGetValueShows("int");
    const current = await s.client.currentContext(shared);
    expect(current.automatic).toBe(true);
    expect(current.context?.uri).toBe(a);
});

/// A switch and its reset keep the document open: the server recompiles
/// the unchanged text and publishes for it.
test("switch republishes diagnostics", async ({ s }) => {
    await s.compiled("main.cpp");
    await s.compiled("utils.h");
    const utils = s.uri("utils.h");
    const published = async () => (await s.counts()).files["utils.h"]?.publish ?? 0;

    let before = await published();
    expect((await s.client.switchContext(utils, s.uri("main.cpp"))).success).toBe(true);
    await s.sync();
    expect(await published(), "diagnostics after the switch").toBeGreaterThan(before);
    expect((await s.client.currentContext(utils)).automatic).toBe(false);

    before = await published();
    expect((await s.client.resetContext(utils)).success).toBe(true);
    await s.sync();
    expect(await published(), "diagnostics after the reset").toBeGreaterThan(before);
    const current = await s.client.currentContext(utils);
    expect(current.automatic).toBe(true);
    expect(current.context?.uri).toContain("main.cpp");
});

/// A source a rule's default command claims compiles under that command,
/// which the listing does not offer, even where another unit includes it.
serve.files(
    {
        "clice.toml": '[[rules]]\npatterns = ["src/**"]\ndefault_command = "clang++ -std=c++20"\n',
        "src/part.cpp": "int part() { return 1; }\n",
        "src/main.cpp": '#include "part.cpp"\nint main() { return part(); }\n',
    },
    { manifest: { units: {} } },
)("default command unit names no host", async ({ s }) => {
    await s.compiled("src/main.cpp");
    await s.compiled("src/part.cpp");
    const current = await s.client.currentContext(s.uri("src/part.cpp"));
    expect(current.automatic).toBe(true);
    expect(current.context).toBeNull();
});
