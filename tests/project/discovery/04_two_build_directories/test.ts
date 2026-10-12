import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("overlapping databases offer both", async (t) => {
    t.disk.database({ "main.cpp": ["-DDEBUG"] }, "out_debug");
    t.disk.database({ "main.cpp": ["-DRELEASE"] }, "out_release");
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    // out_debug comes first by name.
    await t.show("first", s.inactiveLines("main.cpp"));
    const listed = await s.contexts("main.cpp");
    await t.show("contexts", s.contexts("main.cpp"));
    const release = listed.contexts.find((context) => context.label === "-DRELEASE");
    const switched = await s.switchContext("main.cpp", "main.cpp", {
        commandHash: release?.commandHash ?? "",
    });
    expect(switched.success).toBe(true);
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("switched", s.inactiveLines("main.cpp"));
});
