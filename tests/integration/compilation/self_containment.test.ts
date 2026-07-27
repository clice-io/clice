/// Integration tests for automatic self-containment detection.
///
/// Headers without a CDB entry compile self-contained first (borrowed host
/// command, no prefix synthesis). When the trial diagnostics indicate missing
/// includer context, the server falls back to prefix synthesis transparently —
/// only the final diagnostics are published. Verdicts and user context
/// choices persist across server sessions via cache.json.

import * as fs from "node:fs";
import * as path from "node:path";
import { sleep } from "@clice/tools/client";
import type { Workspace } from "@clice/tools/workspace";
import { expect, test } from "../fixtures.ts";

function prefixFiles(workspace: Workspace): string[] {
    const prefixDir = workspace.path(path.join(".clice", "header_context"));
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
    // A self-contained header borrows a command but gets no prefix.
    const { client, workspace } = session.tmp();
    workspace.write("types.h", "#pragma once\nstruct Point { int x; int y; };\n");
    workspace.write(
        "helper.h",
        '#pragma once\n#include "types.h"\ninline int get_x(Point p) { return p.x; }\n',
    );
    workspace.write("main.cpp", '#include "helper.h"\nint main() { return get_x({1, 2}); }\n');
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait("main.cpp");
    const [helperUri] = await client.openAndWait("helper.h");
    client.assertCleanCompile(helperUri);
    expect(prefixFiles(workspace), "Self-contained headers must not synthesize a prefix").toEqual(
        [],
    );
});

test("fallback on missing context", async ({ session }) => {
    // A non-self-contained header falls back to prefix synthesis
    // automatically; the trial's error diagnostics are never published.
    const { client, workspace } = session.tmp();
    workspace.write("types.h", "#pragma once\nstruct Point { int x; int y; };\n");
    workspace.write("utils.h", "inline int get_x(Point p) { return p.x; }\n");
    workspace.write(
        "main.cpp",
        '#include "types.h"\n#include "utils.h"\nint main() { return get_x({1, 2}); }\n',
    );
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait("main.cpp");
    const [utilsUri] = await client.openAndWait("utils.h");
    client.assertCleanCompile(utilsUri);
    expect(prefixFiles(workspace).length, "Fallback must synthesize exactly one prefix").toBe(1);
});

test("verdict persisted across sessions", async ({ session }) => {
    // The NeedsContext verdict lands in cache.json and survives restarts.
    const workspace = session.tmpdir();
    workspace.write("types.h", "#pragma once\nstruct Point { int x; int y; };\n");
    workspace.write("utils.h", "inline int get_x(Point p) { return p.x; }\n");
    workspace.write(
        "main.cpp",
        '#include "types.h"\n#include "utils.h"\nint main() { return get_x({1, 2}); }\n',
    );
    workspace.writeCDB(["main.cpp"]);

    const c1 = session.spawn(workspace);
    await c1.initialize(workspace);
    await c1.openAndWait("main.cpp");
    const [utilsUri] = await c1.openAndWait("utils.h");
    c1.assertCleanCompile(utilsUri);
    await c1.shutdown();

    const cache = workspace.readCacheJson();
    expect(cache, `Expected a persisted header mode, got: ${JSON.stringify(cache)}`).not.toBeNull();
    expect(
        ((cache!["header_modes"] ?? []) as unknown[]).length,
        `Expected a persisted header mode, got: ${JSON.stringify(cache)}`,
    ).toBeGreaterThan(0);

    const c2 = session.spawn(workspace);
    await c2.initialize(workspace);
    await c2.openAndWait("main.cpp");
    const [utilsUri2] = await c2.openAndWait("utils.h");
    c2.assertCleanCompile(utilsUri2);
    await c2.shutdown();
});

