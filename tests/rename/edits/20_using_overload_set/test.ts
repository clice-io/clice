import { answers } from "@clice/tools/testing";

answers({ paths: { server: "clice inspect does not rename" }, indexing: true }, async (a) => {
    // One overload cannot leave the using-declaration naming the other.
    await a.show("partial", a.rename(a.marker("foo"), "bar"));
});
