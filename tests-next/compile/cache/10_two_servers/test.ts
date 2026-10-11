import { expect } from "vitest";
import { test, type Serve } from "@clice/tools/testing";

async function pchBuilds(s: Serve): Promise<number> {
    await s.sync();
    return (await s.counts()).pch;
}

test("two servers share the cache", async (t) => {
    // Two editor windows on one project: one cache, two servers.
    const first = await t.start();
    expect(await first.errors("main.cpp")).toEqual([]);
    const second = await t.start();
    expect(await second.errors("main.cpp")).toEqual([]);
    t.disk.write("header.h", "#pragma once\n\nstruct Shared {\n    int y;\n};\n");
    await t.show("changed", first.errors("main.cpp"));
    const changed = await first.errors("main.cpp");
    expect(await second.errors("main.cpp")).toEqual(changed);
    // Each built for itself, and neither spoiled the other's work.
    expect({ first: await pchBuilds(first), second: await pchBuilds(second) }).toEqual({
        first: 2,
        second: 2,
    });
    await second.stop();
    await first.restart();
    expect(await first.errors("main.cpp")).toEqual(changed);
    expect(await pchBuilds(first)).toBe(0);
});
