/// The action layer's own options and actions: how a case lays out its
/// workspace and starts its server, and the reads that start no work.

import * as fs from "node:fs";
import { at, expect, serve } from "../../fixtures.ts";

const CXX = ["-std=c++17"];

serve.files(
    {
        "src/main.cpp": '#include "gen.h"\nint main() { return GEN; }\n',
        "inc/gen.h": "#define GEN 0\n",
    },
    { manifest: { cxx: CXX, units: { "src/main.cpp": ["-I${workspace}/inc"] } } },
)("manifest arguments name the workspace", async ({ s }) => {
    await s.clean("src/main.cpp");
});

serve.files(
    { "main.cpp": "int main() { return VALUE; }\n" },
    { manifest: { cxx: CXX, units: { "main.cpp": [["-DVALUE=1"], ["-DVALUE=2"]] } } },
)("a unit of two configurations", async ({ s }) => {
    const entries = JSON.parse(s.disk.read("compile_commands.json")) as { arguments: string[] }[];
    expect(entries.map((entry) => entry.arguments.find((arg) => arg.startsWith("-D")))).toEqual([
        "-DVALUE=1",
        "-DVALUE=2",
    ]);
    await s.clean("main.cpp");
});

serve.files(
    {
        "real/lib.h": "int lib();\n",
        "main.cpp": '#include "link/lib.h"\nint main() { return lib(); }\n',
        "build/compile_commands.json": (workspace) =>
            JSON.stringify([
                {
                    directory: workspace.root,
                    file: workspace.path("main.cpp"),
                    arguments: ["clang++", ...CXX, "-fsyntax-only", workspace.path("main.cpp")],
                },
            ]),
    },
    {
        databases: false,
        setup: (workspace) => {
            fs.symlinkSync(workspace.path("real"), workspace.path("link"), "junction");
        },
    },
)("own database and a symlink", async ({ s }) => {
    expect(s.disk.read("build/compile_commands.json")).toContain("main.cpp");
    expect(fs.existsSync(s.workspace.path("compile_commands.json"))).toBe(false);
    await s.clean("main.cpp");
});

serve.files({ "main.cpp": "int main() {}\n" }).skipIf(true)("a skipped case never starts", () => {
    throw new Error("skipIf(true) ran the case");
});

serve.files({
    "main.cpp": '#include "header.h"\nint main() { return value(); }\n',
    "header.h": "inline int value() { return 0; }\n",
})("pushed reads what the server did unasked", async ({ s }) => {
    s.open("main.cpp", { pull: false });
    expect(await s.pushed("main.cpp"), "an open alone compiles nothing").toBeUndefined();
    await s.clean("main.cpp");
    expect(await s.pushed("main.cpp")).toEqual([]);

    // The header is not open: its save says the disk changed. The includer
    // recompiles on the next request, not on its own.
    s.disk.write("header.h", "inline int value() { return missing; }\n");
    s.save("header.h");
    expect(await s.pushed("main.cpp"), "nothing asked for main.cpp yet").toEqual([]);
    expect(await s.errors("main.cpp")).toHaveLength(1);
    expect(await s.pushed("main.cpp")).toHaveLength(1);
});

serve.files({ "main.cpp": "int main() { return 0; }\n" })("kill keeps the cache", async ({ s }) => {
    await s.clean("main.cpp");
    await s.indexed();
    await s.kill();
    await s.start();
    await s.clean("main.cpp");
    expect(await s.poll("workspace")).toBe(0);
    expect((await s.stats()).sessions).toBe(1);
});

serve.files(
    { "main.cpp": "int main() { return 0; }\n" },
    { launch: { config: { project: { enable_indexing: false } } } },
)("a server starts with its own config", async ({ s }) => {
    await s.clean("main.cpp");
    await s.sync();
    expect((await s.counts()).index, "the case's launch keeps indexing off").toBe(0);
    await s.stop();
    await s.start({});
    await s.indexed();
    expect((await s.counts()).index, "a launch of its own runs the default config").toBe(1);
});

serve.files({
    "main.cpp": "struct Shape {};\nstruct Square : Shape {};\nint main() {}\n",
})("hierarchy and requests by loc", async ({ s }) => {
    await s.clean("main.cpp");
    const bases = await s.supertypes(at("main.cpp", "Square :"));
    expect(bases?.map((item) => item.name)).toEqual(["Shape"]);
    const derived = await s.subtypes(at("main.cpp", "struct |Shape"));
    expect(derived?.map((item) => item.name)).toEqual(["Square"]);
    const [hovered, symbols] = await Promise.all([
        s.hover(at("main.cpp", "Square :")),
        s.workspaceSymbols("Shape"),
    ]);
    expect(s.show(hovered)).toContain("Square");
    expect(symbols?.length).toBeGreaterThan(0);
});
