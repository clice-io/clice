import { test } from "@clice/tools/testing";

// Pins a unit that crashes its worker: a retry would crash as well, so the
// run gives up on it at once, still checks the rest, and says the report is
// partial.
test(async (t) => {
    await t.cli("lint --workers 1", { crash: { request: "tuRun", file: "poison.cpp" } });
});
