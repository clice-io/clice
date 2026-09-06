/// Databases nobody declared: startup loads the root's and every direct
/// subdirectory's, opening a file registers the ones above it, a vanished
/// one yields to the present, and a file with neither an entry nor a host
/// borrows a nearby unit's command.

import { MTIME_GRANULARITY, sleep, type CliceClient } from "@clice/tools/client";
import { expect, test } from "../fixtures.ts";

function gated(macro: string): string {
    return `#ifndef ${macro}\n#error missing ${macro}\n#endif\nint main() { return 0; }\n`;
}

function guidance(client: CliceClient, uri: string): number {
    return (client.diagnostics.get(uri) ?? []).filter((d) => d.code === "inferred-compile-command")
        .length;
}

async function cdbEvents(client: CliceClient, force = false): Promise<number> {
    return (await client.poll("cdb", { force })).events;
}

test("nested projects load on open", async ({ session }) => {
    const { client } = await session("cdb/nested_projects");
    const [p1] = await client.openAndWait("group-a/p1/main.cpp");
    client.assertNoErrors(p1, "opening a file registers its project's database");
    const [p2] = await client.openAndWait("group-a/p2/main.cpp");
    client.assertNoErrors(p2);

    // Both projects list the shared unit; the one first by name is its
    // default, the other a candidate.
    const [shared] = await client.openAndWait("group-a/shared/generated.cpp");
    client.assertNoErrors(shared);
    expect(await client.inactiveLines(shared), "p1's command is the default").toEqual([3]);
    expect((await client.queryContext(shared)).total).toBe(2);
    expect(await client.waitForIndex(p1, "p2_main"), "both databases are indexed").toBe(true);
    expect(await client.waitForIndex(p1, "p1_main")).toBe(true);
});

test("root wins over subdirectory", async ({ session }) => {
    const { client } = await session("cdb/root_over_sub");
    const [main] = await client.openAndWait("main.cpp");
    expect(await client.inactiveLines(main), "the root's command applies").toEqual([3]);
    const [extra] = await client.openAndWait("extra.cpp");
    client.assertNoErrors(extra, "the subdirectory's database fills the gap");
    expect(client.drainedStderr().toString("utf8")).toContain(
        "No rule names a compilation database; the 2 found apply in this order",
    );
});

test("overlapping databases offer both", async ({ session }) => {
    const { client } = await session("cdb/two_out_dirs");
    const [main] = await client.openAndWait("main.cpp");
    expect(await client.inactiveLines(main), "out_debug comes first by name").toEqual([1]);
    const contexts = await client.queryContext(main);
    expect(contexts.total).toBe(2);
    const release = contexts.contexts.find((c) => c.label.includes("RELEASE"));
    expect(release).toBeDefined();
    const switched = await client.switchContext(main, main, {
        commandHash: release!.commandHash!,
    });
    expect(switched.success).toBe(true);
    await client.waitForRecompile(main);
    expect(await client.inactiveLines(main)).toEqual([3]);
});

test("vanished database yields to present", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", "#ifdef MOVED\nint moved = 1;\n#else\nint original = 1;\n#endif\n");
    workspace.write("only.cpp", gated("FEATURE"));
    workspace.writeEntries(
        [
            ["main.cpp", ["-DFEATURE"]],
            ["only.cpp", ["-DFEATURE"]],
        ],
        { at: "build/compile_commands.json" },
    );
    await client.initialize(workspace);
    const [main] = await client.openAndWait("main.cpp");
    expect(await client.inactiveLines(main)).toEqual([1]);
    const [only] = await client.openAndWait("only.cpp");
    client.assertNoErrors(only);

    // The build directory is wiped and regenerated elsewhere: the files
    // both databases list follow the present one, the rest keep serving.
    workspace.rm("build/compile_commands.json");
    workspace.writeCDB(["main.cpp"], {
        extraArgs: ["-DFEATURE", "-DMOVED"],
        at: "out/compile_commands.json",
    });
    expect(await cdbEvents(client), "the change settles for a tick").toBe(0);
    expect(await cdbEvents(client)).toBe(1);
    await client.waitForRecompile(main);
    expect(await client.inactiveLines(main), "out/ took the shared unit over").toEqual([3]);
    expect((await client.queryContext(main)).total, "the old entry is still offered").toBe(2);
    client.assertNoErrors(only, "the vanished database keeps serving its own files");

    // The original comes back and takes its place again.
    workspace.writeEntries(
        [
            ["main.cpp", ["-DFEATURE"]],
            ["only.cpp", ["-DFEATURE"]],
        ],
        { at: "build/compile_commands.json" },
    );
    expect(await cdbEvents(client)).toBe(0);
    expect(await cdbEvents(client)).toBe(1);
    await client.waitForRecompile(main);
    expect(await client.inactiveLines(main)).toEqual([1]);
});

