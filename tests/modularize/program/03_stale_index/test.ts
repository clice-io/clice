import { test } from "@clice/tools/testing";

// A file written since `clice index` (as `clice refactor rename` leaves the
// files it edits) makes the index describe text that is gone: modularize
// refuses it.
test(async (t) => {
    await t.cli("index");
    t.disk.edit("app/core/text.h", { replace: "int size() const;", with: "int length() const;" });
    await t.cli("modularize --scope app/** --partition partition.json --out wrap");
});
