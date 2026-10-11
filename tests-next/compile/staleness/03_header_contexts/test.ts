import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// utils.h and target.h have no command of their own: each compiles after
// what main.cpp includes before it. The files on the way are inputs too.

test("host change reaches the header", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await s.errors("utils.h")).toEqual([]);
    // The open host's buffer is its own; the header follows the disk.
    t.disk.edit("main.cpp", { remove: '#include "types.h"\n' });
    await t.show("utils.h", s.errors("utils.h"));
});

test("intermediate change reaches the header", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await s.errors("target.h")).toEqual([]);
    t.disk.edit("wrapper.h", { replace: "#define VALUE 42", with: "#define OTHER 42" });
    await t.show("target.h", s.errors("target.h"));
});

test("rewrite keeps its stamp", async (t) => {
    // An old file rewritten in place with its size and time put back, no
    // save sent: its change time still tells.
    const stamp = new Date("2020-01-01T00:00:00Z");
    t.disk.touch("main.cpp", stamp);
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await s.errors("utils.h")).toEqual([]);
    t.disk.edit("main.cpp", { replace: '#include "types.h"', with: "#define TYPES_GONE" });
    t.disk.touch("main.cpp", stamp);
    await t.show("utils.h", s.errors("utils.h"));
});
