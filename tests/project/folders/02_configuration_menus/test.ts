import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("configuration menu per project", async (t) => {
    const s = await t.start({ folders: ["alpha", "beta"] });
    expect(await s.errors("alpha/main.cpp")).toEqual([]);
    expect(await s.errors("beta/main.cpp")).toEqual([]);
    await t.show("alpha", s.configurations("alpha/main.cpp"));
    await t.show("beta", s.configurations("beta/main.cpp"));
    expect(await s.switchConfiguration("slow", "beta/main.cpp")).toEqual({
        success: true,
        reason: null,
    });
    await t.show("beta switched", s.configurations("beta/main.cpp"));
    // Naming no file, the request asks the first folder.
    await t.show("no file", s.configurations());
});
