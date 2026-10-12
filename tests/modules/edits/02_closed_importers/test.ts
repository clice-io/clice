import { test } from "@clice/tools/testing";

// The added overload is what closed.cpp's call prefers: only a reindex of
// the importer against the new interface points the call at it. The next
// server's startup finds the importer fresh and never runs it; only the
// index's record of what it imported leads the save to it.
test(async (t) => {
    const s = await t.start();
    await s.indexed();
    const first = t.at("m.cppm", "int |pick(int v)");
    await t.show("before", s.references(first));
    await s.restart();
    s.open("m.cppm");
    s.edit("m.cppm", {
        after: "export int pick(int v) { return v; }\n",
        insert: "export int pick(long v) { return 2; }\n",
    });
    s.save("m.cppm");
    await s.indexed();
    await t.show("int overload", s.references(first));
    await t.show("long overload", s.references(t.at("m.cppm", "int |pick(long v)")));
});
