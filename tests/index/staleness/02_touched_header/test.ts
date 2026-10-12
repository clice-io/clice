import { test } from "@clice/tools/testing";

// A header whose time changes but whose bytes do not (a checkout, a build
// tool rewriting it) leaves its includers' rows alone: the second run
// re-hashes it, finds the content unchanged, and indexes nothing.
test(async (t) => {
    await t.cli("index");
    t.disk.touch("header.h", new Date(t.disk.mtime("header.h").getTime() + 5_000));
    await t.cli("index");
});
