import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("requests look at workspace files only", async (t) => {
    // clang-tidy's configuration lookups would add looks of their own.
    const s = await t.start({
        config: { project: { enable_indexing: false }, diagnostics: { clang_tidy: false } },
    });
    expect(await s.errors("main.cpp")).toEqual([]);
    const use = t.at("main.cpp", "return |INSTALLED");
    await s.hover(use);
    const before = await s.stats();
    await s.hover(use);
    const after = await s.stats();
    // The workspace header is looked at, the installed one trusted.
    expect(after.checksLooked - before.checksLooked).toBe(1);
    expect(after.checksTrusted - before.checksTrusted).toBe(1);
});

test("save looks at installed headers", async (t) => {
    t.disk.touch("sysroot/usr/include/installed.h", new Date("2020-01-01T00:00:00Z"));
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("installed 1", s.errors("check.cpp"));
    // An upgrade rewrites the installed header; nothing asks until a save.
    t.disk.write("sysroot/usr/include/installed.h", "#define INSTALLED 2\n");
    s.save("check.cpp");
    expect(await s.errors("check.cpp")).toEqual([]);
});
