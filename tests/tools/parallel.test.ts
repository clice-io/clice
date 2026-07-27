/// Tests for the shared worker pool (tools/parallel.ts).

import { expect, test } from "vitest";
import { mapParallel } from "@clice/tools/parallel";

test("parallel map preserves order", async () => {
    // Reverse-sorted delays: completion order inverts input order, results
    // must not.
    const items = [30, 20, 10, 0];
    const results = await mapParallel(items, async (ms) => {
        await new Promise((resolve) => setTimeout(resolve, ms));
        return ms;
    });
    expect(results).toEqual(items);
});

test("parallel map bounds concurrency", async () => {
    let running = 0;
    let peak = 0;
    await mapParallel(
        Array.from({ length: 8 }, (_v, i) => i),
        async () => {
            running += 1;
            peak = Math.max(peak, running);
            await new Promise((resolve) => setTimeout(resolve, 5));
            running -= 1;
        },
        2,
    );
    expect(peak).toBeLessThanOrEqual(2);
});
