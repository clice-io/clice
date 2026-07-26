/// Integration tests for automatic self-containment detection.
///
/// Headers without a CDB entry compile self-contained first (borrowed host
/// command, no prefix synthesis). When the trial diagnostics indicate missing
/// includer context, the server falls back to prefix synthesis transparently —
/// only the final diagnostics are published. Verdicts and user context
/// choices persist across server sessions via cache.json.

import * as fs from "node:fs";
import * as path from "node:path";
import { assertCleanCompile, sleep, waitForRecompile } from "../../tools/checks.ts";
import { writeCdb, writeEntries } from "../../tools/compile_commands.ts";
import { cliceExecutable, expect, test } from "../../tools/fixtures.ts";
import { makeClient, shutdownClient } from "../../tools/lifecycle.ts";
import { makeTempWorkspace, readCacheJson } from "../../tools/workspace.ts";

function prefixFiles(workspace: string): string[] {
    const prefixDir = path.join(workspace, ".clice", "header_context");
    if (!fs.existsSync(prefixDir)) {
        return [];
    }
    return fs
        .readdirSync(prefixDir)
        .filter(
            (name) =>
                name.endsWith(".h") && !name.endsWith(".suffix.h") && !name.endsWith(".self.h"),
        )
        .sort()
        .map((name) => path.join(prefixDir, name));
}

test("self contained skips synthesis", async ({ session }) => {
    /// A self-contained header borrows a command but gets no prefix.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(
        path.join(workspace, "types.h"),
        "#pragma once\nstruct Point { int x; int y; };\n",
    );
    fs.writeFileSync(
        path.join(workspace, "helper.h"),
        '#pragma once\n#include "types.h"\ninline int get_x(Point p) { return p.x; }\n',
    );
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "helper.h"\nint main() { return get_x({1, 2}); }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait(path.join(workspace, "main.cpp"));
    const [helperUri] = await client.openAndWait(path.join(workspace, "helper.h"));
    assertCleanCompile(client, helperUri);
    expect(prefixFiles(workspace), "Self-contained headers must not synthesize a prefix").toEqual(
        [],
    );
});

test("fallback on missing context", async ({ session }) => {
    /// A non-self-contained header falls back to prefix synthesis
    /// automatically; the trial's error diagnostics are never published.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(
        path.join(workspace, "types.h"),
        "#pragma once\nstruct Point { int x; int y; };\n",
    );
    fs.writeFileSync(
        path.join(workspace, "utils.h"),
        "inline int get_x(Point p) { return p.x; }\n",
    );
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "types.h"\n#include "utils.h"\nint main() { return get_x({1, 2}); }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait(path.join(workspace, "main.cpp"));
    const [utilsUri] = await client.openAndWait(path.join(workspace, "utils.h"));
    assertCleanCompile(client, utilsUri);
    expect(prefixFiles(workspace).length, "Fallback must synthesize exactly one prefix").toBe(1);
});

test("verdict persisted across sessions", async () => {
    /// The NeedsContext verdict lands in cache.json and survives restarts.
    const { workspace, track } = makeTempWorkspace();
    fs.writeFileSync(
        path.join(workspace, "types.h"),
        "#pragma once\nstruct Point { int x; int y; };\n",
    );
    fs.writeFileSync(
        path.join(workspace, "utils.h"),
        "inline int get_x(Point p) { return p.x; }\n",
    );
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "types.h"\n#include "utils.h"\nint main() { return get_x({1, 2}); }\n',
    );
    writeCdb(workspace, ["main.cpp"]);

    const c1 = track(await makeClient(cliceExecutable(), workspace));
    await c1.openAndWait(path.join(workspace, "main.cpp"));
    const [utilsUri] = await c1.openAndWait(path.join(workspace, "utils.h"));
    assertCleanCompile(c1, utilsUri);
    await shutdownClient(c1);

    const cache = readCacheJson(workspace);
    expect(cache, `Expected a persisted header mode, got: ${JSON.stringify(cache)}`).not.toBeNull();
    expect(
        ((cache!["header_modes"] ?? []) as unknown[]).length,
        `Expected a persisted header mode, got: ${JSON.stringify(cache)}`,
    ).toBeGreaterThan(0);

    const c2 = track(await makeClient(cliceExecutable(), workspace));
    await c2.openAndWait(path.join(workspace, "main.cpp"));
    const [utilsUri2] = await c2.openAndWait(path.join(workspace, "utils.h"));
    assertCleanCompile(c2, utilsUri2);
    await shutdownClient(c2);
});

