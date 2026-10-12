import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The listing comes in pages of ten.
test(async (t) => {
    const s = await t.start();
    expect(await s.errors("s00.cpp")).toEqual([]);
    s.open("common.h");
    await t.show("first page", s.contexts("common.h"));
    await t.show("second page", s.contexts("common.h", { offset: 10 }));
    const [first, second] = [
        await s.contexts("common.h"),
        await s.contexts("common.h", { offset: 10 }),
    ];
    expect([first.total, second.total]).toEqual([11, 11]);
});
