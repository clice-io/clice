import { answers } from "@clice/tools/testing";

answers({ paths: { server: "clice inspect does not rename" }, indexing: true }, async (a) => {
    await a.show("instantiated", a.rename(a.marker("circle"), "perimeter"));
    await a.show("shared", a.rename(a.marker("square"), "side"));
    await a.show("uninstantiated", a.rename(a.marker("box"), "volume"));
    // From the template, the name is what its instantiations resolve it to.
    await a.show("from_instantiated_call", a.rename(a.marker("measure_call"), "perimeter"));
    await a.show("from_uninstantiated_call", a.prepareRename(a.marker("size_of_call")));
    // A static member named through a dependent scope, confirmed by load<int>.
    await a.show("instantiated_scope", a.rename(a.marker("crate"), "volume"));
    await a.show("from_instantiated_scope", a.rename(a.marker("load_call"), "volume"));
    // Overrides are one rename: instantiations naming both agree.
    await a.show("overrides", a.rename(a.marker("count_call"), "edges"));
    // The one function a dependent call finds where it is written is its own.
    await a.show("lone_candidate", a.rename(a.marker("scale"), "stretch"));
});