test("choice persisted across sessions", async ({ session }) => {
    // A switchContext choice is restored on didOpen in a later session.
    const workspace = session.tmpdir();
    workspace.write("shared.h", "VALUE_TYPE get_value();\n");
    workspace.write(
        "a.cpp",
        '#define VALUE_TYPE int\n#include "shared.h"\nint main() { return 0; }\n',
    );
    workspace.write(
        "b.cpp",
        '#define VALUE_TYPE float\n#include "shared.h"\nfloat f() { return 0; }\n',
    );
    workspace.writeEntries([
        ["a.cpp", []],
        ["b.cpp", []],
    ]);

    const c1 = session.spawn(workspace);
    await c1.initialize(workspace);
    await c1.openAndWait("a.cpp");
    await c1.openAndWait("b.cpp");
    const [sharedUri] = c1.open("shared.h");
    const bUri = workspace.uri("b.cpp");
    const sw = await c1.switchContext(sharedUri, bUri);
    expect(sw.success).toBe(true);
    await c1.shutdown();

    const c2 = session.spawn(workspace);
    await c2.initialize(workspace);
    const [sharedUri2] = c2.open("shared.h");
    const current = await c2.currentContext(sharedUri2);
    const ctx = current.context;
    expect(
        ctx?.uri.includes("b.cpp") ?? false,
        `Persisted context choice should be restored on didOpen, got: ${JSON.stringify(current)}`,
    ).toBe(true);
    await c2.shutdown();
});

test("ordinary error no fallback", async ({ session }) => {
    // A self-contained header with a benign syntax error must not trigger
    // prefix synthesis nor persist any verdict.
    const { client, workspace } = session.tmp();
    workspace.write("typo.h", "inline int broken() { return }\n"); // syntax error
    workspace.write("main.cpp", '#include "typo.h"\nint main() { return 0; }\n');
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    const [typoUri] = await client.openAndWait("typo.h");
    const diags = client.diagnostics.get(typoUri) ?? [];
    expect(diags.length, "The syntax error must be published").toBeGreaterThan(0);
    expect(prefixFiles(workspace), "Ordinary errors must not trigger prefix synthesis").toEqual([]);
});

test("header save resets verdict", async ({ session }) => {
    // Saving the header itself re-evaluates its self-containment: a header
    // that gains its own include stops using the synthesized prefix.
    const workspace = session.tmpdir();
    workspace.write("types.h", "#pragma once\nstruct Point { int x; int y; };\n");
    workspace.write("utils.h", "inline int get_x(Point p) { return p.x; }\n");
    workspace.write(
        "main.cpp",
        '#include "types.h"\n#include "utils.h"\nint main() { return get_x({1, 2}); }\n',
    );
    workspace.writeCDB(["main.cpp"]);

    const c = session.spawn(workspace);
    await c.initialize(workspace);
    await c.openAndWait("main.cpp");
    const [utilsUri] = await c.openAndWait("utils.h");
    c.assertCleanCompile(utilsUri);
    expect(prefixFiles(workspace).length, "Initial verdict: needs context").toBe(1);

    // Make the header self-contained on disk and in the buffer, then save.
    await sleep(1_100);
    const newText = '#include "types.h"\ninline int get_x(Point p) { return p.x; }\n';
    workspace.write("utils.h", newText);
    c.change(utilsUri, 2, newText);
    c.save(utilsUri);

    await c.waitForRecompile(utilsUri);
    c.assertCleanCompile(utilsUri);
    await c.shutdown();

    // After shutdown the persisted verdict must be gone.
    const cache = workspace.readCacheJson();
    expect(
        (cache?.["header_modes"] ?? []) as unknown[],
        `Verdict should be reset after the header was saved, got: ${JSON.stringify(cache)}`,
    ).toEqual([]);
});

test("dependency change retries trial", async ({ session }) => {
    // A header judged self-contained must be re-evaluated when one of its
    // own includes changes: here foo.h stops providing FOO, and only the
    // includer context (the host's define) can still supply it.
    const { client, workspace } = session.tmp();
    workspace.write("foo.h", "#pragma once\n#define FOO 1\n");
    workspace.write("h.h", '#pragma once\n#include "foo.h"\ninline int get() { return FOO; }\n');
    workspace.write("main.cpp", '#define FOO 2\n#include "h.h"\nint main() { return get(); }\n');
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait("main.cpp");
    const [hUri] = await client.openAndWait("h.h");
    client.assertCleanCompile(hUri);
    expect(prefixFiles(workspace), "Initially self-contained").toEqual([]);

    // foo.h stops defining FOO; only the host's #define can provide it now.
    await sleep(1_100);
    workspace.write("foo.h", "#pragma once\n");

    await client.waitForRecompile(hUri);
    client.assertCleanCompile(hUri);
    expect(
        prefixFiles(workspace).length,
        "Dependency change must re-run the trial and fall back to synthesis",
    ).toBe(1);
});

