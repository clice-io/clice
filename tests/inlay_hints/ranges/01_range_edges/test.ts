import { answers } from "@clice/tools/testing";

// Each range asks the hints of its lines alone, the code around it running
// past its ends: a macro invocation begun above it, a block closed in it, a
// declaration below or above its attribute. A range ending where a hint
// stands leaves it out, one beginning there holds it.
answers({ config: { inlay_hints: { block_end: true, min_block_lines: 2 } } }, async (a) => {
    await a.show("main.cpp", a.inlayHints("main.cpp"));
    for (const m of a.markers()) {
        await a.show(m.name, a.inlayHints(m));
    }
});
