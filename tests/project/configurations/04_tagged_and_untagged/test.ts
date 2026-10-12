import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("tagged rules switch while untagged stay", async (t) => {
    const s = await t.start();
    expect(await s.errors("firmware/main.c")).toEqual([]);
    await t.show("board-a", s.inactiveLines("firmware/main.c"));
    // The untagged rule's database serves tools/.
    expect(await s.errors("tools/gen.cpp")).toEqual([]);
    expect(await s.switchConfiguration("board-b")).toEqual({ success: true });
    await s.restart();
    expect(await s.errors("firmware/main.c")).toEqual([]);
    await t.show("board-b", s.inactiveLines("firmware/main.c"));
    expect(await s.errors("tools/gen.cpp")).toEqual([]);
});
