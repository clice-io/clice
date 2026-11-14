import { answers } from "@clice/tools/testing";

answers({ paths: { server: "clice inspect does not rename" }, indexing: true }, async (a) => {
    const rename = (marker: string, to: string) =>
        a.rename(a.marker(marker), to, { compile: true });
    await a.show("parameter_to_local", rename("parameter", "result"));
    await a.show("local_to_parameter", rename("local", "factor"));
    await a.show("parameter_to_parameter", rename("second", "factor"));
    // Onto another function with the same parameters, a redefinition: clice does not
    // refuse this rename, it only warns of the clash.
    await a.show("same_signature", rename("compute", "twin"));
    await a.show("label_to_label", rename("again", "done"));
});
