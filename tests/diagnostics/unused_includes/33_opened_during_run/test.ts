import { test } from "@clice/tools/testing";

// The file opens while a run of it, closed, computes no unused includes:
// the run asks for the next, which does.
test(async (t) => {
    // main.cpp joins the database once its index run is parked: the pass
    // indexing it starts after the park.
    t.disk.database({ "other.cpp": [] });
    const s = await t.start();
    await s.indexed();
    const park = await s.park("compileUnit", "main.cpp", { at: "worker" });
    t.disk.database();
    await s.poll("cdb");
    await park.reached();
    s.open("main.cpp", { pull: false });
    await park.release();
    await s.indexed();
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["tags"] });
});
