import { modularize, test } from "@clice/tools/testing";

// Static as <emmintrin.h>'s intrinsics, which simdjson and CRoaring both
// call from their inline code. alpha reaches fake_simd through fake_simd2
// alone, fake_deep through the C library's inline fake_wrap, fake_seed
// through fake_value's initializer, and fake_noise only from a helper
// nothing calls.
const ARGS = "--scope app/** --scope third/** --partition partition.json --std stdmod";

test(async (t) => {
    await t.cli("index");
    await t.cli(`analyze modules interface alpha ${ARGS}`);
    await t.cli(`analyze modules interface beta ${ARGS}`);
    await t.cli(`analyze modules interface ${ARGS}`);
    const run = await t.cli(`modularize ${ARGS} --out wrap`);
    // The program as the plan builds it compiles cleanly.
    await modularize.compiles(t, run, "wrap");
});
