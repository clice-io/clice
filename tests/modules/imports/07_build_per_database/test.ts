import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// Two build directories list the module and its importer, the release one
// with -DNDEBUG, which clang lets a module and its importer differ in:
// switched to its release entry, the importer reads the module's release
// build, as the real build does.
test(async (t) => {
    t.disk.database({ "m.cppm": [], "user.cpp": [] }, "out_debug");
    t.disk.database({ "m.cppm": ["-DNDEBUG"], "user.cpp": ["-DNDEBUG"] }, "out_release");
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("user.cpp")).toEqual([]);
    const use = t.at("user.cpp", "return |checked;");
    await t.show("debug", s.hover(use));
    const { contexts } = await s.contexts("user.cpp");
    const release = contexts.find((context) => context.label.includes("NDEBUG"));
    const switched = await s.switchContext("user.cpp", "user.cpp", {
        commandHash: release?.commandHash ?? "",
    });
    expect(switched.success).toBe(true);
    expect(await s.errors("user.cpp")).toEqual([]);
    await t.show("release", s.hover(use));
});
