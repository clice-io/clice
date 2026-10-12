import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("buffer branch reaches the header", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.indexed();
    // The buffer takes the branch no compile from disk has seen: only the
    // rebuilt preamble knows only_a.
    s.edit(
        "main.cpp",
        { before: '#include "crypto.h"', insert: "#define USE_A 1\n" },
        { replace: "only_b();", with: "only_a();" },
    );
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("only_a", s.definition(t.at("main.cpp", "|only_a();")));
});
