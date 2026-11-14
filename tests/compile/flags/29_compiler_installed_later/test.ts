import fs from "node:fs";
import path from "node:path";
import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The build's compiler is missing when the file opens and installed while
// it is open: past the cooldown the server asks it again, and the file
// compiles and is indexed again under its answer without an edit.
test(async (t) => {
    const s = await t.start({
        config: { tracker: { workspace_poll_seconds: 1 } },
        env: { CLICE_TEST_QUERY_RETRY_MS: "0" },
    });
    await t.show("missing", s.diagnostics("main.cpp"));
    await s.indexed();
    const indexed = (await s.counts()).files["main.cpp"]?.index ?? 0;

    const name = process.platform === "win32" ? "clang++.exe" : "clang++";
    const compiler = (process.env.PATH ?? "")
        .split(path.delimiter)
        .map((dir) => path.join(dir, name))
        .find((file) => fs.existsSync(file));
    if (!compiler) {
        throw new Error(`no ${name} on PATH`);
    }
    const answered = await s.watch("compilersAnswered");
    t.disk.symlink(`bin/${name}`, compiler);
    await answered.reached();
    await t.show("installed", s.diagnostics("main.cpp"));
    await s.indexed();
    expect((await s.counts()).files["main.cpp"]?.index).toBe(indexed + 1);
});
