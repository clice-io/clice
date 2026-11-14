import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// An editor restarting its server may start the new one before the old one
// has saved and gone: the new one waits for the old one to let go of the
// index, then serves the project as its writer.
test("a restart takes the index over", async (t) => {
    const first = await t.start();
    await first.indexed();
    const second = await t.start({ env: { CLICE_TEST_WRITER_WAIT_MS: "60000" } });
    await first.stop();
    expect(await second.errors("main.cpp")).toEqual([]);
    await second.indexed();
    await t.cli("index");
});
