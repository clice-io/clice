import { test } from "@clice/tools/testing";

// Pins units that do not compile: their errors are findings and the checks
// still report around them, as clang-tidy does; an error the command line
// causes has no place in a file; a source the database lists but the disk
// lacks fails to run, and the report says it is partial.
test("syntax errors", async (t) => {
    t.disk.database({ "syntax.cpp": [] });
    await t.cli("lint --workers 2");
});

test("missing include", async (t) => {
    t.disk.database({ "include.cpp": [] });
    await t.cli("lint --workers 2");
});

test("missing forced include", async (t) => {
    t.disk.database({ "healthy.cpp": ["-include", "missing.h"] });
    await t.cli("lint --workers 2");
});

test("missing source", async (t) => {
    t.disk.database({ "healthy.cpp": [], "ghost.cpp": [] });
    await t.cli("lint --workers 2");
});
