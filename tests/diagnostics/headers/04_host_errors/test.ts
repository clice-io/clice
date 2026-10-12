import { test } from "@clice/tools/testing";

// frag.h compiles in host.cpp's context: the host's errors before and after
// the include are the host's, only the header's own error shows.
test(async (t) => {
    const s = await t.start();
    await t.show("frag.h", s.diagnostics("frag.h"), { parts: ["related"] });
});
