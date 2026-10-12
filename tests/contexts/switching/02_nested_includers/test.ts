import { expect } from "vitest";
import { answers, test } from "@clice/tools/testing";
import { protocolKeys, type SwitchContextResult } from "@clice/tools/protocol";

test("a switch is current", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    s.open("utils.h");
    await t.show("automatic", s.currentContext("utils.h"));
    const switched = await s.switchContext("utils.h", "main.cpp");
    expect(switched.success).toBe(true);
    // The reply's C++ and TS shapes are written by hand on both sides: a
    // field one of them gains shows up here.
    expect(Object.keys(switched).sort()).toEqual(
        [...protocolKeys<SwitchContextResult>()(["stale", "success"])].sort(),
    );
    await t.show("switched", s.currentContext("utils.h"));
    // The pull waits for the switched compile, which the hover then reads.
    await t.show("diagnostics", s.diagnostics("utils.h"));
    await t.show("hover", s.hover(t.at("utils.h", "inline int |calc")));
});

test("a nested header switches", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    s.open("inner.h");
    expect((await s.switchContext("inner.h", "main.cpp")).success).toBe(true);
    await t.show("diagnostics", s.diagnostics("inner.h"));
    await t.show("hover", s.hover(t.at("inner.h", "inline Point |inner_origin")));
});

// A switch and its reset keep the document open: the server compiles the
// same text again and publishes for it.
test("a switch publishes again", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    s.open("utils.h");
    await t.show("opened", s.pushes("utils.h"));
    expect((await s.switchContext("utils.h", "main.cpp")).success).toBe(true);
    await t.show("switched", s.pushes("utils.h"));
    expect((await s.resetContext("utils.h")).success).toBe(true);
    await t.show("reset", s.pushes("utils.h"));
    await t.show("current", s.currentContext("utils.h"));
});

answers(
    "names in the includer's context",
    {
        paths: {
            server: "clice inspect compiles a header alone, without the declarations its includer makes before including it",
        },
    },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, a.highlights(m));
        }
    },
);
