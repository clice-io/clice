/// Integration tests for automatic self-containment detection.
///
/// Headers without a CDB entry compile self-contained first (borrowed host
/// command, no prefix synthesis). When the trial diagnostics indicate missing
/// includer context, the server falls back to prefix synthesis transparently —
/// only the final diagnostics are published. Verdicts and user context
/// choices persist across server sessions via the index database.

import { type CliceClient, MTIME_GRANULARITY, sleep } from "@clice/tools/client";
import { expect, test } from "../fixtures.ts";

async function synthesized(client: CliceClient): Promise<number> {
    return (await client.stats()).synthesizedContexts;
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
    expect(await synthesized(client), "Self-contained headers must not synthesize a prefix").toBe(
        0,
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
    expect(await synthesized(client), "Fallback must synthesize exactly one prefix").toBe(1);
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
    expect(await synthesized(client), "Ordinary errors must not trigger prefix synthesis").toBe(0);
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
    expect(await synthesized(c), "Initial verdict: needs context").toBe(1);

    // Make the header self-contained on disk and in the buffer, then save.
    await sleep(MTIME_GRANULARITY);
    const newText = '#include "types.h"\ninline int get_x(Point p) { return p.x; }\n';
    workspace.write("utils.h", newText);
    c.change(utilsUri, 2, newText);
    c.save(utilsUri);

    await c.waitForRecompile(utilsUri);
    c.assertCleanCompile(utilsUri);
    await c.shutdown();
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
    expect(await synthesized(client), "Initially self-contained").toBe(0);

    // foo.h stops defining FOO; only the host's #define can provide it now.
    await sleep(MTIME_GRANULARITY);
    workspace.write("foo.h", "#pragma once\n");

    await client.waitForRecompile(hUri);
    client.assertCleanCompile(hUri);
    expect(
        await synthesized(client),
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

test("context lookups beside includer", async ({ session }) => {
    // The synthesized context resolves lookups as the file it was cut
    // from does: a `__has_include` probe relative to the host still finds
    // its header.
    const { client, workspace } = session.tmp();
    workspace.write("src/local_config.h", "#pragma once\nstruct Local { int v; };\n");
    workspace.write("src/x.h", "inline int value() { Local l{3}; return l.v; }\n");
    workspace.write(
        "src/main.cpp",
        '#if __has_include("local_config.h")\n#include "local_config.h"\n#endif\n' +
            '#include "x.h"\nint main() { return value(); }\n',
    );
    workspace.writeCDB(["src/main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait("src/main.cpp");
    const [xUri] = await client.openAndWait("src/x.h");
    client.assertCleanCompile(xUri);
    expect(await synthesized(client)).toBe(1);
});

test("context without cache directory", async ({ session }) => {
    // The synthesized context lives in memory: a cache directory the
    // server cannot use leaves it intact.
    const workspace = session.tmpdir();
    workspace.write(".clice", "not a directory\n");
    workspace.write("utils.h", "inline int get(Point p) { return p.x; }\n");
    workspace.write(
        "main.cpp",
        'struct Point { int x; };\n#include "utils.h"\nint main() { return get(Point{1}); }\n',
    );
    workspace.writeCDB(["main.cpp"]);
    const client = session.spawn(workspace);
    await client.initialize(workspace);

    await client.openAndWait("main.cpp");
    const [utilsUri] = await client.openAndWait("utils.h");
    client.assertCleanCompile(utilsUri);
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
