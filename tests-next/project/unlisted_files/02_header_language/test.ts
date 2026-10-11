import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("header hosts match the language", async (t) => {
    const s = await t.start();
    // A C++ header is not hosted by a C unit; a .h takes any host.
    await t.show("types.hpp", {
        errors: s.errors("shared/types.hpp"),
        contexts: s.contexts("shared/types.hpp"),
    });
    expect(await s.errors("shared/plain.h")).toEqual([]);
    await t.show("plain.h", s.contexts("shared/plain.h"));
});
