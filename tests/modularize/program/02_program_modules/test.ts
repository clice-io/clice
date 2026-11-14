import { expect } from "vitest";
import { modularize, test } from "@clice/tools/testing";

// core's text.h forward-declares sink.h's Sink and declares hook(), which
// tool's hook.cpp defines, and includes cfg's config.h, which stays a
// header; detail.h is core's alone, all.h an umbrella over it, sink.h
// guarded. tool.h forward-declares core's Sink and Box, and tool.cpp
// expands core's CORE_TWICE. app/run.cpp, outside both, uses tool.
const ARGS = "--scope app/** --scope third/** --std stdmod --out wrap";

test("rewrites program modules", async (t) => {
    await t.cli("index");
    const run = await t.cli(`modularize ${ARGS} --partition partitions/program.json`);
    // The program as the plan builds it compiles cleanly.
    await modularize.compiles(t, run, "wrap");
});

// Nothing is written when a partition would be rejected.
test("rejected partitions", async (t) => {
    await t.cli("index");
    for (const partition of ["clash", "same_primary", "outside", "dash", "keyword", "core_only"]) {
        await t.cli(`modularize ${ARGS} --partition partitions/${partition}.json`);
    }
    t.disk.write("app/core/sink.cppm", "");
    await t.cli(`modularize ${ARGS} --partition partitions/both.json`);
    expect(t.disk.read("app/core/sink.cppm")).toBe("");
});
