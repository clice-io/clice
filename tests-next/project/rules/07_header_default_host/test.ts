import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("header borrows default command host", async (t) => {
    const s = await t.start();
    expect(await s.errors("include/x.h")).toEqual([]);
    await t.show("hosts", s.contexts("include/x.h"));
});
