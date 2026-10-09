/// Index rows follow the sources on disk.

import { at, expect, serve } from "../../fixtures.ts";

const test = serve("shapes/headers");

test("deleted source withdraws its rows", async ({ s }) => {
    await s.clean("app/main.cpp");
    await s.indexed();
    const call = at("app/main.cpp", "registry_count()");
    expect(s.show(await s.definition(call))).toBe("src/registry.cpp: int registry_count() {");
    expect(s.show(await s.workspaceSymbols("registry_count"))).toBe(
        "registry_count src/registry.cpp: int registry_count() {",
    );

    // The database still lists the file; the rows it gave describe text
    // that is gone.
    s.disk.rm("src/registry.cpp");
    await s.sync({ poll: true });
    expect(s.show(await s.definition(call))).toBe(
        "include/shapes/registry.h: int registry_count();",
    );
    expect(
        s
            .show(await s.references(call))
            .split("\n")
            .sort(),
    ).toEqual([
        "app/main.cpp: return static_cast<int>(total) + shapes::registry_count();",
        "include/shapes/registry.h: int registry_count();",
    ]);
    expect(s.show(await s.workspaceSymbols("registry_count"))).toBe(
        "registry_count include/shapes/registry.h: int registry_count();",
    );

    await s.stop();
    const run = await s.cli("query", "--method", "symbolSearch", "--query", "registry_count");
    expect(run.status, run.stderr).toBe(0);
    expect(run.json).toMatchObject({
        result: {
            symbols: [
                {
                    name: "registry_count",
                    file: s.workspace.displayPath("include/shapes/registry.h"),
                },
            ],
        },
    });
});
