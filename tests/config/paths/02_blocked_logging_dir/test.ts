import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A file stands where the log directory would go: the command says so once
// and runs without a log, its workers too, whose lines stay out of its
// stderr.
test("index", { errors: ["Failed to create log directory"] }, async (t) => {
    await t.cli("index");
});

// An editor's server the same: its workers relay nothing to its stderr.
test("server", async (t) => {
    const s = await t.start();
    const client = s.client;
    await t.show("errors", s.errors("main.cpp"));
    await s.stop();
    const relayed = client
        .drainedStderr()
        .toString("utf8")
        .split(/\r*\n/)
        .filter((line) => /\[S[LF]-\d+\]/.test(line));
    expect(relayed).toEqual([]);
});
