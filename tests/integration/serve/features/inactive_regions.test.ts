/// Integration tests for inactive regions over semantic tokens.
///
/// Every token inside an untaken #if branch of the current compilation
/// context carries the `inactive` modifier; bare identifiers there emit
/// as the `identifier` kind so they have a token to carry it. The
/// preamble's share comes from the PCH build via its pch.idx envelope
/// (conditions inside the bound never replay in the AST compile); a #if
/// cut by the bound resumes via the open-conditional stack.

import type { ServeOptions } from "@clice/tools/actions";
import { expect, serve } from "../../fixtures.ts";

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
serve.files(
    { "render.h": RENDER_H, "render_vk.cpp": RENDER_CPP, "render_mt.cpp": RENDER_CPP },
    {
        ...cxx17({ "render_vk.cpp": ["-DUSE_VULKAN"], "render_mt.cpp": ["-DUSE_METAL"] }),
        launch: { capabilities: { workspace: { semanticTokens: { refreshSupport: true } } } },
    },
)("inactive flips on context switch", async ({ s }) => {
    await s.compiled("render.h");
    const before = await s.inactiveLines("render.h");
    // Whichever host was ranked default, exactly one branch is dead.
    expect([[2], [4]]).toContainEqual(before);

    const target = before[0] === 4 ? "render_mt.cpp" : "render_vk.cpp";
    const { contexts } = await s.contexts("render.h");
    const host = contexts.find((c) => c.uri.includes(target));
    expect(host, `no ${target} context in ${JSON.stringify(contexts)}`).toBeDefined();

    const refreshes = await s.serverRequests(REFRESH);
    const switched = await s.switchContext("render.h", s.relative(host!.uri));
    expect(switched.success).toBe(true);
    await s.diagnostics("render.h");

    // What the landed compile scheduled — the refresh request — goes out
    // before the sync's answer.
    expect(
        await s.serverRequests(REFRESH),
        "no semanticTokens refresh after the switch",
    ).toBeGreaterThan(refreshes);
    expect(await s.inactiveLines("render.h")).toEqual(before[0] === 4 ? [2] : [4]);
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
