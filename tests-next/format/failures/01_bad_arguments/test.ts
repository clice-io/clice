import { test } from "@clice/tools/testing";

// Arguments the run cannot take fail it with exit 2 before anything is
// rewritten.

test("a missing file argument", async (t) => {
    await t.cli("format missing.cpp");
});

test("a non-source file argument", async (t) => {
    await t.cli("format notes.md");
});

test("an invalid log level", async (t) => {
    await t.cli("format --log-level loud");
});

test("an invalid job count", async (t) => {
    await t.cli("format --jobs many");
});

test("a workspace that does not exist", async (t) => {
    await t.cli("format --check --workspace missing");
});

test("an unknown configuration", async (t) => {
    await t.cli("format --configuration nope");
});
