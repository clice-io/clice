import { answers } from "@clice/tools/testing";

answers({ paths: { server: "clice inspect does not rename" }, indexing: true }, async (a) => {
    const rename = (marker: string, to: string) =>
        a.rename(a.marker(marker), to, { compile: true });
    await a.show("namespace", rename("namespace", "numeric"));
    await a.show("enumerator", rename("enumerator", "crimson"));
    await a.show("enum", rename("enum", "Feeling"));
    await a.show("field", rename("field", "column"));
    await a.show("alias", rename("alias", "Length"));
    await a.show("concept", rename("concept", "Tiny"));
    await a.show("label", rename("label", "retry"));
    await a.show("dollar", rename("dollar", "total"));
});
