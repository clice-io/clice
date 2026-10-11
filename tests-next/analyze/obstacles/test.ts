import { test } from "@clice/tools/testing";

// third/ is a third-party library: out of the scope.
test(async (t) => {
    await t.cli("index");
    await t.cli("analyze modules --view obstacles --scope core/**,util/**,feature/**,app/**");
});
