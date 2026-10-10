/// Integration tests for forced includes (`-include`): the dependency graph
/// scans them like any included header.

import { readManifest } from "@clice/tools/project";
import { expect, serve } from "../../fixtures.ts";

const FORCE_H = "#define FORCED 1\n";

const HEADERS = readManifest("shapes/headers");
const MODULES = readManifest("shapes/modules");
const FAST = MODULES.files!["fast"]!;
const FAST_ARGS = MODULES.units![FAST] as string[];

serve("shapes/headers", {
    files: { "force.h": FORCE_H },
    databases: {
        "compile_commands.json": {
            ...HEADERS,
            args: [...(HEADERS.args ?? []), "-include", "force.h"],
        },
    },
})("edits pay no import scan", async ({ s }) => {
    expect(await s.errors(s.file("main"))).toEqual([]);
    expect(await s.errors(s.file("circle"))).toEqual([]);
    s.edit(s.file("main"), {
        replace: "shapes::registry_count();",
        with: "shapes::registry_count() + 1;",
    });
    await s.compiled(s.file("main"));
    s.edit(s.file("circle"), { replace: "double pi = SHAPES_PI;", with: "double pi = FORCED;" });
    expect(await s.errors(s.file("circle")), "the header compiles through its host").toEqual([]);
    await s.indexed();
    const symbols = (await s.workspaceSymbols("registry_reset")) ?? [];
    expect(
        symbols.some((symbol) => symbol.name === "registry_reset"),
        "the closed unit is indexed",
    ).toBe(true);

    expect((await s.stats()).importScans).toBe(0);
});

serve("shapes/modules", {
    config: { project: { enable_indexing: false } },
    files: { "force.h": FORCE_H },
    units: { [FAST]: [...FAST_ARGS, "-include", "force.h"] },
})("modules elsewhere cost nothing", async ({ s }) => {
    await s.compiled(s.file("fast"));
    s.edit(s.file("fast"), {
        replace: "int fast_precision() {\n    return shapes_precision;",
        with: "int fast_precision() {\n    return shapes_precision + FORCED;",
    });
    await s.clean(s.file("fast"));
    expect((await s.stats()).importScans).toBe(0);
});

serve.files(
    { "force.h": "#define FORCED 1\n", "main.cpp": "int main() { return FORCED; }\n" },
    { manifest: { args: ["-include", "force.h"], units: { "main.cpp": [] } } },
)("saved forced header recompiles", async ({ s }) => {
    await s.clean("main.cpp");

    s.disk.write("force.h", "#define FORCED_RENAMED 1\n");
    s.save("force.h");
    expect(
        (await s.errors("main.cpp")).length,
        "the forced header no longer defines FORCED",
    ).toBeGreaterThan(0);
});