test("choice persisted across sessions", async () => {
    /// A switchContext choice is restored on didOpen in a later session.
    const { workspace, track } = makeTempWorkspace();
    fs.writeFileSync(path.join(workspace, "shared.h"), "VALUE_TYPE get_value();\n");
    fs.writeFileSync(
        path.join(workspace, "a.cpp"),
        '#define VALUE_TYPE int\n#include "shared.h"\nint main() { return 0; }\n',
    );
    fs.writeFileSync(
        path.join(workspace, "b.cpp"),
        '#define VALUE_TYPE float\n#include "shared.h"\nfloat f() { return 0; }\n',
    );
    writeEntries(workspace, [
        ["a.cpp", []],
        ["b.cpp", []],
    ]);

    const c1 = track(await makeClient(cliceExecutable(), workspace));
    await c1.openAndWait(path.join(workspace, "a.cpp"));
    await c1.openAndWait(path.join(workspace, "b.cpp"));
    const [sharedUri] = c1.open(path.join(workspace, "shared.h"));
    const bUri = c1.pathToUri(path.join(workspace, "b.cpp"));
    const sw = (await c1.switchContext(sharedUri, bUri)) as { success?: boolean };
    expect(sw.success).toBe(true);
    await shutdownClient(c1);

    const c2 = track(await makeClient(cliceExecutable(), workspace));
    const [sharedUri2] = c2.open(path.join(workspace, "shared.h"));
    const current = (await c2.currentContext(sharedUri2)) as { context?: { uri: string } | null };
    const ctx = current.context;
    expect(
        ctx?.uri.includes("b.cpp") ?? false,
        `Persisted context choice should be restored on didOpen, got: ${JSON.stringify(current)}`,
    ).toBe(true);
    await shutdownClient(c2);
});

test("ordinary error no fallback", async ({ session }) => {
    /// A self-contained header with a benign syntax error must not trigger
    /// prefix synthesis nor persist any verdict.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "typo.h"), "inline int broken() { return }\n"); // syntax error
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "typo.h"\nint main() { return 0; }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    const [typoUri] = await client.openAndWait(path.join(workspace, "typo.h"));
    const diags = client.diagnostics.get(typoUri) ?? [];
    expect(diags.length, "The syntax error must be published").toBeGreaterThan(0);
    expect(prefixFiles(workspace), "Ordinary errors must not trigger prefix synthesis").toEqual([]);
});

test("header save resets verdict", async () => {
    /// Saving the header itself re-evaluates its self-containment: a header
    /// that gains its own include stops using the synthesized prefix.
    const { workspace, track } = makeTempWorkspace();
    fs.writeFileSync(
        path.join(workspace, "types.h"),
        "#pragma once\nstruct Point { int x; int y; };\n",
    );
    const utilsH = path.join(workspace, "utils.h");
    fs.writeFileSync(utilsH, "inline int get_x(Point p) { return p.x; }\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "types.h"\n#include "utils.h"\nint main() { return get_x({1, 2}); }\n',
    );
    writeCdb(workspace, ["main.cpp"]);

    const c = track(await makeClient(cliceExecutable(), workspace));
    await c.openAndWait(path.join(workspace, "main.cpp"));
    const [utilsUri] = await c.openAndWait(utilsH);
    assertCleanCompile(c, utilsUri);
    expect(prefixFiles(workspace).length, "Initial verdict: needs context").toBe(1);

    // Make the header self-contained on disk and in the buffer, then save.
    await sleep(1_100);
    const newText = '#include "types.h"\ninline int get_x(Point p) { return p.x; }\n';
    fs.writeFileSync(utilsH, newText);
    c.change(utilsUri, 2, newText);
    c.save(utilsUri);

    await waitForRecompile(c, utilsUri);
    assertCleanCompile(c, utilsUri);
    await shutdownClient(c);

    // After shutdown the persisted verdict must be gone.
    const cache = readCacheJson(workspace);
    expect(
        (cache?.["header_modes"] ?? []) as unknown[],
        `Verdict should be reset after the header was saved, got: ${JSON.stringify(cache)}`,
    ).toEqual([]);
});

