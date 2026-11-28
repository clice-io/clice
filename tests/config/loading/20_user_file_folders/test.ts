import * as path from "node:path";

import { test } from "@clice/tools/testing";

// Both folders read the user's file: the key it misspells is listed once,
// and the configuration only a/ declares is an error of b/'s alone, listed
// all the same.
test(async (t) => {
    const s = await t.start({
        folders: ["a", "b"],
        env: { XDG_CONFIG_HOME: path.join(t.root, "user") },
    });
    await t.show("clice.toml", s.pushes("user/clice/clice.toml"));
});
