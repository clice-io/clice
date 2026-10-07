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

test("header save keeps verdict", async ({ session }) => {
    // The verdict was scored on the buffer, which the save does not change:
    // a header that gains its own include keeps its includer context
    // rather than compiling twice for the save.
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
    expect(await synthesized(client), "Initial verdict: needs context").toBe(1);

    await sleep(MTIME_GRANULARITY);
    const newText = '#include "types.h"\ninline int get_x(Point p) { return p.x; }\n';
    client.change(utilsUri, 2, newText);
    await client.waitForRecompile(utilsUri);
    workspace.write("utils.h", newText);
    client.save(utilsUri);

    client.change(utilsUri, 3, newText + "\n");
    await client.waitForRecompile(utilsUri);
    client.assertCleanCompile(utilsUri);
    expect(await synthesized(client), "the save leaves the verdict alone").toBe(1);
});

test("save retries missing context", async ({ session }) => {
    // An edit makes a header the trial found self-contained lean on its
    // includer; the save that shows it missing a name compiles it in the
    // includer's context.
    const { client, workspace } = session.tmp();
    workspace.write("h.h", "#pragma once\ninline int get() { return 1; }\n");
    workspace.write(
        "main.cpp",
        'struct Host { int v; };\n#include "h.h"\nint main() { return get(); }\n',
    );
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait("main.cpp");
    const [hUri] = await client.openAndWait("h.h");
    expect(await synthesized(client), "Initially self-contained").toBe(0);

    await sleep(MTIME_GRANULARITY);
    const newText = "#pragma once\ninline int get() { return Host{2}.v; }\n";
    client.change(hUri, 2, newText);
    await client.waitForRecompile(hUri);
    client.assertHasErrors(hUri);

    workspace.write("h.h", newText);
    client.save(hUri);
    await client.waitForRecompile(hUri);
    client.assertCleanCompile(hUri);
    expect(await synthesized(client), "the save switches to the includer's context").toBe(1);
});

