import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// main.cpp asks whether files exist that are not there yet; table.cpp
// embeds a file that is.

test("embedded data changes", async (t) => {
    const s = await t.start();
    expect(await s.errors("table.cpp")).toEqual([]);
    t.disk.write("table.bin", "ABC");
    await t.show("table.bin", s.errors("table.cpp"));
});

test("checked header appears", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    t.disk.write("config.h", "#pragma once\n");
    await t.show("config.h", s.errors("main.cpp"));
});

test("checked data appears", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    t.disk.write("data.bin", "AB");
    await s.sync({ poll: true });
    await t.show("data.bin", s.errors("main.cpp"));
});
