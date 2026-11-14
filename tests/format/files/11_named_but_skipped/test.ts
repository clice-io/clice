import { test } from "@clice/tools/testing";

// A named file the run will not format, one outside the workspace or one a
// rule keeps out, fails the run with exit 2, saying why: a check that
// skipped it would pass it unchecked.
test(async (t) => {
    await t.cli("format --check ../outside/far.cpp", { cwd: "ws" });
    await t.cli("format --check kept.cpp", { cwd: "ws" });
});
