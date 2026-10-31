import { expect } from "vitest";
import { test, type TestContext } from "@clice/tools/testing";

/// Whether the directory holds any file.
function exists(t: TestContext, dir: string): boolean {
    return t.disk.list(`${dir}/**`).length > 0;
}

test("nested folder joins its project", async (t) => {
    // Listed first, the nested folder still is no project of its own.
    const s = await t.start({ folders: ["outer/inner", "outer"] });
    expect(await s.errors("outer/inner/main.cpp")).toEqual([]);
    // One project, holding the client's cache directory: neither folder
    // fell back to a default one.
    expect(exists(t, "outer/.clice")).toBe(false);
    expect(exists(t, "outer/inner/.clice")).toBe(false);

    // Alone, the nested folder is a project that knows no command for it.
    await s.changeFolders({ removed: ["outer"] });
    await s.sync();
    await t.show("alone", s.errors("outer/inner/main.cpp"));
    // The enclosing project takes the folder back.
    await s.changeFolders({ added: ["outer"] });
    await s.sync();
    expect(await s.errors("outer/inner/main.cpp")).toEqual([]);
    // A move keeps what the document shows until the project taking it over
    // replaces it: no empty push in between.
    await t.show("pushes", s.pushes("outer/inner/main.cpp"));
});
