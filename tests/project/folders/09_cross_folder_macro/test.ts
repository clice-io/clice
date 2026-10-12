import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("shared macro references cross folders", async (t) => {
    // The macro's id takes its header relative to the library's root in the
    // library and absolute in the application; the two meet at its
    // definition.
    const s = await t.start({ folders: ["app", "lib"] });
    expect(await s.errors("lib/src/lib.cpp")).toEqual([]);
    await s.indexed();
    await t.show("LIB_LIMIT", s.references(t.at("lib/src/lib.cpp", "return |LIB_LIMIT")));
});
