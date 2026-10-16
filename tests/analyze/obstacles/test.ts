import { test } from "@clice/tools/testing";

// third/ is a third-party library: out of the scope.
test(async (t) => {
    await t.cli("index");
    await t.cli(
        "analyze modules obstacles --scope core/** --scope util/** --scope feature/** --scope app/**",
    );
});
