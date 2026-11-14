import { modularize, test } from "@clice/tools/testing";

// count.cpp reaches bits.h only through set.h, and bits.h includes kit's
// roaring.h, which stays a header: count.cpp imports bits.h's partition
// itself, since clang finds what an implementation partition's global
// module fragment declares only from a unit importing it directly.
test(async (t) => {
    await t.cli("index");
    const run = await t.cli(
        "modularize --scope app/** --scope third/** --partition partition.json --out wrap",
    );
    // The program as the plan builds it compiles cleanly.
    await modularize.compiles(t, run, "wrap");
});
