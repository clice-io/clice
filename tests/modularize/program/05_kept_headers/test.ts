import { test } from "@clice/tools/testing";

// kit stays headers: the alias kt and the static kit_local reach core's
// text.h only through alpha.h, which the wrapping empties. core and tool
// keep their primary interfaces side by side in app/, so their prelude
// partitions need two names.
test(async (t) => {
    await t.cli("index");
    await t.cli("modularize --scope app/** --scope third/** --partition partition.json --out wrap");
});
