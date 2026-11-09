import { expect } from "vitest";
import { generate, test } from "@clice/tools/testing";

// Twenty layers of modules, each importing both modules below it, over a
// base that fails to build: 2^20 import paths reach the base. Its failure
// is taken as it stands, not tried again along each path, so the server
// answers at once and builds the base once.
test(async (t) => {
    t.disk.write(generate.moduleLayers(20));
    const s = await t.start();
    await t.show("main", s.errors("main.cpp"));
    await s.sync();
    expect((await s.counts()).files["base.cppm"]?.pcm).toBe(1);
});
