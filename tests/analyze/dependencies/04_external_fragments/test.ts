import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index --workspace project");
    await t.cli("analyze modules --workspace project --view edge --from app --to core");
});
