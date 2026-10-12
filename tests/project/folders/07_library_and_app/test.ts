import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const FOLDERS = { folders: ["app", "lib"] };

test("shared header symbol listed once", async (t) => {
    const s = await t.start(FOLDERS);
    await s.indexed();
    await t.show("shared_fn", s.workspaceSymbols("shared_fn"));
});

test("definition crosses folders", async (t) => {
    // The application's index only declares lib_fn; the library's defines it.
    const s = await t.start(FOLDERS);
    expect(await s.errors("app/main.cpp")).toEqual([]);
    await s.indexed();
    await t.show("lib_fn", s.definition(t.at("app/main.cpp", "return |lib_fn")));
});

test("references cross folders", async (t) => {
    const s = await t.start(FOLDERS);
    expect(await s.errors("lib/src/lib.cpp")).toEqual([]);
    await s.indexed();
    await t.show("lib_fn", s.references(t.at("lib/src/lib.cpp", "int |lib_fn")));
});

test("definition follows an open buffer", async (t) => {
    // The application builds the library's source too; the library, listed
    // first, serves it once open.
    t.disk.database({ "lib/src/lib.cpp": ["-Ilib/include"] }, "lib");
    t.disk.database(
        { "app/main.cpp": ["-Ilib/include"], "lib/src/lib.cpp": ["-Ilib/include"] },
        "app",
    );
    const s = await t.start({ folders: ["lib", "app"] });
    await s.indexed();
    expect(await s.errors("lib/src/lib.cpp")).toEqual([]);
    s.edit("lib/src/lib.cpp", { before: '#include "lib.h"', insert: "// moved\n" });
    expect(await s.errors("lib/src/lib.cpp")).toEqual([]);
    expect(await s.errors("app/main.cpp")).toEqual([]);
    await t.show("lib_fn", s.definition(t.at("app/main.cpp", "return |lib_fn")));
});

test("an open file answers through its project", async (t) => {
    // Both projects index the header; the library serves it once open, and
    // its unsaved buffer moved the declaration a line down.
    const s = await t.start(FOLDERS);
    await s.indexed();
    expect(await s.errors("lib/include/lib.h")).toEqual([]);
    s.edit("lib/include/lib.h", { before: "#pragma once", insert: "// moved\n" });
    expect(await s.errors("lib/include/lib.h")).toEqual([]);
    expect(await s.errors("app/main.cpp")).toEqual([]);
    await t.show("lib_fn", s.references(t.at("app/main.cpp", "return |lib_fn")));
});

test("save reaches every folder", async (t) => {
    // Saved from the library, the header moves its declaration a line down;
    // the application, which includes it too, is indexed again as well.
    const s = await t.start(FOLDERS);
    await s.indexed();
    expect(await s.errors("lib/include/lib.h")).toEqual([]);
    s.edit("lib/include/lib.h", { before: "#pragma once", insert: "// moved\n" });
    s.save("lib/include/lib.h");
    s.close("lib/include/lib.h");
    await s.indexed();
    expect(await s.errors("app/main.cpp")).toEqual([]);
    await t.show("lib_fn", s.references(t.at("app/main.cpp", "return |lib_fn")));
});
