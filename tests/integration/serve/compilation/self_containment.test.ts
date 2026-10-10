/// Integration tests for automatic self-containment detection.
///
/// Headers without a CDB entry compile self-contained first (borrowed host
/// command, no prefix synthesis). When the trial diagnostics indicate missing
/// includer context, the server falls back to prefix synthesis transparently —
/// only the final diagnostics are published. Verdicts and user context
/// choices persist across server sessions via the index database.

import type * as proto from "vscode-languageserver-protocol";
import type { Serve } from "@clice/tools/actions";
import { expect, serve } from "../../fixtures.ts";

/// The diagnostics of `file` from a compile the request had to run: the
/// file compiled and published again.
async function recompiled(s: Serve, file: string): Promise<proto.Diagnostic[]> {
    const before = (await s.counts()).files[file];
    const diagnostics = await s.diagnostics(file);
    const after = (await s.counts()).files[file];
    expect(after?.compile, `${file} compiles again`).toBeGreaterThan(before?.compile ?? 0);
    expect(after?.publish, `${file} publishes again`).toBeGreaterThan(before?.publish ?? 0);
    return diagnostics;
}

const POINT = "#pragma once\nstruct Point { int x; int y; };\n";
const GET_X = "inline int get_x(Point p) { return p.x; }\n";

serve.files({
    "types.h": POINT,
    "helper.h": `#pragma once\n#include "types.h"\n${GET_X}`,
    "main.cpp": '#include "helper.h"\nint main() { return get_x({1, 2}); }\n',
})("self contained skips synthesis", async ({ s }) => {
    // A self-contained header borrows a command but gets no prefix.
    await s.compiled("main.cpp");
    await s.clean("helper.h");
    expect(
        (await s.stats()).synthesizedContexts,
        "Self-contained headers must not synthesize a prefix",
    ).toBe(0);
});

const NEEDS_POINT = {
    "types.h": POINT,
    "utils.h": GET_X,
    "main.cpp": '#include "types.h"\n#include "utils.h"\nint main() { return get_x({1, 2}); }\n',
};

serve.files(NEEDS_POINT)("fallback on missing context", async ({ s }) => {
    // A non-self-contained header falls back to prefix synthesis
    // automatically; the trial's error diagnostics are never published.
    await s.compiled("main.cpp");
    await s.clean("utils.h");
    expect((await s.counts()).files["utils.h"]?.publish, "One publish, the clean one").toBe(1);
    expect(
        (await s.stats()).synthesizedContexts,
        "Fallback must synthesize exactly one prefix",
    ).toBe(1);
});

serve.files({
    "shared.h": "VALUE_TYPE get_value();\n",
    "a.cpp": '#define VALUE_TYPE int\n#include "shared.h"\nint main() { return 0; }\n',
    "b.cpp": '#define VALUE_TYPE float\n#include "shared.h"\nfloat f() { return 0; }\n',
})("choice persisted across sessions", async ({ s }) => {
    // A switchContext choice is restored on didOpen in a later session.
    await s.compiled("a.cpp");
    await s.compiled("b.cpp");
    s.open("shared.h");
    const sw = await s.switchContext("shared.h", "b.cpp");
    expect(sw.success).toBe(true);
    await s.stop();

    await s.start();
    s.open("shared.h");
    const current = await s.currentContext("shared.h");
    const ctx = current.context;
    expect(
        ctx?.uri.includes("b.cpp") ?? false,
        `Persisted context choice should be restored on didOpen, got: ${JSON.stringify(current)}`,
    ).toBe(true);
    expect(current.automatic).toBe(false);

    // A reset is persisted too.
    expect((await s.resetContext("shared.h")).success).toBe(true);
    await s.stop();

    await s.start();
    s.open("shared.h");
    expect((await s.currentContext("shared.h")).automatic).toBe(true);
});

serve.files({
    "typo.h": "inline int broken() { return }\n", // syntax error
    "main.cpp": '#include "typo.h"\nint main() { return 0; }\n',
})("ordinary error no fallback", async ({ s }) => {
    // A self-contained header with a benign syntax error must not trigger
    // prefix synthesis nor persist any verdict.
    const diags = await s.compiled("typo.h");
    expect(diags.length, "The syntax error must be published").toBeGreaterThan(0);
    expect(
        (await s.stats()).synthesizedContexts,
        "Ordinary errors must not trigger prefix synthesis",
    ).toBe(0);
});

