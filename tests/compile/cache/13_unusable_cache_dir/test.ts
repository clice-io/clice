import { test } from "@clice/tools/testing";

// The cache directory cannot be created (a file sits where it goes): no
// preamble or module is precompiled. The server tells the user, naming the
// directory, and the import says why its module did not build.
test(async (t) => {
    t.disk.write(".clice", "not a directory\n");
    const s = await t.start();
    await t.show("diagnostics", s.diagnostics("main.cpp"));
    await t.show("guidance", await s.guidance());
});
