/// Databases nobody declared: startup loads the root's and every direct
/// subdirectory's, opening a file registers the ones above it, a vanished
/// one yields to the present, and a file with neither an entry nor a host
/// borrows a nearby unit's command.

import { MTIME_GRANULARITY } from "@clice/tools/client";
import type { Serve } from "@clice/tools/actions";
import type { Manifest } from "@clice/tools/project";
import { expect, serve } from "../../fixtures.ts";

function gated(macro: string): string {
    return `#ifndef ${macro}\n#error missing ${macro}\n#endif\nint main() { return 0; }\n`;
}

async function guidance(s: Serve, file: string): Promise<string[]> {
    return (await s.compiled(file))
        .filter((d) => d.code === "inferred-compile-command")
        .map((d) => (typeof d.message === "string" ? d.message : d.message.value));
}

async function indexes(s: Serve, name: string): Promise<boolean> {
    return ((await s.workspaceSymbols(name)) ?? []).some((symbol) => symbol.name === name);
}

serve("layouts/nested_projects")("nested projects load on open", async ({ s }) => {
    expect(
        await s.errors("group-a/p1/main.cpp"),
        "opening a file registers its project's database",
    ).toEqual([]);
    expect(await s.errors("group-a/p2/main.cpp")).toEqual([]);

    const shared = "group-a/shared/generated.cpp";
    expect(await s.errors(shared)).toEqual([]);
    expect(await s.inactiveLines(shared), "p1's command is the default").toEqual([3]);
    expect((await s.contexts(shared)).total).toBe(2);
    await s.indexed();
    expect(await indexes(s, "p2_main"), "both databases are indexed").toBe(true);
    expect(await indexes(s, "p1_main")).toBe(true);
});

serve("layouts/root_over_sub")("root wins over subdirectory", async ({ s }) => {
    await s.compiled("main.cpp");
    expect(await s.inactiveLines("main.cpp"), "the root's command applies").toEqual([3]);
    expect(await s.errors("extra.cpp"), "the subdirectory's database fills the gap").toEqual([]);
    // The hint is a log line only; the log is complete once the server is down.
    await s.stop();
    expect(s.workspace.log("master.log")).toContain(
        "No rule names a compilation database; the 2 found apply in this order",
    );
});

serve("layouts/two_out_dirs")("overlapping databases offer both", async ({ s }) => {
    await s.compiled("main.cpp");
    expect(await s.inactiveLines("main.cpp"), "out_debug comes first by name").toEqual([1]);
    const contexts = await s.contexts("main.cpp");
    expect(contexts.total).toBe(2);
    const release = contexts.contexts.find((c) => c.label.includes("RELEASE"));
    expect(release).toBeDefined();
    const switched = await s.switchContext("main.cpp", "main.cpp", {
        commandHash: release!.commandHash!,
    });
    expect(switched.success).toBe(true);
    await s.compiled("main.cpp");
    expect(await s.inactiveLines("main.cpp")).toEqual([3]);
});

const ORIGINAL: Manifest = {
    cxx: ["-std=c++17"],
    units: { "main.cpp": ["-DFEATURE"], "only.cpp": ["-DFEATURE"] },
};

serve.files(
    {
        "main.cpp": "#ifdef MOVED\nint moved = 1;\n#else\nint original = 1;\n#endif\n",
        "only.cpp": gated("FEATURE"),
    },
    { databases: { "build/compile_commands.json": ORIGINAL } },
)("vanished database yields to present", async ({ s }) => {
    await s.compiled("main.cpp");
    expect(await s.inactiveLines("main.cpp")).toEqual([1]);
    expect(await s.errors("only.cpp")).toEqual([]);

    // The build directory is wiped: alone, a vanished database stays silent.
    s.disk.rm("build/compile_commands.json");
    expect(await s.poll("cdb", { force: false })).toBe(0);
    expect(await s.poll("cdb", { force: false })).toBe(0);

    // Regenerated elsewhere: the files both databases list follow the
    // present one, the rest keep serving.
    s.disk.database(
        { cxx: ["-std=c++17"], units: { "main.cpp": ["-DFEATURE", "-DMOVED"] } },
        "out/compile_commands.json",
    );
    expect(await s.poll("cdb", { force: false }), "the new database settles for a tick").toBe(0);
    expect(await s.poll("cdb", { force: false })).toBe(1);
    await s.compiled("main.cpp");
    expect(await s.inactiveLines("main.cpp"), "out/ took the shared unit over").toEqual([3]);
    expect((await s.contexts("main.cpp")).total, "the old entry is still offered").toBe(2);
    expect((await s.contexts("only.cpp")).total, "the vanished database's own entry").toBe(1);

    s.disk.database(ORIGINAL, "build/compile_commands.json");
    expect(await s.poll("cdb", { force: false })).toBe(0);
    expect(
        await s.poll("cdb", { force: false }),
        "the returning database takes its place back",
    ).toBe(1);
    await s.compiled("main.cpp");
    expect(await s.inactiveLines("main.cpp")).toEqual([1]);
});

serve.files(
    { "main.cpp": gated("FEATURE"), "other.cpp": gated("OTHER"), "flags.rsp": "-DFEATURE\n" },
    { manifest: { units: { "main.cpp": ["@flags.rsp"], "other.cpp": ["-DOTHER"] } } },
)("response file change reloads", async ({ s }) => {
    expect(await s.errors("main.cpp"), "the response file supplies FEATURE").toEqual([]);
    expect(await s.errors("other.cpp")).toEqual([]);

    // The rewrite keeps the size: only a later mtime tells the stamp it changed.
    s.disk.write("flags.rsp", "-DCHANGED\n");
    s.disk.touch("flags.rsp", new Date(s.disk.mtime("flags.rsp").getTime() + MTIME_GRANULARITY));
    expect(
        await s.poll("cdb", { force: false }),
        "the response file settles like the database",
    ).toBe(0);
    expect(await s.poll("cdb", { force: false })).toBe(1);
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
    s.disk.database(lenders(false));
    expect(await s.poll("cdb", { force: true })).toBe(1);
    expect(
        (await s.errors("zsrc/new.cpp")).length,
        "the borrowed command lost FEATURE",
    ).toBeGreaterThan(0);
    s.disk.database(lenders(true));
    expect(await s.poll("cdb", { force: true })).toBe(1);
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
    expect((await s.contexts("shared/types.hpp")).total).toBe(0);
    await s.compiled("shared/plain.h");
    expect((await s.contexts("shared/plain.h")).total, "a .h takes any host").toBe(1);
});

serve.files(
    {
        "clice.toml":
            '[[rules]]\npatterns = ["src/**"]\ndefault_command = "clang++ -std=c++20 -DFEATURE"\n',
        "src/main.cpp": gated("FEATURE"),
    },
    { databases: false },
)("default command claims new files", async ({ s }) => {
    expect(await s.errors("src/main.cpp")).toEqual([]);
    await s.poll("workspace");

    s.disk.write("src/later.cpp", "int later_entry() { return 1; }\n");
    expect(await s.poll("workspace"), "the new member is reported").toBe(1);
    await s.indexed();
    expect(await indexes(s, "later_entry"), "a new member is indexed").toBe(true);
});
