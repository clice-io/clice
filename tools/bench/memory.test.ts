/// Tests for the memory watch over a process tree (tools/bench/memory.ts).

import { expect, test } from "vitest";
import { watchMemory } from "./memory.ts";

test("a watch stopped before its first sample still has a peak", () => {
    const { peak, end } = watchMemory(process.pid).stop();
    expect(end.root).toBeGreaterThan(0);
    expect(peak.root).toBeGreaterThanOrEqual(end.root);
    expect(peak.total).toBeGreaterThanOrEqual(end.root + end.children);
});
