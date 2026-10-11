import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The rule's default command claims part.cpp, which compiles under it: no
// host is picked for it, though main.cpp includes it.
test(async (t) => {
    const s = await t.start();
    expect(await s.errors("src/main.cpp")).toEqual([]);
    expect(await s.errors("src/part.cpp")).toEqual([]);
    await t.show("current", s.currentContext("src/part.cpp"));
});
