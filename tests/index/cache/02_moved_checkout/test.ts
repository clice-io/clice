import { expect } from "vitest";
import { test, type TestContext } from "@clice/tools/testing";

// A checkout moved to another directory, its cache with it and its
// database regenerated: the index is still current, and a macro and a
// file-local symbol keep their ids, though the latter's locator names its
// file, which moved, and a search filtering by path finds it there.
function database(t: TestContext, checkout: string): void {
    const include = `-I${t.root}/${checkout}/inc`;
    t.disk.database(
        { [`${checkout}/src/a.cpp`]: [include], [`${checkout}/src/b.cpp`]: [include] },
        checkout,
    );
}

async function ids(t: TestContext, checkout: string): Promise<string[]> {
    const found: string[] = [];
    for (const text of ["UTIL_LIMIT", "src/a.cpp:2", "helper path:src/a.cpp"]) {
        const search = await t.cli(["query", "symbol-search", text], { cwd: checkout });
        const { result } = search.json as { result: { symbols: { symbol: string }[] } };
        found.push(result.symbols[0]?.symbol.split(" ")[0] ?? "none");
    }
    return found;
}

test(async (t) => {
    database(t, "before");
    await t.cli("index --workspace before");
    const before = await ids(t, "before");
    t.disk.move("before", "after");
    database(t, "after");
    await t.cli("index --workspace after");
    expect(await ids(t, "after")).toEqual(before);
});