serve.files(NEEDS_POINT)("header save keeps verdict", async ({ s }) => {
    // The verdict was scored on the buffer, which the save does not change:
    // a header that gains its own include keeps its includer context
    // rather than compiling twice for the save.
    await s.compiled("main.cpp");
    await s.compiled("utils.h");
    expect((await s.stats()).synthesizedContexts, "Initial verdict: needs context").toBe(1);

    s.edit("utils.h", { before: "inline int get_x", insert: '#include "types.h"\n' });
    await s.compiled("utils.h");
    s.save("utils.h");

    s.edit("utils.h", { after: "p.x; }\n", insert: "\n" });
    expect(await recompiled(s, "utils.h")).toEqual([]);
    expect((await s.stats()).synthesizedContexts, "the save leaves the verdict alone").toBe(1);
});

const HOST_AFTER = {
    "h.h": "#pragma once\ninline int get() { return 1; }\n",
    "main.cpp": 'struct Host { int v; };\n#include "h.h"\nint main() { return get(); }\n',
};

serve.files(HOST_AFTER)("save retries missing context", async ({ s }) => {
    // An edit makes a header the trial found self-contained lean on its
    // includer; the save that shows it missing a name compiles it in the
    // includer's context.
    await s.compiled("main.cpp");
    await s.compiled("h.h");
    expect((await s.stats()).synthesizedContexts, "Initially self-contained").toBe(0);

    s.edit("h.h", { replace: "return 1;", with: "return Host{2}.v;" });
    expect((await s.errors("h.h")).length).toBeGreaterThan(0);

    s.save("h.h");
    expect(await recompiled(s, "h.h")).toEqual([]);
    expect(
        (await s.stats()).synthesizedContexts,
        "the save switches to the includer's context",
    ).toBe(1);
});

serve.files(HOST_AFTER)("save before compile lands", async ({ s }) => {
    // Saved before the edit's compile shows what it misses, the buffer is
    // judged again all the same.
    await s.compiled("main.cpp");
    await s.compiled("h.h");
    expect((await s.stats()).synthesizedContexts, "Initially self-contained").toBe(0);

    s.edit("h.h", { replace: "return 1;", with: "return Host{2}.v;" });
    s.save("h.h");
    expect(await recompiled(s, "h.h")).toEqual([]);
    expect((await s.stats()).synthesizedContexts).toBe(1);
});

serve.files({
    "foo.h": "#pragma once\n#define FOO 1\n",
    "h.h": '#pragma once\n#include "foo.h"\ninline int get() { return FOO; }\n',
    "main.cpp": '#define FOO 2\n#include "h.h"\nint main() { return get(); }\n',
})("dependency change retries trial", async ({ s }) => {
    // A header judged self-contained must be re-evaluated when one of its
    // own includes changes: here foo.h stops providing FOO, and only the
    // includer context (the host's define) can still supply it.
    await s.compiled("main.cpp");
    await s.clean("h.h");
    expect((await s.stats()).synthesizedContexts, "Initially self-contained").toBe(0);

    // foo.h stops defining FOO; only the host's #define can provide it now.
    s.disk.write("foo.h", "#pragma once\n");
    expect(await recompiled(s, "h.h")).toEqual([]);
    expect(
        (await s.stats()).synthesizedContexts,
        "Dependency change must re-run the trial and fall back to synthesis",
    ).toBe(1);
});

serve.files({
    "errors.def": 'X(Ok, 0, "success")\nX(NotFound, 1, "not found")\n',
    "main.cpp":
        "#define X(name, code, msg) name = code,\n" +
        "enum ErrorCode {\n" +
        '#include "errors.def"\n' +
        "};\n" +
        "#undef X\n" +
        "int main() { return Ok; }\n",
})("suffix closes embedding", async ({ s }) => {
    // X-macro fragments embedded in an enum or a function body compile
    // cleanly: the synthesized suffix closes the surrounding braces.
    await s.compiled("main.cpp");
    await s.clean("errors.def");
});

serve.files({
    "handlers.def": "X(alpha)\nX(beta)\n",
    "main.cpp":
        "inline void handle(int) {}\n" +
        "enum Ids { alpha, beta };\n" +
        "void register_all() {\n" +
        "#define X(name) handle(name);\n" +
        '#include "handlers.def"\n' +
        "#undef X\n" +
        "}\n" +
        "int main() { register_all(); return 0; }\n",
})("suffix function body", async ({ s }) => {
    // The doc's classic register_all() case: statements expanded inside a
    // function body, closing brace restored by the suffix.
    await s.compiled("main.cpp");
    await s.clean("handlers.def");
});

