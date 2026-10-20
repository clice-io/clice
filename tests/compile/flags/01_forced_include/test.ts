import { expect } from "vitest";
import { answers, test } from "@clice/tools/testing";

// main.cpp and other.cpp compile with `-include force.h`; header.h uses
// what it defines and compiles after main.cpp's prefix.

answers("hover", async (a) => {
    await a.show("forced", a.hover(a.marker("forced")));
});

answers(
    "navigation",
    { paths: { server: "definition reads the server's preambles" } },
    async (a) => {
        await a.show("forced", a.definition(a.marker("forced")));
    },
);

test("edits pay no import scan", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await s.errors("header.h")).toEqual([]);
    s.edit("main.cpp", { replace: "return header();", with: "return header() + 1;" });
    expect(await s.errors("main.cpp")).toEqual([]);
    s.edit("header.h", { replace: "return FORCED;", with: "return FORCED + 1;" });
    expect(await s.errors("header.h")).toEqual([]);
    await s.indexed();
    await t.show("other", s.workspaceSymbols("other"));
    // The forced header is scanned like any include: no unit needed a
    // look for module imports of its own.
    expect((await s.stats()).importScans).toBe(0);
});

test("forced header edit", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    // The first index of the closed unit is done before the edits, which
    // would otherwise preempt it and fold it into theirs.
    await s.indexed();
    const original = t.disk.read("force.h");
    t.disk.write("force.h", "#define FORCED_RENAMED 1\n");
    await t.show("renamed", s.errors("main.cpp"));
    await s.sync();
    t.disk.write("force.h", original);
    s.save("force.h");
    expect(await s.errors("main.cpp")).toEqual([]);
    // Each change rebuilds the PCH the forced header is part of, and indexes
    // the closed unit again. The forced header, hosted by the units forcing
    // it in, is indexed on its own too: its new text of the first change
    // starts its index over, dropping what the closed unit's index put
    // there, and the unit indexes once more.
    const { failed } = await s.sync();
    const { files } = await s.counts();
    expect({ pch: files["main.cpp"]?.pch, index: files["other.cpp"]?.index }).toEqual({
        pch: 3,
        index: 4,
    });
    await t.show("failed", failed);
});
