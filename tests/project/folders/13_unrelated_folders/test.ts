import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("unrelated folders keep references apart", async (t) => {
    const s = await t.start({ folders: ["alpha", "beta"] });
    expect(await s.errors("alpha/main.cpp")).toEqual([]);
    await s.indexed();
    await t.show("users", s.workspaceSymbols("use_"));
    // The same name and symbol id, but beta's index holds no file declaring
    // alpha's helper.
    await t.show("helper", s.references(t.at("alpha/main.cpp", "int |helper")));
});
