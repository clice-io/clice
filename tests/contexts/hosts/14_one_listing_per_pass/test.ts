import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The pages after the first come from the listing the first page computed.
// Between them s10.cpp's index arrives: it enters the unguarded header
// twice, which lists it twice, yet the epoch stays, and the second page
// still answers from the listing of eleven. The next pass lists twelve.
// s10.cpp's index is held before the units join the database, so no run
// merges it first.
test(async (t) => {
    t.disk.database({});
    const s = await t.start();
    const held = await s.park("index", "s10.cpp", { at: "reply" });
    t.disk.database();
    expect(await s.poll("cdb")).toBe(1);
    await held.reached();
    const first = await s.contexts("common.h");
    await held.release();
    await s.sync();
    const second = s.contexts("common.h", { offset: 10 });
    await t.show("second page", second);
    expect([first.total, (await second).total, (await second).epoch]).toEqual([
        11,
        11,
        first.epoch,
    ]);
    expect((await s.contexts("common.h")).total).toBe(12);
    await t.show("next pass, second page", s.contexts("common.h", { offset: 10 }));
});
