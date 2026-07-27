/// Integration tests for the clice/inactiveRegions notification.
///
/// Pushed after every compile with the untaken #if branch bodies of the
/// current compilation context. The preamble's share comes from the PCH
/// build (conditions inside the bound never replay in the AST compile);
/// a #if cut by the bound resumes via the open-conditional stack.

import type { Range } from "vscode-languageserver-protocol";
import { sleep, type CliceClient } from "@clice/tools/client";
import { InactiveRegionsNotification, type InactiveRegionsParams } from "@clice/tools/protocol";
import { expect, test } from "../fixtures.ts";

async function waitRegions(captured: InactiveRegionsParams[], timeout = 15_000): Promise<Range[]> {
    const deadline = Date.now() + timeout;
    while (Date.now() < deadline) {
        const last = captured[captured.length - 1];
        if (last && last.regions.length > 0) {
            return last.regions;
        }
        await sleep(50);
    }
    const last = captured[captured.length - 1];
    return last ? last.regions : [];
}

function capture(client: CliceClient): InactiveRegionsParams[] {
    const captured: InactiveRegionsParams[] = [];
    client.onNotification(InactiveRegionsNotification, (params) => {
        captured.push(params);
    });
    return captured;
}

function lines(regions: Range[]): [number, number][] {
    return regions.map((r) => [r.start.line, r.end.line]);
}

test("inactive after bound", async ({ session }) => {
    // Conditions entirely past the preamble bound (no PCH involvement).
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", "int a();\n#if 0\nint dead();\n#endif\nint main() { return 0; }\n");
    workspace.writeCDB(["main.cpp"]);
    const captured = capture(client);

    await client.initialize(workspace);
    await client.openAndWait("main.cpp");
    const regions = await waitRegions(captured);
    expect(lines(regions), JSON.stringify(regions)).toEqual([[2, 3]]);
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
    const captured = capture(client);

    await client.initialize(workspace);
    await client.openAndWait("render.h");
    const regions = await waitRegions(captured);
    expect(lines(regions), JSON.stringify(regions)).toEqual([[4, 5]]);
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
    const captured = capture(client);

    await client.initialize(workspace);
    await client.openAndWait("main.cpp");
    const regions = await waitRegions(captured);
    expect(lines(regions), JSON.stringify(regions)).toEqual([[5, 6]]);
});
