import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index");
    await t.cli("query --method readSymbol --symbol area");
    await t.cli("query --method definition --symbol surface");
    await t.cli("query --method readSymbol --symbol Square::draw");
    await t.cli("query --method definition --symbol Shape::sides");
});
