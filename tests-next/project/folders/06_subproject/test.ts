import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("subproject serves what the folder does not build", async (t) => {
    const s = await t.start({ folders: ["mono"] });
    // The folder's build compiles the file it lists.
    expect(await s.errors("mono/sub/vendored.cpp")).toEqual([]);
    // The subproject compiles the rest with its own database.
    expect(await s.errors("mono/sub/tool.cpp")).toEqual([]);
    // The listed file stays with the folder.
    expect(await s.errors("mono/sub/vendored.cpp")).toEqual([]);
});
