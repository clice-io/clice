import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("cdb removed entry recheck", async (t) => {
    const s = await t.start();
    await s.sync();
    await t.show("listed", s.contexts("header.h"));
    t.disk.database({});
    expect(await s.poll("cdb")).toBe(1);
    // The removed entry stops hosting the header.
    await t.show("removed", s.contexts("header.h"));
});
