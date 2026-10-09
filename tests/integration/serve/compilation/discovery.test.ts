/// Databases nobody declared: startup loads the root's and every direct
/// subdirectory's, opening a file registers the ones above it, a vanished
/// one yields to the present, and a file with neither an entry nor a host
/// borrows a nearby unit's command.

import { MTIME_GRANULARITY, sleep } from "@clice/tools/client";
import type { Serve } from "@clice/tools/actions";
import { writeDatabase, type Manifest } from "@clice/tools/project";
import { expect, serve } from "../../fixtures.ts";

function gated(macro: string): string {
    return `#ifndef ${macro}\n#error missing ${macro}\n#endif\nint main() { return 0; }\n`;
}

async function guidance(s: Serve, file: string): Promise<string[]> {
    return (await s.compiled(file))
        .filter((d) => d.code === "inferred-compile-command")
        .map((d) => (typeof d.message === "string" ? d.message : d.message.value));
}

async function cdbEvents(s: Serve, force = false): Promise<number> {
    return (await s.client.poll("cdb", { force })).events;
}

async function inactiveLines(s: Serve, file: string): Promise<number[]> {
    await s.compiled(file);
    return s.client.inactiveLines(s.uri(file));
}

async function indexes(s: Serve, name: string): Promise<boolean> {
    return ((await s.workspaceSymbols(name)) ?? []).some((symbol) => symbol.name === name);
}

/// Start over without the database the fixture writes at the root; `write`
/// lays the case's own down while no server runs.
function dropRootDatabase(s: Serve, write: () => void = () => undefined): Promise<void> {
    return s.offline(() => {
        s.disk.rm("compile_commands.json");
        write();
    });
}

serve.data("cdb/nested_projects")("nested projects load on open", async ({ s }) => {
    expect(
        await s.errors("group-a/p1/main.cpp"),
        "opening a file registers its project's database",
    ).toEqual([]);
    expect(await s.errors("group-a/p2/main.cpp")).toEqual([]);

    const shared = "group-a/shared/generated.cpp";
    expect(await s.errors(shared)).toEqual([]);
    expect(await inactiveLines(s, shared), "p1's command is the default").toEqual([3]);
    expect((await s.client.queryContext(s.uri(shared))).total).toBe(2);
    await s.indexed();
    expect(await indexes(s, "p2_main"), "both databases are indexed").toBe(true);
    expect(await indexes(s, "p1_main")).toBe(true);
});

serve.data("cdb/root_over_sub")("root wins over subdirectory", async ({ s }) => {
    expect(await inactiveLines(s, "main.cpp"), "the root's command applies").toEqual([3]);
    expect(await s.errors("extra.cpp"), "the subdirectory's database fills the gap").toEqual([]);
    // The hint is a log line only; the log is complete once the server is down.
    await s.stop();
    expect(s.workspace.log("master.log")).toContain(
        "No rule names a compilation database; the 2 found apply in this order",
    );
});

serve.data("cdb/two_out_dirs")("overlapping databases offer both", async ({ s }) => {
    const main = s.uri("main.cpp");
    expect(await inactiveLines(s, "main.cpp"), "out_debug comes first by name").toEqual([1]);
    const contexts = await s.client.queryContext(main);
    expect(contexts.total).toBe(2);
    const release = contexts.contexts.find((c) => c.label.includes("RELEASE"));
    expect(release).toBeDefined();
    const switched = await s.client.switchContext(main, main, {
        commandHash: release!.commandHash!,
    });
    expect(switched.success).toBe(true);
    expect(await inactiveLines(s, "main.cpp")).toEqual([3]);
});

serve.files(
    {
        "main.cpp": "#ifdef MOVED\nint moved = 1;\n#else\nint original = 1;\n#endif\n",
        "only.cpp": gated("FEATURE"),
    },
    { manifest: { units: {} } },
)("vanished database yields to present", async ({ s }) => {
    const original: [string, string[]][] = [
        ["main.cpp", ["-DFEATURE"]],
        ["only.cpp", ["-DFEATURE"]],
    ];
    await dropRootDatabase(s, () => {
        s.workspace.writeEntries(original, { at: "build/compile_commands.json" });
    });
    expect(await inactiveLines(s, "main.cpp")).toEqual([1]);
    expect(await s.errors("only.cpp")).toEqual([]);

    // The build directory is wiped: alone, a vanished database stays silent.
    s.disk.rm("build/compile_commands.json");
    expect(await cdbEvents(s)).toBe(0);
    expect(await cdbEvents(s)).toBe(0);

    // Regenerated elsewhere: the files both databases list follow the
    // present one, the rest keep serving.
    s.workspace.writeCDB(["main.cpp"], {
        extraArgs: ["-DFEATURE", "-DMOVED"],
        at: "out/compile_commands.json",
    });
    expect(await cdbEvents(s), "the new database settles for a tick").toBe(0);
    expect(await cdbEvents(s)).toBe(1);
    expect(await inactiveLines(s, "main.cpp"), "out/ took the shared unit over").toEqual([3]);
    expect(
        (await s.client.queryContext(s.uri("main.cpp"))).total,
        "the old entry is still offered",
    ).toBe(2);
    expect(
        (await s.client.queryContext(s.uri("only.cpp"))).total,
        "the vanished database's own entry",
    ).toBe(1);

    s.workspace.writeEntries(original, { at: "build/compile_commands.json" });
    expect(await cdbEvents(s)).toBe(0);
    expect(await cdbEvents(s), "the returning database takes its place back").toBe(1);
    expect(await inactiveLines(s, "main.cpp")).toEqual([1]);
});

