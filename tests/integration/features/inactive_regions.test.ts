/// Integration tests for inactive regions over semantic tokens.
///
/// Every token inside an untaken #if branch of the current compilation
/// context carries the `inactive` modifier; bare identifiers there emit
/// as the `identifier` kind so they have a token to carry it. The
/// preamble's share comes from the PCH build via its pch.idx envelope
/// (conditions inside the bound never replay in the AST compile); a #if
/// cut by the bound resumes via the open-conditional stack.

import { waitUntil } from "@clice/tools/client";
import { expect, test } from "../fixtures.ts";

test("inactive after bound", async ({ session }) => {
    // Conditions entirely past the preamble bound (no PCH involvement).
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", "int a();\n#if 0\nint dead();\n#endif\nint main() { return 0; }\n");
    workspace.writeCDB(["main.cpp"]);

    await client.initialize(workspace);
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.inactiveLines(uri)).toEqual([2]);
});

test("inactive inside preamble", async ({ session }) => {
    // A #if 0 among the leading directives sits inside the preamble
    // bound: its region comes from the PCH build's scan, carried through
    // the pch.idx envelope and the compile params.
    const { client, workspace } = session.tmp();
    workspace.write(
        "main.cpp",
        "#define KEEP 1\n#if 0\n#define DEAD 2\n#endif\nint main() { return KEEP; }\n",
    );
    workspace.writeCDB(["main.cpp"]);

    await client.initialize(workspace);
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.inactiveLines(uri)).toEqual([2]);
});

test("inactive across bound", async ({ session }) => {
    // A #if inside the preamble bound lives in the PCH; its #elif/#endif
    // replay in the AST compile and resume from the PCH's open stack.
    const { client, workspace } = session.tmp();
    workspace.write(
        "render.h",
        "#pragma once\n" +
            "#if defined(USE_VULKAN)\n" +
            'inline const char* backend() { return "vk"; }\n' +
            "#elif defined(USE_METAL)\n" +
            'inline const char* backend() { return "mt"; }\n' +
            "#endif\n",
    );
    workspace.write("render_vk.cpp", '#include "render.h"\nint main() { return backend()[0]; }\n');
    workspace.writeEntries([["render_vk.cpp", ["-DUSE_VULKAN"]]]);

    await client.initialize(workspace);
    const [uri] = await client.openAndWait("render.h");
    expect(await client.inactiveLines(uri)).toEqual([4]);
});

test("inactive flips on context switch", async ({ session }) => {
    // A context switch recompiles the same document version, so the
    // client has no didChange to re-pull on: the landed compile must
    // fire workspace/semanticTokens/refresh, and the re-pulled tokens
    // carry the other branch's inactive lines.
    const { client, workspace } = session.tmp();
    workspace.write(
        "render.h",
        "#pragma once\n" +
            "#if defined(USE_VULKAN)\n" +
            'inline const char* backend() { return "vk"; }\n' +
            "#elif defined(USE_METAL)\n" +
            'inline const char* backend() { return "mt"; }\n' +
            "#endif\n",
    );
    workspace.write("render_vk.cpp", '#include "render.h"\nint main() { return backend()[0]; }\n');
    workspace.write("render_mt.cpp", '#include "render.h"\nint main() { return backend()[0]; }\n');
    workspace.writeEntries([
        ["render_vk.cpp", ["-DUSE_VULKAN"]],
        ["render_mt.cpp", ["-DUSE_METAL"]],
    ]);

    await client.initialize(workspace, {
        capabilities: { workspace: { semanticTokens: { refreshSupport: true } } },
    });
    const [uri] = await client.openAndWait("render.h");
    const before = await client.inactiveLines(uri);
    // Whichever host was ranked default, exactly one branch is dead.
    expect([[2], [4]]).toContainEqual(before);

    const target = before[0] === 4 ? "render_mt.cpp" : "render_vk.cpp";
    const { contexts } = await client.queryContext(uri);
    const host = contexts.find((c) => c.uri.includes(target));
    expect(host, `no ${target} context in ${JSON.stringify(contexts)}`).toBeDefined();

    const marker = client.serverRequests.length;
    const switched = await client.switchContext(uri, host!.uri);
    expect(switched.success).toBe(true);
    await client.waitForRecompile(uri);

    // The refresh request rides its own task after the diagnostics push.
    const refreshed = () =>
        client.serverRequests.slice(marker).includes("workspace/semanticTokens/refresh");
    await waitUntil(refreshed, {
        timeout: 10_000,
        interval: 50,
        description: "semantic token refresh after a context switch",
    });
    expect(refreshed(), "no semanticTokens refresh after the switch").toBe(true);
    expect(await client.inactiveLines(uri)).toEqual(before[0] === 4 ? [2] : [4]);
});

test("inactive else branch", async ({ session }) => {
    // #else carries no condition value; inactivity is derived from
    // whether an earlier branch was taken.
    const { client, workspace } = session.tmp();
    workspace.write(
        "main.cpp",
        "#define USE_A 1\n" +
            "int x();\n" +
            "#if USE_A\n" +
            "int active();\n" +
            "#else\n" +
            "int dead();\n" +
            "#endif\n" +
            "int main() { return 0; }\n",
    );
    workspace.writeCDB(["main.cpp"]);

    await client.initialize(workspace);
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.inactiveLines(uri)).toEqual([5]);
});
