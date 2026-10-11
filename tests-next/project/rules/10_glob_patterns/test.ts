import { test } from "@clice/tools/testing";

test(async (t) => {
    const s = await t.start();
    for (const file of [
        "tests/unit/math_test.cpp",
        "src/core/alpha_test.cpp",
        "src/core/alpha.cpp",
        "src/core/zeta.cpp",
        "lib/util.cpp",
    ]) {
        await t.show(file, s.errors(file));
    }
});
