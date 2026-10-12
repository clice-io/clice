import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test(async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    s.open("utils.h");
    expect(await s.switchContext("utils.h", "other.cpp")).toEqual({ success: false, stale: false });
    await t.show("current", s.currentContext("utils.h"));
});
