import { test } from "@clice/tools/testing";

// The worker linting a.cpp is killed from outside, naming no request: the
// pass waits for the worker to come back and retries a.cpp there, and the
// report is whole.
test(async (t) => {
    await t.cli("lint --jobs 1", { kill: { request: "compileUnit", file: "a.cpp" } });
});
