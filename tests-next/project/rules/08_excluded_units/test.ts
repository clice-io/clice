import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("index skips excluded units", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.indexed();
    await t.show("indexed", {
        main_sym: s.workspaceSymbols("main_sym"),
        tp_sym: s.workspaceSymbols("tp_sym"),
    });
    // A command change queues the unit again through the reload: the
    // exclusion holds there as well.
    t.disk.database({ "main.cpp": ["-DCHANGED"], "third_party/lib.cpp": ["-DCHANGED"] });
    expect(await s.poll("cdb")).toBe(1);
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.indexed();
    await t.show("reloaded", {
        main_sym: s.workspaceSymbols("main_sym"),
        tp_sym: s.workspaceSymbols("tp_sym"),
    });
});

test("excluded unit escalates when read", async (t) => {
    // No shard will ever serve an excluded unit: the read-only session
    // compiles it at once when it is opened.
    const s = await t.start({ config: { project: { readonly: "auto" } } });
    s.open("third_party/lib.cpp", { pull: false });
    await t.show("symbols", s.documentSymbols("third_party/lib.cpp"));
    // Nothing pulled: the compile the escalation started pushed these.
    await t.show("pushed", s.pushes("third_party/lib.cpp"));
});