serve.files({
    "src/local_config.h": "#pragma once\nstruct Local { int v; };\n",
    "src/x.h": "inline int value() { Local l{3}; return l.v; }\n",
    "src/main.cpp":
        '#if __has_include("local_config.h")\n#include "local_config.h"\n#endif\n' +
        '#include "x.h"\nint main() { return value(); }\n',
})("context lookups beside includer", async ({ s }) => {
    // The synthesized context resolves lookups as the file it was cut
    // from does: a `__has_include` probe relative to the host still finds
    // its header.
    await s.compiled("src/main.cpp");
    await s.clean("src/x.h");
    expect((await s.stats()).synthesizedContexts).toBe(1);
});

serve.files({
    ".clice": "not a directory\n",
    "utils.h": "inline int get(Point p) { return p.x; }\n",
    "main.cpp":
        'struct Point { int x; };\n#include "utils.h"\nint main() { return get(Point{1}); }\n',
})("context without cache directory", async ({ s }) => {
    // The synthesized context lives in memory: a cache directory the
    // server cannot use leaves it intact.
    await s.compiled("main.cpp");
    await s.clean("utils.h");
});

serve.files({
    "list.def": "X(alpha)\nvoid oops() {\n", // unbalanced {
    "main.cpp":
        "#define X(name) int name;\n" +
        "enum Ids {\n" +
        '#include "list.def"\n' +
        "};\n" +
        "#undef X\n" +
        "int main() { return 0; }\n",
})("unbalanced brace degrades gracefully", async ({ s }) => {
    // A user-typed unbalanced brace in an embedded fragment steals the
    // suffix's closer: diagnostics must appear, and the server must keep
    // serving requests afterwards.
    await s.compiled("main.cpp");
    const diags = await s.compiled("list.def");
    expect(diags.length, "The imbalance must surface as diagnostics").toBeGreaterThan(0);

    // The server stays healthy: a follow-up request still answers.
    const q = await s.contexts("list.def");
    expect(q.total).toBeGreaterThanOrEqual(1);
});

serve.files({
    "lib.h": "#pragma once\nnamespace lib { struct A {}; }\n",
    "more.h": "#pragma once\nnamespace lib { struct B {}; template <class T> struct Box {}; }\n",
    "user.h":
        '#pragma once\n#include "lib.h"\n' +
        "inline lib::B make() { return {}; }\n" +
        "inline lib::Box<int> box() { return {}; }\n",
    "main.cpp": '#include "more.h"\n#include "user.h"\nint main() { make(); box(); }\n',
})("namespace members from host", async ({ s }) => {
    // Names an includer's earlier include adds to a namespace the header
    // already sees are missing context, not errors of the header.
    await s.compiled("main.cpp");
    await s.clean("user.h");
    expect((await s.stats()).synthesizedContexts).toBe(1);
});

serve.files({
    "guard.h":
        "#pragma once\n#ifndef VIA_HOST\n#error include through host\n#endif\n" +
        "inline int g() { return 1; }\n",
    "main.cpp": '#define VIA_HOST\n#include "guard.h"\nint main() { return g(); }\n',
})("error directive needs host", async ({ s }) => {
    await s.compiled("main.cpp");
    await s.clean("guard.h");
});

serve.files({
    "mode.h":
        "#pragma once\n#ifdef FAST\ninline int mode() { return 1; }\n" +
        "#else\ninline int mode() { return 2; }\n#endif\n",
    "a.cpp": '#define FAST\n#include "mode.h"\nint main() { return mode(); }\n',
})("pinned host synthesizes", async ({ s }) => {
    // A host the user picks is picked for its preprocessor state, even for
    // a header that compiles on its own.
    await s.compiled("a.cpp");
    await s.compiled("mode.h");
    expect((await s.stats()).synthesizedContexts, "Self-contained on its own").toBe(0);

    expect((await s.switchContext("mode.h", "a.cpp")).success).toBe(true);
    expect(await recompiled(s, "mode.h")).toEqual([]);
    expect((await s.stats()).synthesizedContexts).toBe(1);
});

serve.files({
    "ops.def": "OP(add)\nOP(sub)\n",
    "main.cpp":
        '#include "missing.h"\n#define OP(x) x,\nenum Op {\n#include "ops.def"\n};\n' +
        "int main() { return add; }\n",
})("fatal includer error shows", async ({ s }) => {
    // A fatal error before the include point silences every diagnostic
    // after it; it surfaces at the top of the header instead of leaving it
    // spotless.
    const errors = await s.errors("ops.def");
    expect(errors.map((diagnostic) => diagnostic.message)).toEqual([
        "In includer context: 'missing.h' file not found",
    ]);
    expect(errors[0]?.range.start).toEqual({ line: 0, character: 0 });
});

