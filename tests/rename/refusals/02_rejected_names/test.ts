import { answers } from "@clice/tools/testing";

answers({ paths: { server: "clice inspect does not rename" }, indexing: true }, async (a) => {
    const at = a.marker("compute");
    for (const name of [
        "1abc",
        "class",
        "_Bool",
        "char8_t",
        "co_await",
        "requires",
        "Point",
        "TWICE",
        "other",
        "_Compute",
        "_lower",
        "two__parts",
        "café",
    ]) {
        await a.show(name, a.rename(at, name));
    }
    // A sibling class's members are out of reach.
    await a.show("sibling_member", a.rename(a.marker("sibling"), "extra"));
});
