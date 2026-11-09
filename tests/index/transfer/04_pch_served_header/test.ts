import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The target's units read shapes.h through its precompiled header, whose
// build indexes it: a unit's index holds no hash of shapes.h. One worker
// takes the units in turn, and the last one starts after an earlier one
// stored twice<Circle>'s resolution, so it skips the instantiation. Its
// merge checks the skipped key against shapes.h as the precompiled header
// read it; a merge that cannot check it indexes the unit a second time,
// sending everything.
test("each unit is indexed once", async (t) => {
    const s = await t.start({
        config: {
            project: {
                stateless_worker_count: 1,
                min_stateless_worker_count: 1,
                max_stateless_worker_count: 1,
            },
        },
    });
    await s.indexed();
    const { files } = await s.counts();
    const units = ["src/a.cpp", "src/b.cpp", "src/c.cpp"];
    expect(units.map((unit) => files[unit]?.index)).toEqual([1, 1, 1]);
});
