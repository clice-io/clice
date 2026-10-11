import { test } from "@clice/tools/testing";

test(async (t) => {
    const s = await t.start();
    await t.show("src/main.cpp", s.errors("src/main.cpp"));
    await t.show("other.cpp", s.errors("other.cpp"));
});
