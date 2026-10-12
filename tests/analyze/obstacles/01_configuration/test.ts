import { test } from "@clice/tools/testing";

// third/ is a third-party library: out of the scope. The overview counts
// only the header whose readings declare different entities.
test(async (t) => {
    await t.cli("index");
    await t.cli("analyze modules --view obstacles --scope core/**,app/**");
    await t.cli("analyze modules --scope core/**,app/**");
});
