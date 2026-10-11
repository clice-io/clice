import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const PART = "#ifndef UNITY\n#error needs the unity file\n#endif\nint part() { return 0; }\n";

test("regenerated unity takes member", async (t) => {
    const s = await t.start();
    t.disk.write("part.cpp", PART);
    // A member of its own, without the unity file.
    await t.show("member", s.errors("part.cpp"));
    s.save("part.cpp");
    s.close("part.cpp");

    t.disk.write("unity.cpp", '#define UNITY 1\n#include "part.cpp"\n');
    t.disk.database({ "other.cpp": [], "unity.cpp": [] });
    expect(await s.poll("cdb")).toBe(1);
    // The regenerated unity file hosts it.
    expect(await s.errors("part.cpp")).toEqual([]);
});
