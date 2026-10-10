/// Integration tests for inactive regions over semantic tokens.
///
/// Every token inside an untaken #if branch of the current compilation
/// context carries the `inactive` modifier; bare identifiers there emit
/// as the `identifier` kind so they have a token to carry it. The
/// preamble's share comes from the PCH build via its pch.idx envelope
/// (conditions inside the bound never replay in the AST compile); a #if
/// cut by the bound resumes via the open-conditional stack.

import type * as proto from "vscode-languageserver-protocol";
import type { ServeOptions } from "@clice/tools/actions";
import { at, expect, serve } from "../../fixtures.ts";

/// C++17 units with their own arguments.
function cxx17(units: Record<string, string[]>): ServeOptions {
    return { manifest: { cxx: ["-std=c++17"], units } };
}

const RENDER_H =
    "#pragma once\n" +
    "#if defined(USE_VULKAN)\n" +
    'inline const char* backend() { return "vk"; }\n' +
    "#elif defined(USE_METAL)\n" +
    'inline const char* backend() { return "mt"; }\n' +
    "#endif\n";

const RENDER_CPP = '#include "render.h"\nint main() { return backend()[0]; }\n';

const REFRESH = "workspace/semanticTokens/refresh";

/// The lines strictly between two directive lines.
function linesBetween(open: proto.Range, close: proto.Range): number[] {
    const first = open.start.line + 1;
    return Array.from({ length: close.start.line - first }, (_, index) => first + index);
}

// Conditions entirely past the preamble bound (no PCH involvement).
serve.files(
    { "main.cpp": "int a();\n#if 0\nint dead();\n#endif\nint main() { return 0; }\n" },
    cxx17({ "main.cpp": [] }),
)("inactive after bound", async ({ s }) => {
    await s.compiled("main.cpp");
    expect(await s.inactiveLines("main.cpp")).toEqual([2]);
});

// A #if 0 among the leading directives sits inside the preamble bound: its
// region comes from the PCH build's scan, carried through the pch.idx
// envelope and the compile params.
serve.files(
    { "main.cpp": "#define KEEP 1\n#if 0\n#define DEAD 2\n#endif\nint main() { return KEEP; }\n" },
    cxx17({ "main.cpp": [] }),
)("inactive inside preamble", async ({ s }) => {
    await s.compiled("main.cpp");
    expect(await s.inactiveLines("main.cpp")).toEqual([2]);
});

// A #if inside the preamble bound lives in the PCH; its #elif/#endif replay
// in the AST compile and resume from the PCH's open stack.
serve.files(
    { "render.h": RENDER_H, "render_vk.cpp": RENDER_CPP },
    cxx17({ "render_vk.cpp": ["-DUSE_VULKAN"] }),
)("inactive across bound", async ({ s }) => {
    await s.compiled("render.h");
    expect(await s.inactiveLines("render.h")).toEqual([4]);
});

// A context switch recompiles the same document version, so the client has
// no didChange to re-pull on: the landed compile must fire
// workspace/semanticTokens/refresh, and the re-pulled tokens carry the
// other branch's inactive lines.
serve("shapes/headers", {
    launch: { capabilities: { workspace: { semanticTokens: { refreshSupport: true } } } },
})("inactive flips on context switch", async ({ s }) => {
    const exact = linesBetween(
        s.range(at(s.file("detail"), "#ifdef SHAPES_EXACT")),
        s.range(at(s.file("detail"), "#else")),
    );
    const rounded = linesBetween(
        s.range(at(s.file("detail"), "#else")),
        s.range(at(s.file("detail"), "#endif")),
    );

    await s.compiled(s.file("detail"));
    const before = await s.inactiveLines(s.file("detail"));
    // Whichever host was ranked default, exactly one branch is dead.
    expect([exact, rounded]).toContainEqual(before);

    // Only circle.cpp compiles with SHAPES_EXACT: the other branch is live
    // in any other host, else in circle.cpp.
    const exactNow = before[0] === rounded[0];
    const { contexts } = await s.contexts(s.file("detail"));
    const host = contexts.find((c) => c.uri.endsWith(s.file("circle_impl")) !== exactNow);
    expect(host, `no host to flip to in ${JSON.stringify(contexts)}`).toBeDefined();

    const refreshes = await s.serverRequests(REFRESH);
    const switched = await s.switchContext(s.file("detail"), s.relative(host!.uri));
    expect(switched.success).toBe(true);
    await s.diagnostics(s.file("detail"));

    // What the landed compile scheduled — the refresh request — goes out
    // before the sync's answer.
    expect(
        await s.serverRequests(REFRESH),
        "no semanticTokens refresh after the switch",
    ).toBeGreaterThan(refreshes);
    expect(await s.inactiveLines(s.file("detail"))).toEqual(exactNow ? exact : rounded);
});

// #else carries no condition value; inactivity is derived from whether an
// earlier branch was taken.
serve.files(
    {
        "main.cpp":
            "#define USE_A 1\n" +
            "int x();\n" +
            "#if USE_A\n" +
            "int active();\n" +
            "#else\n" +
            "int dead();\n" +
            "#endif\n" +
            "int main() { return 0; }\n",
    },
    cxx17({ "main.cpp": [] }),
)("inactive else branch", async ({ s }) => {
    await s.compiled("main.cpp");
    expect(await s.inactiveLines("main.cpp")).toEqual([5]);
});
