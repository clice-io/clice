import { test } from "@clice/tools/testing";

// answer is core/api.h's, which declares it: app/access.h only befriends
// it. same is declared only as Token's friend, which argument dependent
// lookup finds through Token: core/token.h provides it, not the source
// defining it.
test(async (t) => {
    await t.cli("index");
    await t.cli("analyze modules obstacles --scope core/** --scope app/**");
    await t.cli("analyze modules edge app core");
});
