import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// h.h compiles on its own until foo.h stops defining FOO: its own include's
// change runs the trial again, and only main.cpp's define is left.
test(async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("before", s.diagnostics("h.h"));
    expect((await s.stats()).synthesizedContexts).toBe(0);
    const written = t.disk.mtime("foo.h");
    t.disk.write("foo.h", "#pragma once\n");
    t.disk.touch("foo.h", new Date(written.getTime() + 10_000));
    await t.show("after", s.diagnostics("h.h"));
    expect((await s.stats()).synthesizedContexts).toBe(1);
});
