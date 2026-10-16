import { test } from "@clice/tools/testing";

// A rename of a kind or modifier clice does not have is pointed out once,
// when the names are announced.
test(async (t) => {
    const s = await t.start({
        config: { semantic_tokens: { kinds: { feild: "property" }, modifiers: { scope: "" } } },
    });
    await t.show("guidance", await s.guidance());
});
