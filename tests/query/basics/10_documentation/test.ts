import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index");
    await t.cli("query read-symbol area");
    await t.cli("query definition surface");
    await t.cli("query read-symbol Square::draw");
    await t.cli("query definition Shape::sides");
});
