import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// An editor restarting its server may start the new one before the old one
// has saved and gone: the new one compiles at once, and becomes the index's
// writer once the old one lets the lock go.
test("a restart takes the index over", async (t) => {
    const first = await t.start();
    await first.indexed();
    const second = await t.start({ env: { CLICE_TEST_WRITER_WAIT_MS: "60000" } });
    expect(await second.errors("main.cpp")).toEqual([]);
    await first.stop();
    await second.indexed();
    await t.cli("index");
});

// A second window on the project waits for the lock in vain: it answers
// from the index the first one saved, and saves nothing of its own.
test("a second window reads the index", async (t) => {
    await t.cli("index");
    const first = await t.start();
    await first.indexed();
    const second = await t.start({ env: { CLICE_TEST_WRITER_WAIT_MS: "100" } });
    expect(await second.errors("main.cpp")).toEqual([]);
    await second.indexed();
    await t.show("guidance", await second.guidance());
    await t.show("symbols", second.workspaceSymbols("compute"));
});
