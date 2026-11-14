import { test } from "@clice/tools/testing";

// app/spec.h names lib's Traits only in the specialization it declares.
test(async (t) => {
    await t.cli("index");
    await t.cli("analyze modules edge app lib");
});
