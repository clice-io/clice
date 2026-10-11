import { test } from "@clice/tools/testing";

// An importer's index reads the module's interface without entering its
// file; a touch of the interface reindexes nothing, a change reindexes the
// interface and its importer.
test(async (t) => {
    await t.cli("index");
    t.disk.touch("m.cppm", new Date(t.disk.mtime("m.cppm").getTime() + 5_000));
    await t.cli("index");
    t.disk.edit("m.cppm", { replace: "return 1;", with: "return 2;" });
    await t.cli("index");
});
