import { test } from "@clice/tools/testing";

// a.cppm is rewritten while its build runs, and the server sees no change:
// a save rewrites a file in place, so the build may read it half written,
// and a save of the same text changes nothing it watches. The failure stands
// on text the disk no longer holds: the module builds again from the disk
// before main.cpp compiles against it.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    const park = await s.park("pcm", "a.cppm", { at: "reply" });
    const first = s.errors("main.cpp");
    await park.reached();
    t.disk.edit("a.cppm", { replace: "return broken_in_a;", with: "return 1;" });
    await park.release();
    await t.show("errors", first);
    await t.show("pushes", s.pushes("main.cpp"));
    const counts = (await s.counts()).files;
    await t.show("builds", { compiles: counts["main.cpp"]?.compile, pcms: counts["a.cppm"]?.pcm });
});
