import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The condition's macro is among the leading directives, so the edit changes
// what the server compiles ahead of the rest.
test(async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("before", s.inactiveLines("main.cpp"));
    s.edit("main.cpp", { replace: "#define FEATURE 0", with: "#define FEATURE 1" });
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("after", s.inactiveLines("main.cpp"));
});
