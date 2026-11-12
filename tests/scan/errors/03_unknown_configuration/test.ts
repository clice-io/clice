import { test } from "@clice/tools/testing";

test({ errors: ["names no configuration"] }, async (t) => {
    await t.cli("scan --configuration missing");
});
