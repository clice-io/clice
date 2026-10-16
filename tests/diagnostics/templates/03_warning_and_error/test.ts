import { test } from "@clice/tools/testing";

// One instantiation raises a warning and an error. Raised in instantiations
// of their own, both show on the request, the warning not hiding the error;
// raised in the same one, only the first comes with clang's chain of
// instantiations back to the request.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["related"] });
    await t.show("flat.cpp", s.diagnostics("flat.cpp"), { parts: ["related"] });
});
