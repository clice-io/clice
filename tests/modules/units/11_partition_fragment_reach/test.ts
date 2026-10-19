import { test } from "@clice/tools/testing";

// The implementation unit imports :set, which imports :bits, so it imports
// :bits too ([module.import]/7) and the definition :bits's global module
// fragment gives Bitmap is reachable from it ([module.reach]/1). Clang only
// takes a module interface unit imported that way as reachable.
test(async (t) => {
    const s = await t.start();
    await t.show("impl.cpp", s.errors("impl.cpp"));
    await t.show("set.cppm", s.errors("set.cppm"));
});
