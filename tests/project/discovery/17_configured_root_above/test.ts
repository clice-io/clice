import { answers } from "@clice/tools/testing";

// The nearest directory holding a configuration file or a database is the
// project's root, for inspect as for the server: sub/, nearer than the
// clice.toml above it.
answers(async (a) => {
    await a.show("value", a.hover(a.marker("value")));
});
