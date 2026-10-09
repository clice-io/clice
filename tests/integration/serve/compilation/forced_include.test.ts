/// Integration tests for forced includes (`-include`): the dependency graph
/// scans them like any included header.

import { expect, serve } from "../../fixtures.ts";

serve.files(
    {
        "force.h": "#define FORCED 1\n",
        "header.h": "inline int header() { return FORCED; }\n",
        "main.cpp": '#include "header.h"\nint main() { return header(); }\n',
        "other.cpp": "int other() { return FORCED; }\n",
    },
    { manifest: { args: ["-include", "force.h"], units: { "main.cpp": [], "other.cpp": [] } } },
)("edits pay no import scan", async ({ s }) => {
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await s.errors("header.h")).toEqual([]);
    s.edit("main.cpp", { replace: "header();", with: "header() + 1;" });
    await s.compiled("main.cpp");
    s.edit("header.h", { replace: "FORCED", with: "FORCED + 1" });
    expect(await s.errors("header.h"), "the header compiles through its host").toEqual([]);
    await s.indexed();
    const symbols = (await s.workspaceSymbols("other")) ?? [];
    expect(
        symbols.some((symbol) => symbol.name === "other"),
        "the closed unit is indexed",
    ).toBe(true);

    expect((await s.client.stats()).importScans).toBe(0);
});

serve.files(
    {
        "force.h": "#define FORCED 1\n",
        "main.cpp": "int main() { return FORCED; }\n",
        "a.cppm": "export module A;\nexport int a() { return 1; }\n",
        "user.cpp": "import A;\nint user() { return a(); }\n",
    },
    {
        config: { project: { enable_indexing: false } },
        manifest: {
            cxx: ["-std=c++20"],
            units: { "main.cpp": ["-include", "force.h"], "a.cppm": [], "user.cpp": [] },
        },
    },
)("modules elsewhere cost nothing", async ({ s }) => {
    await s.compiled("main.cpp");
    s.edit("main.cpp", { replace: "FORCED;", with: "FORCED + 1;" });
    await s.clean("main.cpp");
    expect((await s.client.stats()).importScans).toBe(0);
});

serve.files(
    { "force.h": "#define FORCED 1\n", "main.cpp": "int main() { return FORCED; }\n" },
    { manifest: { args: ["-include", "force.h"], units: { "main.cpp": [] } } },
)("saved forced header recompiles", async ({ s }) => {
    await s.clean("main.cpp");

    s.disk.write("force.h", "#define FORCED_RENAMED 1\n");
    s.client.save(s.uri("force.h"));
    expect(
        (await s.errors("main.cpp")).length,
        "the forced header no longer defines FORCED",
    ).toBeGreaterThan(0);
});
