import { test } from "@clice/tools/testing";

// A file written since `clice index` makes the index describe text that is
// gone: analyze refuses it.
test(async (t) => {
    await t.cli("index");
    t.disk.edit("core/str.h", { before: "#pragma once", insert: "// edited\n" });
    await t.cli("analyze modules");
});