test("dependency change retries trial", async ({ session }) => {
    /// A header judged self-contained must be re-evaluated when one of its
    /// own includes changes: here foo.h stops providing FOO, and only the
    /// includer context (the host's define) can still supply it.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "foo.h"), "#pragma once\n#define FOO 1\n");
    fs.writeFileSync(
        path.join(workspace, "h.h"),
        '#pragma once\n#include "foo.h"\ninline int get() { return FOO; }\n',
    );
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#define FOO 2\n#include "h.h"\nint main() { return get(); }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait(path.join(workspace, "main.cpp"));
    const [hUri] = await client.openAndWait(path.join(workspace, "h.h"));
    assertCleanCompile(client, hUri);
    expect(prefixFiles(workspace), "Initially self-contained").toEqual([]);

    // foo.h stops defining FOO; only the host's #define can provide it now.
    await sleep(1_100);
    fs.writeFileSync(path.join(workspace, "foo.h"), "#pragma once\n");

    await waitForRecompile(client, hUri);
    assertCleanCompile(client, hUri);
    expect(
        prefixFiles(workspace).length,
        "Dependency change must re-run the trial and fall back to synthesis",
    ).toBe(1);
});

test("suffix closes embedding", async ({ session }) => {
    /// X-macro fragments embedded in an enum or a function body compile
    /// cleanly: the synthesized suffix closes the surrounding braces.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(
        path.join(workspace, "errors.def"),
        'X(Ok, 0, "success")\nX(NotFound, 1, "not found")\n',
    );
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        "#define X(name, code, msg) name = code,\n" +
            "enum ErrorCode {\n" +
            '#include "errors.def"\n' +
            "};\n" +
            "#undef X\n" +
            "int main() { return Ok; }\n",
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait(path.join(workspace, "main.cpp"));
    const [defUri] = await client.openAndWait(path.join(workspace, "errors.def"));
    assertCleanCompile(client, defUri);
});

test("suffix function body", async ({ session }) => {
    /// The doc's classic register_all() case: statements expanded inside a
    /// function body, closing brace restored by the suffix.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "handlers.def"), "X(alpha)\nX(beta)\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        "inline void handle(int) {}\n" +
            "enum Ids { alpha, beta };\n" +
            "void register_all() {\n" +
            "#define X(name) handle(name);\n" +
            '#include "handlers.def"\n' +
            "#undef X\n" +
            "}\n" +
            "int main() { register_all(); return 0; }\n",
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait(path.join(workspace, "main.cpp"));
    const [defUri] = await client.openAndWait(path.join(workspace, "handlers.def"));
    assertCleanCompile(client, defUri);
});

test("open synthesized artifact", async ({ session }) => {
    /// Opening a synthesized prefix file compiles it with its host's
    /// command (it is a fragment of that TU), not with junk context.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(
        path.join(workspace, "types.h"),
        "#pragma once\nstruct Point { int x; int y; };\n",
    );
    fs.writeFileSync(
        path.join(workspace, "utils.h"),
        "inline int get_x(Point p) { return p.x; }\n",
    );
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "types.h"\n#include "utils.h"\nint main() { return get_x({1, 2}); }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait(path.join(workspace, "main.cpp"));
    const [utilsUri] = await client.openAndWait(path.join(workspace, "utils.h"));
    assertCleanCompile(client, utilsUri);

    const prefixes = prefixFiles(workspace);
    expect(prefixes.length).toBe(1);
    const [prefixUri] = await client.openAndWait(prefixes[0]!);
    assertCleanCompile(client, prefixUri);

    // No context of its own, and no further synthesis chained off it.
    const q = (await client.queryContext(prefixUri)) as { total: number };
    expect(q.total, JSON.stringify(q)).toBe(0);
    expect(prefixFiles(workspace).length, "Opening an artifact must not synthesize more").toBe(1);
});

test("unbalanced brace degrades gracefully", async ({ session }) => {
    /// A user-typed unbalanced brace in an embedded fragment steals the
    /// suffix's closer: diagnostics must appear, and the server must keep
    /// serving requests afterwards.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "list.def"), "X(alpha)\nvoid oops() {\n"); // unbalanced {
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        "#define X(name) int name;\n" +
            "enum Ids {\n" +
            '#include "list.def"\n' +
            "};\n" +
            "#undef X\n" +
            "int main() { return 0; }\n",
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait(path.join(workspace, "main.cpp"));
    const [defUri] = await client.openAndWait(path.join(workspace, "list.def"));
    const diags = client.diagnostics.get(defUri) ?? [];
    expect(diags.length, "The imbalance must surface as diagnostics").toBeGreaterThan(0);

    // The server stays healthy: a follow-up request still answers.
    const q = (await client.queryContext(defUri)) as { total: number };
    expect(q.total).toBeGreaterThanOrEqual(1);
});