test("response file change reloads", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", gated("FEATURE"));
    workspace.write("other.cpp", gated("OTHER"));
    workspace.write("flags.rsp", "-DFEATURE\n");
    workspace.writeEntries([
        ["main.cpp", ["@flags.rsp"]],
        ["other.cpp", ["-DOTHER"]],
    ]);
    await client.initialize(workspace);
    const [main] = await client.openAndWait("main.cpp");
    client.assertNoErrors(main, "the response file supplies FEATURE");
    const [other] = await client.openAndWait("other.cpp");
    client.assertNoErrors(other);

    await sleep(MTIME_GRANULARITY);
    workspace.write("flags.rsp", "-DCHANGED\n");
    expect(await cdbEvents(client), "the response file settles like the database").toBe(0);
    expect(await cdbEvents(client)).toBe(1);
    await client.waitForRecompile(main);
    client.assertHasErrors(main, "the reloaded command lost FEATURE");
    client.assertNoErrors(other, "a unit without the response file is untouched");
});

test("borrowed commands", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("src/lib.cpp", '#include "api.h"\nint lib() { return API; }\n');
    workspace.write("include/api.h", "#pragma once\n#define API 1\n");
    workspace.write("tools/tool.c", "int tool(void) { return 0; }\n");
    workspace.writeEntries([
        ["src/lib.cpp", ["-DFEATURE", "-Iinclude"]],
        ["tools/tool.c", ["-DTOOL"]],
    ]);
    await client.initialize(workspace);

    // A new source next to a unit borrows that unit's command.
    workspace.write("src/new.cpp", gated("FEATURE"));
    const [sibling] = await client.openAndWait("src/new.cpp");
    client.assertNoErrors(sibling, "the sibling's -DFEATURE applies");
    expect(guidance(client, sibling), "a working borrowed command needs no note").toBe(0);

    // A header under a unit's include path borrows the unit that searches it.
    workspace.write("include/extra.h", "#pragma once\n" + gated("FEATURE"));
    const [header] = await client.openAndWait("include/extra.h");
    client.assertNoErrors(header, "the unit with -Iinclude lends its command");

    // A C file never borrows a C++ command: the builtin fallback applies.
    workspace.write("src/plain.c", gated("TOOL"));
    const [plain] = await client.openAndWait("src/plain.c");
    client.assertNoErrors(plain, "the nearest C unit lends -DTOOL");
    workspace.write("elsewhere/lone.c", gated("FEATURE"));
    const [lone] = await client.openAndWait("elsewhere/lone.c");
    client.assertHasErrors(lone, "no C unit defines FEATURE and C++ ones do not lend");
});

test("header hosts match the language", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("c/impl.c", '#include "../shared/types.hpp"\nint impl(void) { return 0; }\n');
    workspace.write("shared/types.hpp", "#pragma once\n" + gated("CXX"));
    workspace.writeEntries([["c/impl.c", ["-DFROM_C"]]]);
    await client.initialize(workspace);

    const [header] = await client.openAndWait("shared/types.hpp");
    client.assertHasErrors(header, "a C++ header is not hosted by a C unit");
    expect((await client.queryContext(header)).total).toBe(0);
});

test("default command claims new files", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write(
        "clice.toml",
        '[[rules]]\npatterns = ["src/**"]\ndefault_command = "clang++ -std=c++20 -DFEATURE"\n',
    );
    workspace.write("src/main.cpp", gated("FEATURE"));
    await client.initialize(workspace);
    const [main] = await client.openAndWait("src/main.cpp");
    client.assertNoErrors(main);
    await client.poll("workspace");

    workspace.write("src/later.cpp", "int later_entry() { return 1; }\n");
    expect((await client.poll("workspace")).events, "the new member is reported").toBe(1);
    expect(await client.waitForIndex(main, "later_entry"), "a new member is indexed").toBe(true);
});
