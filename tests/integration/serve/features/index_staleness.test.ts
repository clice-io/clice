/// Index rows follow the sources on disk.

import { at, expect, serve } from "../../fixtures.ts";

const test = serve("shapes/headers");

test("deleted source withdraws its rows", async ({ s }) => {
    await s.clean("app/main.cpp");
    await s.indexed();
    const call = at("app/main.cpp", "registry_count()");
    expect(s.show(await s.definition(call))).toBe("src/registry.cpp: int registry_count() {");
    expect(
        s
            .show(await s.references(call))
            .split("\n")
            .sort(),
    ).toEqual([
        "app/main.cpp: return static_cast<int>(total) + shapes::registry_count();",
        "include/shapes/registry.h: int registry_count();",
        "src/registry.cpp: int registry_count() {",
        "src/registry.cpp: return registry_count();",
    ]);
    expect(s.show(await s.workspaceSymbols("registry_reset"))).toBe(
        "registry_reset src/registry.cpp: int registry_reset() {",
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
    expect(await s.workspaceSymbols("registry_reset")).toEqual([]);

    await s.stop();
    const run = await s.cli("query", "--method", "symbolSearch", "--query", "registry_reset");
    expect(run.status, run.stderr).toBe(0);
    expect(run.json).toMatchObject({ result: { symbols: [] } });
});

// No background sweep reaches b.cpp before the query.
serve.files(
    { "main.cpp": "int main() { return 0; }\n", "b.cpp": "int only_in_b() { return 2; }\n" },
    { config: { project: { idle_timeout_ms: 600_000 } } },
)("source deleted while down withdrawn", async ({ s }) => {
    await s.offline(async () => {
        const run = await s.cli("index");
        expect(run.status, run.stderr).toBe(0);
        expect(run.stdout).toContain("Indexed 2 translation units");
        s.disk.rm("b.cpp");
    });
    expect(await s.workspaceSymbols("only_in_b")).toEqual([]);
    expect(await s.workspaceSymbols("main")).toHaveLength(1);
});
