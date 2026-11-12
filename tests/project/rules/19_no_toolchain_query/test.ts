import { test } from "@clice/tools/testing";

// The compiler of both commands does not exist. clice asks it how it
// compiles asked.cpp and says it could not; quiet.cpp's rule says not to
// ask, and clang's own driver reads its command with nothing to say.
test(async (t) => {
    const s = await t.start();
    await t.show("asked.cpp", s.diagnostics("asked.cpp"));
    await t.show("quiet.cpp", s.diagnostics("quiet.cpp"));
});
