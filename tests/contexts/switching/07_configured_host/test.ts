import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A host built two ways gives the header a context per command, each with a
// hash of its own to switch by.
test(async (t) => {
    const s = await t.start();
    expect(await s.errors("host.cpp")).toEqual([]);
    expect(await s.errors("render.h")).toEqual([]);
    await t.show("contexts", s.contexts("render.h"));
    await t.show("automatic", s.inactiveLines("render.h"));
    const listed = (await s.contexts("render.h")).contexts;
    const hashes = new Set(listed.map((c) => c.commandHash ?? ""));
    expect([hashes.size, hashes.has("")]).toEqual([2, false]);
    const metal = listed.find((c) => c.label.includes("USE_METAL"))?.commandHash ?? "";
    expect((await s.switchContext("render.h", "host.cpp", { commandHash: metal })).success).toBe(
        true,
    );
    await s.sync();
    expect((await s.currentContext("render.h")).context?.commandHash).toBe(metal);
    expect(await s.errors("render.h")).toEqual([]);
    await t.show("metal", s.inactiveLines("render.h"));
});
