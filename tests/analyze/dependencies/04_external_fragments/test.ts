import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index --workspace project");
    await t.cli("analyze modules edge app core --workspace project");
});
