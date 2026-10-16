import { test } from "@clice/tools/testing";

// A function's overloads in one scope move together.
test(async (t) => {
    await t.cli("index");
    await t.cli("analyze modules edge app core");
    await t.cli('analyze modules edge app core --move-entity "operator+=app/ops.h"');
});
