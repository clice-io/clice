import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The interface's command embeds its sources in its module, as Bazel's
// does. A module embedding a file would override it in main.cpp, whose
// preamble's PCH holds the same file and then refuses to load: clice builds
// its modules from the files on disk.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.sync();
    expect((await s.counts()).files["main.cpp"]?.pch).toBe(1);
});
