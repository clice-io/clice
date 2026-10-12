import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const TMP = ".clice/cache/*/tmp/**";

test("killed mid-build", async (t) => {
    const s = await t.start();
    // The server dies with the PCH and its index written, not yet
    // committed to the cache.
    const build = await s.park("pch", "main.cpp", { at: "reply" });
    s.open("main.cpp");
    await build.reached();
    expect(t.disk.list(TMP)).toHaveLength(2);
    await s.kill();
    // The next server sweeps what the dead one left and builds again.
    await s.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(t.disk.list(TMP)).toEqual([]);
    await s.sync();
    expect((await s.counts()).pch).toBe(1);
    expect(t.disk.list(".clice/cache/*/pch/*")).toHaveLength(2);
});