test("save before compile lands", async ({ session }) => {
    // Saved before the edit's compile shows what it misses, the buffer is
    // judged again all the same.
    const { client, workspace } = session.tmp();
    workspace.write("h.h", "#pragma once\ninline int get() { return 1; }\n");
    workspace.write(
        "main.cpp",
        'struct Host { int v; };\n#include "h.h"\nint main() { return get(); }\n',
    );
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait("main.cpp");
    const [hUri] = await client.openAndWait("h.h");
    expect(await synthesized(client), "Initially self-contained").toBe(0);

    await sleep(MTIME_GRANULARITY);
    const newText = "#pragma once\ninline int get() { return Host{2}.v; }\n";
    client.change(hUri, 2, newText);
    workspace.write("h.h", newText);
    client.save(hUri);
    await client.waitForRecompile(hUri);
    client.assertCleanCompile(hUri);
    expect(await synthesized(client)).toBe(1);
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

test("namespace members from host", async ({ session }) => {
    // Names an includer's earlier include adds to a namespace the header
    // already sees are missing context, not errors of the header.
    const { client, workspace } = session.tmp();
    workspace.write("lib.h", "#pragma once\nnamespace lib { struct A {}; }\n");
    workspace.write(
        "more.h",
        "#pragma once\nnamespace lib { struct B {}; template <class T> struct Box {}; }\n",
    );
    workspace.write(
        "user.h",
        '#pragma once\n#include "lib.h"\n' +
            "inline lib::B make() { return {}; }\n" +
            "inline lib::Box<int> box() { return {}; }\n",
    );
    workspace.write(
        "main.cpp",
        '#include "more.h"\n#include "user.h"\nint main() { make(); box(); }\n',
    );
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait("main.cpp");
    const [userUri] = await client.openAndWait("user.h");
    client.assertCleanCompile(userUri);
    expect(await synthesized(client)).toBe(1);
});

test("error directive needs host", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write(
        "guard.h",
        "#pragma once\n#ifndef VIA_HOST\n#error include through host\n#endif\n" +
            "inline int g() { return 1; }\n",
    );
    workspace.write(
        "main.cpp",
        '#define VIA_HOST\n#include "guard.h"\nint main() { return g(); }\n',
    );
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait("main.cpp");
    const [guardUri] = await client.openAndWait("guard.h");
    client.assertCleanCompile(guardUri);
});

test("pinned host synthesizes", async ({ session }) => {
    // A host the user picks is picked for its preprocessor state, even for
    // a header that compiles on its own.
    const { client, workspace } = session.tmp();
    workspace.write(
        "mode.h",
        "#pragma once\n#ifdef FAST\ninline int mode() { return 1; }\n" +
            "#else\ninline int mode() { return 2; }\n#endif\n",
    );
    workspace.write("a.cpp", '#define FAST\n#include "mode.h"\nint main() { return mode(); }\n');
    workspace.writeCDB(["a.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait("a.cpp");
    const [modeUri] = await client.openAndWait("mode.h");
    expect(await synthesized(client), "Self-contained on its own").toBe(0);

    expect((await client.switchContext(modeUri, workspace.uri("a.cpp"))).success).toBe(true);
    await client.waitForRecompile(modeUri);
    client.assertCleanCompile(modeUri);
    expect(await synthesized(client)).toBe(1);
});

test("fatal includer error shows", async ({ session }) => {
    // A fatal error before the include point silences every diagnostic
    // after it; it surfaces at the top of the header instead of leaving it
    // spotless.
    const { client, workspace } = session.tmp();
    workspace.write("ops.def", "OP(add)\nOP(sub)\n");
    workspace.write(
        "main.cpp",
        '#include "missing.h"\n#define OP(x) x,\nenum Op {\n#include "ops.def"\n};\n' +
            "int main() { return add; }\n",
    );
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    const [opsUri] = await client.openAndWait("ops.def");
    const errors = client.errors(opsUri);
    expect(errors.map((diagnostic) => diagnostic.message)).toEqual([
        "In includer context: 'missing.h' file not found",
    ]);
    expect(errors[0]?.range.start).toEqual({ line: 0, character: 0 });
});

test("includer error at note", async ({ session }) => {
    // An error in the includer's code past the include lands where the
    // header takes part in it.
    const { client, workspace } = session.tmp();
    workspace.write("m.inc", "void f(int) {}\n");
    workspace.write("main.cpp", 'struct S {\n#include "m.inc"\n};\nint main() { S{}.f(); }\n');
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    const [incUri] = await client.openAndWait("m.inc");
    const errors = client.errors(incUri);
    expect(errors.map((diagnostic) => diagnostic.message)).toEqual([
        "In includer context: too few arguments to function call, expected 1, have 0",
    ]);
    expect(errors[0]?.range.start.line).toBe(0);
});

test("C call needs host", async ({ session }) => {
    // C99 dropped implicit declarations: a call to a function only the
    // includer declares is a missing name.
    const { client, workspace } = session.tmp();
    workspace.write("util.h", "static inline int twice(void) { return helper() * 2; }\n");
    workspace.write(
        "main.c",
        'int helper(void);\n#include "util.h"\nint main(void) { return twice(); }\n',
    );
    workspace.writeEntries([["main.c", ["-x", "c", "-std=c17"]]]);
    await client.initialize(workspace);

    const [utilUri] = await client.openAndWait("util.h");
    client.assertCleanCompile(utilUri);
    expect(await synthesized(client)).toBe(1);
});

test("unclosed fragment scope", async ({ session }) => {
    // A scope the fragment opens and never closes swallows the includer's
    // remainder; the error lands on the brace that opened it.
    const { client, workspace } = session.tmp();
    workspace.write(
        "vec.h",
        '#pragma once\ntemplate <class T> struct Vec { T get(); };\n#include "vec.tpp"\n',
    );
    workspace.write(
        "vec.tpp",
        "namespace detail {\ntemplate <class T> T helper() { return T{}; }\n",
    );
    workspace.write("main.cpp", '#include "vec.h"\nint main() { return 0; }\n');
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    const [tppUri] = await client.openAndWait("vec.tpp");
    const errors = client.errors(tppUri);
    expect(errors.map((diagnostic) => diagnostic.message)).toEqual(["expected '}'"]);
    expect(errors[0]?.range.start.line).toBe(0);
});

test("includer warnings stay out", async ({ session }) => {
    // The includer's own code after the include point is the includer's:
    // its warnings do not move onto the header through their notes.
    const { client, workspace } = session.tmp();
    workspace.write(
        "api.h",
        "#pragma once\n[[deprecated]] inline int old_api() { return BASE; }\n",
    );
    workspace.write(
        "main.cpp",
        '#define BASE 1\n#include "api.h"\nint main() { return old_api(); }\n',
    );
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait("main.cpp");
    const [apiUri] = await client.openAndWait("api.h");
    expect(client.diagnostics.get(apiUri) ?? []).toEqual([]);
    expect(await synthesized(client)).toBe(1);
});

test("unmatched chain is reported", async ({ session }) => {
    // The scan resolved shared.h's include under b.cpp's directories, but
    // the host chosen for config_b/config.h is a.cpp, whose directories
    // reach another config.h: the context cannot be rebuilt, and the
    // header says so instead of only showing what is missing.
    const { client, workspace } = session.tmp();
    workspace.write("common/shared.h", "#pragma once\n#include <config.h>\n");
    workspace.write("config_a/config.h", "#pragma once\nstruct ConfigA {};\n");
    workspace.write("config_b/config.h", "#pragma once\nBType make_b();\n");
    workspace.write("a.cpp", '#include "shared.h"\nint main() { return 0; }\n');
    workspace.write(
        "b.cpp",
        'typedef int BType;\n#include "shared.h"\nint f() { return make_b(); }\n',
    );
    workspace.writeEntries([
        ["b.cpp", ["-Iconfig_b", "-Icommon"]],
        ["a.cpp", ["-Iconfig_a", "-Icommon"]],
    ]);
    await client.initialize(workspace);

    const [configUri] = await client.openAndWait("config_b/config.h");
    const codes = (client.diagnostics.get(configUri) ?? []).map((diagnostic) => diagnostic.code);
    expect(codes).toContain("unmatched-includer-context");
});
