import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("units share a preamble's pch", async (t) => {
    const s = await t.start();
    // b.cpp reuses what a.cpp built; c.cpp includes another header and
    // d.cpp defines MODE, so each needs its own. Each compiling clean
    // shows it got the PCH of its own flags.
    const totals = { "a.cpp": 1, "b.cpp": 1, "c.cpp": 2, "d.cpp": 3 };
    for (const [file, total] of Object.entries(totals)) {
        expect(await s.errors(file)).toEqual([]);
        await s.sync();
        expect((await s.counts()).pch).toBe(total);
    }
});