serve.files({
    "m.inc": "void f(int) {}\n",
    "main.cpp": 'struct S {\n#include "m.inc"\n};\nint main() { S{}.f(); }\n',
})("includer error at note", async ({ s }) => {
    // An error in the includer's code past the include lands where the
    // header takes part in it.
    const errors = await s.errors("m.inc");
    expect(errors.map((diagnostic) => diagnostic.message)).toEqual([
        "In includer context: too few arguments to function call, expected 1, have 0",
    ]);
    expect(errors[0]?.range.start.line).toBe(0);
});

serve.files({
    "util.h": "static inline int twice(void) { return helper() * 2; }\n",
    "main.c": 'int helper(void);\n#include "util.h"\nint main(void) { return twice(); }\n',
})("C call needs host", async ({ s }) => {
    // C99 dropped implicit declarations: a call to a function only the
    // includer declares is a missing name.
    await s.clean("util.h");
    expect((await s.stats()).synthesizedContexts).toBe(1);
});

serve.files({
    "vec.h": '#pragma once\ntemplate <class T> struct Vec { T get(); };\n#include "vec.tpp"\n',
    "vec.tpp": "namespace detail {\ntemplate <class T> T helper() { return T{}; }\n",
    "main.cpp": '#include "vec.h"\nint main() { return 0; }\n',
})("unclosed fragment scope", async ({ s }) => {
    // A scope the fragment opens and never closes swallows the includer's
    // remainder; the error lands on the brace that opened it.
    const errors = await s.errors("vec.tpp");
    expect(errors.map((diagnostic) => diagnostic.message)).toEqual(["expected '}'"]);
    expect(errors[0]?.range.start.line).toBe(0);
});

serve.files({
    "api.h": "#pragma once\n[[deprecated]] inline int old_api() { return BASE; }\n",
    "main.cpp": '#define BASE 1\n#include "api.h"\nint main() { return old_api(); }\n',
})("includer warnings stay out", async ({ s }) => {
    // The includer's own code after the include point is the includer's:
    // its warnings do not move onto the header through their notes.
    await s.compiled("main.cpp");
    expect(await s.compiled("api.h")).toEqual([]);
    expect((await s.stats()).synthesizedContexts).toBe(1);
});

serve.files(
    {
        "common/shared.h": "#pragma once\n#include <config.h>\n",
        "config_a/config.h": "#pragma once\nstruct ConfigA {};\n",
        "config_b/config.h": "#pragma once\nBType make_b();\n",
        "a.cpp": '#include "shared.h"\nint main() { return 0; }\n',
        "b.cpp": 'typedef int BType;\n#include "shared.h"\nint f() { return make_b(); }\n',
    },
    {
        manifest: {
            units: {
                "b.cpp": ["-Iconfig_b", "-Icommon"],
                "a.cpp": ["-Iconfig_a", "-Icommon"],
            },
        },
    },
)("config header finds includer", async ({ s }) => {
    // The scan resolved shared.h's include under b.cpp's directories;
    // a.cpp ranks first, but its compile enters another config.h.
    await s.clean("config_b/config.h");
    expect((await s.contexts("config_b/config.h")).contexts.map((context) => context.uri)).toEqual([
        s.uri("b.cpp"),
    ]);
});

serve.files({
    "foo.h": '#pragma once\nstruct Foo {};\n#include "bar.h"\n',
    "bar.h": "#pragma once\ninline Foo make() { return {}; }\n",
    "main.cpp": '#include "foo.h"\n#include "bar.h"\nint main() { make(); }\n',
})("chain the compile takes", async ({ s }) => {
    // main.cpp enters bar.h through foo.h; its own include of bar.h then
    // finds the guard.
    await s.clean("bar.h");

    // foo.h moves its include: the recorded tree no longer vouches for
    // the chain, so the host is preprocessed again.
    s.disk.edit("foo.h", { before: '#include "bar.h"', insert: "\n" });
    expect(await recompiled(s, "bar.h")).toEqual([]);
});

serve.files({
    "utils.h": "inline Posix make() { return {}; }\n",
    "main.cpp":
        '#ifdef CLICE_NEVER\nstruct Other {};\n#include "utils.h"\n#else\n' +
        'struct Posix {};\n#include "utils.h"\n#endif\nint main() { make(); }\n',
})("inactive include skipped", async ({ s }) => {
    await s.clean("utils.h");
});
