import * as path from "node:path";

import { test } from "@clice/tools/testing";

// The user's file is read from XDG_CONFIG_HOME, here a directory of the
// copy: its rule adds up with the project's, which comes first.
test(async (t) => {
    const env = { XDG_CONFIG_HOME: path.join(t.root, "user") };
    const s = await t.start({ env });
    await t.show("main.cpp", s.diagnostics("main.cpp"));
    await t.cli("inspect config", { env });
});
