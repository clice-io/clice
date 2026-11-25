import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A save in place truncates the file before it writes the bytes: a unit
// indexed in between reads an empty file, and once the write is done the
// disk holds the bytes the server last saw, so no look reports a change.
// The unit is indexed again, or `beta` is never found.
test(async (t) => {
    const s = await t.start();
    await s.indexed();
    const read = await s.park("compileUnit", "main.cpp", { at: "worker" });
    const merged = await s.park("index", "main.cpp", { at: "reply" });
    t.disk.edit("main.cpp", { replace: "alpha", with: "beta" });
    expect(await s.poll("workspace")).toBe(1);
    await read.reached();
    const saved = t.disk.read("main.cpp");
    t.disk.write("main.cpp", "");
    await read.release();
    await merged.reached();
    t.disk.write("main.cpp", saved);
    await merged.release();
    await s.indexed();
    await t.show("symbols", s.workspaceSymbols("beta"));
});
