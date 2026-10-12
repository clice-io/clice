import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// core's text.h forward-declares sink.h's Sink and declares hook(), which
// tool's hook.cpp defines, and includes cfg's config.h, which stays a
// header; detail.h is core's alone, all.h an umbrella over it, sink.h
// guarded. tool.h forward-declares core's Sink and Box, and tool.cpp
// expands core's CORE_TWICE. app/run.cpp, outside both, uses tool.
const ARGS = "--scope app/**,third/** --std stdmod --out wrap";

// One worker: which header provides fake_widen, declared both in
// <cwide.h> and in a fragment widen.h pastes, depends on the order the
// units are indexed in (analyze/dependencies/06_unit_order).
test("rewrites program modules", async (t) => {
    await t.cli("index --workers 1");
    await t.cli(`modularize ${ARGS} --partition partitions/program.json`);
});

// Nothing is written when a partition would be rejected.
test("rejected partitions", async (t) => {
    await t.cli("index --workers 1");
    for (const partition of ["clash", "same_primary", "outside", "dash", "keyword", "core_only"]) {
        await t.cli(`modularize ${ARGS} --partition partitions/${partition}.json`);
    }
    t.disk.write("app/core/sink.cppm", "");
    await t.cli(`modularize ${ARGS} --partition partitions/both.json`);
    expect(t.disk.read("app/core/sink.cppm")).toBe("");
});
