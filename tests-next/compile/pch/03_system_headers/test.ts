import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("lookups are no change", async (t) => {
    // The standard library's lookups (`#include_next`, `__has_include`)
    // fail in some directories on the way: none of that is a change.
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.completion(t.at("main.cpp", "std::|cout"));
    s.edit("main.cpp", { replace: "return 0;", with: "return 1;" });
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.sync();
    expect((await s.counts()).files["main.cpp"]).toMatchObject({ compile: 2, pch: 1 });
});
