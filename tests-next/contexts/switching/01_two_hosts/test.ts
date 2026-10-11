import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("each host gives the header its own reading", async (t) => {
    const s = await t.start();
    expect(await s.diagnostics("int_host.cpp")).toEqual([]);
    expect(await s.diagnostics("real_host.cpp")).toEqual([]);
    s.open("shared.h");
    const decl = t.at("shared.h", "VALUE_TYPE |get_value");
    await s.switchContext("shared.h", "int_host.cpp");
    expect(await s.diagnostics("shared.h")).toEqual([]);
    await t.show("int host", s.hover(decl));
    await s.switchContext("shared.h", "real_host.cpp");
    expect(await s.diagnostics("shared.h")).toEqual([]);
    await t.show("real host", s.hover(decl));
});

test("a chosen host survives a restart", async (t) => {
    const s = await t.start();
    expect(await s.diagnostics("real_host.cpp")).toEqual([]);
    s.open("shared.h");
    await s.switchContext("shared.h", "real_host.cpp");
    await s.restart();
    const current = await s.currentContext("shared.h");
    expect(current.automatic).toBe(false);
    expect(s.relative(current.context?.uri ?? "")).toBe("real_host.cpp");
});
