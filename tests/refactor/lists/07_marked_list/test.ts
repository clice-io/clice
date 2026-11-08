import { test } from "@clice/tools/testing";

// The list is saved as UTF-8 with a byte order mark, as Windows tools write
// it: the mark is no part of the first locator.
test(async (t) => {
    t.disk.write("renames.txt", `\ufeff${t.disk.read("renames.txt")}`);
    await t.cli("index");
    await t.cli("refactor rename --spec renames.txt");
});
