import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test(async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("main.cpp", {
        inactive: s.inactiveLines("main.cpp"),
        contexts: s.contexts("main.cpp"),
    });
});