serve.files(
    { "main.cpp": gated("FEATURE"), "other.cpp": gated("OTHER"), "flags.rsp": "-DFEATURE\n" },
    { manifest: { units: { "main.cpp": ["@flags.rsp"], "other.cpp": ["-DOTHER"] } } },
)("response file change reloads", async ({ s }) => {
    expect(await s.errors("main.cpp"), "the response file supplies FEATURE").toEqual([]);
    expect(await s.errors("other.cpp")).toEqual([]);

    // The rewrite keeps the size: only its times tell the stamp it changed.
    await sleep(MTIME_GRANULARITY);
    s.disk.write("flags.rsp", "-DCHANGED\n");
    expect(await cdbEvents(s), "the response file settles like the database").toBe(0);
    expect(await cdbEvents(s)).toBe(1);
    expect(
        (await s.errors("main.cpp")).length,
        "the reloaded command lost FEATURE",
    ).toBeGreaterThan(0);
    expect(await s.errors("other.cpp"), "a unit without the response file is untouched").toEqual(
        [],
    );
});

const lenders = (feature: boolean): Manifest => ({
    c: ["-std=c17"],
    units: {
        "zsrc/lib.cpp": feature ? ["-DFEATURE", "-Iinclude"] : ["-Iinclude"],
        "include/near.cpp": [],
        "tools/tool.c": ["-DTOOL"],
    },
});

serve.files(
    {
        "zsrc/lib.cpp": '#include "api.h"\nint lib() { return API; }\n',
        "include/api.h": "#pragma once\n#define API 1\n",
        "include/near.cpp": "int near() { return 0; }\n",
        "tools/tool.c": "int tool(void) { return 0; }\n",
    },
    { manifest: lenders(true) },
)("nearby unit lends its command", async ({ s }) => {
    s.disk.write("zsrc/new.cpp", gated("FEATURE"));
    expect(await s.errors("zsrc/new.cpp"), "the sibling unit's -DFEATURE applies").toEqual([]);
    expect(await guidance(s, "zsrc/new.cpp")).toEqual([]);

    // The header sits under lib.cpp's -Iinclude: that unit lends, not the
    // nearer include/near.cpp.
    s.disk.write("include/api/extra.h", "#pragma once\n" + gated("FEATURE"));
    expect(
        await s.errors("include/api/extra.h"),
        "the unit searching the directory lends its command",
    ).toEqual([]);

    // C files borrow from C units only.
    s.disk.write("zsrc/plain.c", gated("TOOL"));
    expect(await s.errors("zsrc/plain.c"), "the C unit lends -DTOOL").toEqual([]);
    s.disk.write("elsewhere/lone.c", gated("FEATURE"));
    expect(
        (await s.errors("elsewhere/lone.c")).length,
        "FEATURE comes from C++ commands, which a .c never borrows",
    ).toBeGreaterThan(0);

    // The lender's command changes: the borrower follows.
    writeDatabase(s.workspace, lenders(false));
    expect(await cdbEvents(s, true)).toBe(1);
    expect(
        (await s.errors("zsrc/new.cpp")).length,
        "the borrowed command lost FEATURE",
    ).toBeGreaterThan(0);
    writeDatabase(s.workspace, lenders(true));
    expect(await cdbEvents(s, true)).toBe(1);
    expect(await s.errors("zsrc/new.cpp")).toEqual([]);
});

serve.files({ "src/lib.cpp": "int lib() { return 0; }\n" })(
    "borrowed command notes missing includes",
    async ({ s }) => {
        s.disk.write("src/new.cpp", '#include "nope.h"\n');
        expect((await s.errors("src/new.cpp")).length).toBeGreaterThan(0);
        expect(await guidance(s, "src/new.cpp")).toEqual([
            expect.stringContaining("borrowed from a nearby translation unit"),
        ]);
    },
);

serve.files(
    {
        "c/impl.c":
            '#include "../shared/types.hpp"\n#include "../shared/plain.h"\nint impl(void) { return 0; }\n',
        "shared/types.hpp": "#pragma once\n" + gated("CXX"),
        "shared/plain.h": "#pragma once\n",
    },
    { manifest: { c: ["-std=c17"], units: { "c/impl.c": ["-DFROM_C"] } } },
)("header hosts match the language", async ({ s }) => {
    expect(
        (await s.errors("shared/types.hpp")).length,
        "a C++ header is not hosted by a C unit",
    ).toBeGreaterThan(0);
    expect((await s.client.queryContext(s.uri("shared/types.hpp"))).total).toBe(0);
    await s.compiled("shared/plain.h");
    expect(
        (await s.client.queryContext(s.uri("shared/plain.h"))).total,
        "a .h takes any host",
    ).toBe(1);
});

serve.files(
    {
        "clice.toml":
            '[[rules]]\npatterns = ["src/**"]\ndefault_command = "clang++ -std=c++20 -DFEATURE"\n',
        "src/main.cpp": gated("FEATURE"),
    },
    { manifest: { units: {} } },
)("default command claims new files", async ({ s }) => {
    await dropRootDatabase(s);
    expect(await s.errors("src/main.cpp")).toEqual([]);
    await s.client.poll("workspace");

    s.disk.write("src/later.cpp", "int later_entry() { return 1; }\n");
    expect((await s.client.poll("workspace")).events, "the new member is reported").toBe(1);
    await s.indexed();
    expect(await indexes(s, "later_entry"), "a new member is indexed").toBe(true);
});
