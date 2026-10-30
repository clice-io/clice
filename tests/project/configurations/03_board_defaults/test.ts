import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test(async (t) => {
    const s = await t.start();
    await t.show("menu", s.configurations());
    expect(await s.errors("src/main.c")).toEqual([]);
    expect(await s.errors("include/board.h")).toEqual([]);
    await t.show("board-a", s.inactiveLines("include/board.h"));
    expect(await s.switchConfiguration("board-b")).toEqual({ success: true, reason: null });
    await s.restarted();
    expect(await s.errors("src/main.c")).toEqual([]);
    expect(await s.errors("include/board.h")).toEqual([]);
    await t.show("board-b", s.inactiveLines("include/board.h"));
});
