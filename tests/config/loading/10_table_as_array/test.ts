import { test } from "@clice/tools/testing";

// A table written as an array of tables: the error underlines the first
// header, not the whole array.
test(async (t) => {
    const s = await t.start();
    await t.show("clice.toml", s.pushes("clice.toml"));
});
