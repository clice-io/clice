import { test } from "@clice/tools/testing";

// One instantiation raises a warning and an error. Raised in instantiations
// of their own, both show on the request, the warning not hiding the error;
// raised in the same one, only the first comes with clang's chain of
// instantiations back to the request. The templates are in headers whose
// function bodies the editor skips: the background index reports them.
test(async (t) => {
    const s = await t.start();
    await s.indexed();
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["related"] });
    await t.show("flat.cpp", s.diagnostics("flat.cpp"), { parts: ["related"] });
});
