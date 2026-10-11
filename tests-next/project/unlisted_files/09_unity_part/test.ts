import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const PART = "#ifndef UNITY\n#error needs the unity file\n#endif\nint part() { return 0; }\n";

test("included source keeps host", async (t) => {
    const s = await t.start();
    await t.show("missing", s.errors("unity.cpp"));
    t.disk.write("part.cpp", PART);
    await s.diagnostics("part.cpp");
    s.save("part.cpp");
    // A source a unit includes is no member of its own.
    s.edit("part.cpp", {
        after: "int part() { return 0; }\n",
        insert: "int more() { return 1; }\n",
    });
    expect(await s.errors("part.cpp")).toEqual([]);
    // Nor after a restart.
    await s.restart();
    expect(await s.errors("part.cpp")).toEqual([]);
});
