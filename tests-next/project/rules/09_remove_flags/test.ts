import { test } from "@clice/tools/testing";

test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.errors("main.cpp"));
    await t.show("other.cpp", s.errors("other.cpp"));
});
