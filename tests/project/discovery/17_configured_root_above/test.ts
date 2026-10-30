import { answers, test } from "@clice/tools/testing";

// The nearest directory holding a configuration file or a database is the
// project's root, for inspect as for the server: sub/, nearer than the
// clice.toml above it.
answers("from the file", async (a) => {
    await a.show("value", a.hover(a.marker("value")));
});

// --workspace names the root as given: the root's project, which has no
// database for a.cpp, so VALUE is no macro there.
test("--workspace", async (t) => {
    await t.cli("inspect semantic-tokens sub/a.cpp --workspace .");
});
