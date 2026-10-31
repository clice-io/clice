import { test } from "@clice/tools/testing";

// A rename in one folder's project leaves the other folder's open buffer
// alone: that project keeps its uses, and the rename is made.
test(async (t) => {
    const s = await t.start({ folders: ["app", "lib"] });
    await s.indexed();
    s.open("app/main.cpp");
    await s.diagnostics("app/main.cpp");
    await t.show("renamed", s.rename(t.at("lib/lib.cpp", "int |compute(int x)"), "evaluate"));
});
