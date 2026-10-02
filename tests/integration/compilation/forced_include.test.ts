/// Integration tests for forced includes (`-include`): the dependency graph
/// scans them like any included header.

import { MTIME_GRANULARITY, sleep } from "@clice/tools/client";
import { expect, test } from "../fixtures.ts";

test("edits pay no import scan", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("force.h", "#define FORCED 1\n");
    workspace.write("header.h", "inline int header() { return FORCED; }\n");
    workspace.write("main.cpp", '#include "header.h"\nint main() { return header(); }\n');
    workspace.writeCDB(["main.cpp"], { extraArgs: ["-include", "force.h"] });
    await client.initialize(workspace);

    const [main] = await client.openAndWait("main.cpp");
    const [header] = await client.openAndWait("header.h");
    client.assertNoErrors(main);
    client.assertNoErrors(header);
    client.change(main, 1, '#include "header.h"\nint main() { return header() + 1; }\n');
    await client.waitForRecompile(main);
    client.change(header, 1, "inline int header() { return FORCED + 1; }\n");
    await client.waitForRecompile(header);
    client.assertNoErrors(header, "the header compiles through its host");
    expect(await client.waitForIndex(main, "header"), "the unit is indexed").toBe(true);

    expect((await client.stats()).importScans).toBe(0);
});

test("saved forced header recompiles", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("force.h", "#define FORCED 1\n");
    workspace.write("main.cpp", "int main() { return FORCED; }\n");
    workspace.writeCDB(["main.cpp"], { extraArgs: ["-include", "force.h"] });
    await client.initialize(workspace);

    const [main] = await client.openAndWait("main.cpp");
    client.assertCleanCompile(main);

    await sleep(MTIME_GRANULARITY);
    workspace.write("force.h", "#define FORCED_RENAMED 1\n");
    client.save(workspace.uri("force.h"));
    await client.waitForRecompile(main);
    client.assertHasErrors(main, "the forced header no longer defines FORCED");
});
