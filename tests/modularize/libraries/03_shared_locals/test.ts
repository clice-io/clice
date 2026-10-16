import { test } from "@clice/tools/testing";

// Static as <emmintrin.h>'s intrinsics, which simdjson and CRoaring both
// call from their inline code. alpha reaches fake_simd through fake_simd2
// alone, fake_deep through the C library's inline fake_wrap, fake_seed
// through fake_value's initializer, and fake_noise only from a helper
// nothing calls.
const ARGS = "--scope app/** --scope third/** --partition partition.json --std stdmod";

// One worker: which header provides fake_widen, declared both in
// <cwide.h> and in a fragment widen.h pastes, depends on the order the
// units are indexed in (analyze/dependencies/06_unit_order).
test(async (t) => {
    await t.cli("index --jobs 1");
    await t.cli(`analyze modules interface alpha ${ARGS}`);
    await t.cli(`analyze modules interface beta ${ARGS}`);
    await t.cli(`analyze modules interface ${ARGS}`);
    await t.cli(`modularize ${ARGS} --out wrap`);
});
