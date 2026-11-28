import * as path from "node:path";

import { test, type TestContext } from "@clice/tools/testing";

/// The user's file, under the directory of the copy XDG_CONFIG_HOME names.
function env(t: TestContext): Record<string, string> {
    return { XDG_CONFIG_HOME: path.join(t.root, "user") };
}

// A problem of the user's file is shown on it, at the index its rule has
// there, and the project, which reads it, serves nothing.
test("server", async (t) => {
    const s = await t.start({ env: env(t) });
    await t.show("clice.toml", s.pushes("user/clice/clice.toml"));
    s.open("main.cpp");
    await t.show("main.cpp", s.documentSymbols("main.cpp"));
});

// A command names the user's file where it is.
test("command line", { errors: ["holds a backslash"] }, async (t) => {
    await t.cli("lint", { env: env(t) });
});