test("suffix closes embedding", async ({ session }) => {
    // X-macro fragments embedded in an enum or a function body compile
    // cleanly: the synthesized suffix closes the surrounding braces.
    const { client, workspace } = session.tmp();
    workspace.write("errors.def", 'X(Ok, 0, "success")\nX(NotFound, 1, "not found")\n');
    workspace.write(
        "main.cpp",
        "#define X(name, code, msg) name = code,\n" +
            "enum ErrorCode {\n" +
            '#include "errors.def"\n' +
            "};\n" +
            "#undef X\n" +
            "int main() { return Ok; }\n",
    );
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait("main.cpp");
    const [defUri] = await client.openAndWait("errors.def");
    client.assertCleanCompile(defUri);
});

test("suffix function body", async ({ session }) => {
    // The doc's classic register_all() case: statements expanded inside a
    // function body, closing brace restored by the suffix.
    const { client, workspace } = session.tmp();
    workspace.write("handlers.def", "X(alpha)\nX(beta)\n");
    workspace.write(
        "main.cpp",
        "inline void handle(int) {}\n" +
            "enum Ids { alpha, beta };\n" +
            "void register_all() {\n" +
            "#define X(name) handle(name);\n" +
            '#include "handlers.def"\n' +
            "#undef X\n" +
            "}\n" +
            "int main() { register_all(); return 0; }\n",
    );
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait("main.cpp");
    const [defUri] = await client.openAndWait("handlers.def");
    client.assertCleanCompile(defUri);
});

test("open synthesized artifact", async ({ session }) => {
    // Opening a synthesized prefix file compiles it with its host's
    // command (it is a fragment of that TU), not with junk context.
    const { client, workspace } = session.tmp();
    workspace.write("types.h", "#pragma once\nstruct Point { int x; int y; };\n");
    workspace.write("utils.h", "inline int get_x(Point p) { return p.x; }\n");
    workspace.write(
        "main.cpp",
        '#include "types.h"\n#include "utils.h"\nint main() { return get_x({1, 2}); }\n',
    );
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait("main.cpp");
    const [utilsUri] = await client.openAndWait("utils.h");
    client.assertCleanCompile(utilsUri);

    const prefixes = prefixFiles(workspace);
    expect(prefixes.length).toBe(1);
    const [prefixUri] = await client.openAndWait(prefixes[0]!);
    client.assertCleanCompile(prefixUri);

    // No context of its own, and no further synthesis chained off it.
    const q = await client.queryContext(prefixUri);
    expect(q.total, JSON.stringify(q)).toBe(0);
    expect(prefixFiles(workspace).length, "Opening an artifact must not synthesize more").toBe(1);
});

test("unbalanced brace degrades gracefully", async ({ session }) => {
    // A user-typed unbalanced brace in an embedded fragment steals the
    // suffix's closer: diagnostics must appear, and the server must keep
    // serving requests afterwards.
    const { client, workspace } = session.tmp();
    workspace.write("list.def", "X(alpha)\nvoid oops() {\n"); // unbalanced {
    workspace.write(
        "main.cpp",
        "#define X(name) int name;\n" +
            "enum Ids {\n" +
            '#include "list.def"\n' +
            "};\n" +
            "#undef X\n" +
            "int main() { return 0; }\n",
    );
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait("main.cpp");
    const [defUri] = await client.openAndWait("list.def");
    const diags = client.diagnostics.get(defUri) ?? [];
    expect(diags.length, "The imbalance must surface as diagnostics").toBeGreaterThan(0);

    // The server stays healthy: a follow-up request still answers.
    const q = await client.queryContext(defUri);
    expect(q.total).toBeGreaterThanOrEqual(1);
});
