import { generate, test } from "@clice/tools/testing";

test(async (t) => {
    t.disk.write(generate.units(12));
    await t.cli("index");
});
