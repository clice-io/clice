import { modularize, test } from "@clice/tools/testing";

// kit stays headers: the alias kt, the using-declaration kit::Gizmo and the
// static kit_local reach core's text.h only through alpha.h, which the
// wrapping empties. text.h also redeclares ext's Status, outside the scope,
// over alpha's Code. core and tool keep their primary interfaces side by
// side in app/, so their prelude partitions need two names.
test(async (t) => {
    await t.cli("index");
    const run = await t.cli(
        "modularize --scope app/** --scope third/** --partition partition.json --out wrap",
    );
    // The program as the plan builds it compiles cleanly.
    await modularize.compiles(t, run, "wrap");
});
