import { expect } from "vitest";
import { test, type Serve } from "@clice/tools/testing";

/// How many files `request` looked at on disk.
async function looked(s: Serve, request: () => Promise<unknown>): Promise<number> {
    const before = await s.stats();
    await request();
    return (await s.stats()).checksLooked - before.checksLooked;
}

test("stale request looks once", async (t) => {
    // Finding the PCH stale, the request rebuilds it: the check, the PCH's
    // preparation and its build share one look at each header.
    for (const file of ["a.h", "b.h"]) {
        t.disk.touch(file, new Date("2020-01-01T00:00:00Z"));
    }
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("main.cpp")).toEqual([]);
    const hover = () => s.hover(t.at("main.cpp", "|main()"));
    const completion = () => s.completion(t.at("main.cpp", "return a() + |b()"));
    const freshHover = await looked(s, hover);
    const freshCompletion = await looked(s, completion);

    t.disk.write("b.h", "#pragma once\ninline int b() { return 22; }\n");
    expect(await looked(s, hover)).toBe(freshHover);
    t.disk.write("a.h", "#pragma once\ninline int a() { return 11; }\n");
    expect(await looked(s, completion)).toBe(freshCompletion);
});
