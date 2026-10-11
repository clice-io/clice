import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("each host gives the header its own reading", async (t) => {
    const s = await t.start();
    expect(await s.errors("int_host.cpp")).toEqual([]);
    expect(await s.errors("real_host.cpp")).toEqual([]);
    expect(await s.errors("shared.h")).toEqual([]);
    const decl = t.at("shared.h", "VALUE_TYPE |get_value");
    await t.show("automatic", { context: s.currentContext("shared.h"), hover: s.hover(decl) });
    expect((await s.switchContext("shared.h", "real_host.cpp")).success).toBe(true);
    expect(await s.errors("shared.h")).toEqual([]);
    await t.show("real host", { context: s.currentContext("shared.h"), hover: s.hover(decl) });
    expect((await s.switchContext("shared.h", "int_host.cpp")).success).toBe(true);
    expect(await s.errors("shared.h")).toEqual([]);
    await t.show("int host", { context: s.currentContext("shared.h"), hover: s.hover(decl) });
    expect((await s.switchContext("shared.h", "real_host.cpp")).success).toBe(true);
    expect((await s.resetContext("shared.h")).success).toBe(true);
    expect(await s.errors("shared.h")).toEqual([]);
    await t.show("reset", { context: s.currentContext("shared.h"), hover: s.hover(decl) });
});

test("a choice survives a restart", async (t) => {
    const s = await t.start();
    expect(await s.errors("real_host.cpp")).toEqual([]);
    s.open("shared.h");
    expect((await s.switchContext("shared.h", "real_host.cpp")).success).toBe(true);
    await s.restart();
    await t.show("chosen", s.currentContext("shared.h"));
    expect((await s.resetContext("shared.h")).success).toBe(true);
    await s.restart();
    await t.show("reset", s.currentContext("shared.h"));
});
