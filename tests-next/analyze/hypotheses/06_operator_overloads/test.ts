import { test } from "@clice/tools/testing";

// A function's overloads in one scope move together.
test(async (t) => {
    await t.cli("index");
    await t.cli("analyze modules --view edge --from app --to core");
    await t.cli(
        'analyze modules --move-entity "operator+=app/ops.h" --view edge --from app --to core',
    );
});
