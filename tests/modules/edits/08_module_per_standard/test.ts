import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const CHANGED = "export module m;\n\nexport constexpr long standard = __cplusplus + 1;\n";

// newer.cpp reads the module's build under its C++23 entry, older.cpp the
// build under its own command: a change of the module reaches both.
test("changed on disk", async (t) => {
    const s = await t.start();
    expect(await s.errors("newer.cpp")).toEqual([]);
    expect(await s.errors("older.cpp")).toEqual([]);
    t.disk.write("m.cppm", CHANGED);
    await s.sync({ poll: true });
    expect(await s.errors("newer.cpp")).toEqual([]);
    await t.show("newer", s.hover(t.at("newer.cpp", "return |standard;")));
    expect(await s.errors("older.cpp")).toEqual([]);
    await t.show("older", s.hover(t.at("older.cpp", "return |standard;")));
});

// The database gains the module's C++23 entry, as a CMake reconfiguration
// adds a target: newer.cpp, which could not read the C++20 build, reads the
// new one.
test("entry added", async (t) => {
    t.disk.database({ "m.cppm": [], "older.cpp": [], "newer.cpp": ["-std=c++23"] });
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    const refused = await s.errors("newer.cpp");
    expect(refused.some((error) => error.message.includes("C++23 was disabled"))).toBe(true);
    t.disk.database();
    expect(await s.poll("cdb")).toBe(1);
    expect(await s.errors("newer.cpp")).toEqual([]);
    await t.show("newer", s.hover(t.at("newer.cpp", "return |standard;")));
});
