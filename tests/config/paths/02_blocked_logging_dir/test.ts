import { test } from "@clice/tools/testing";

// A file stands where the log directory would go: the command says so once
// and runs without a log, its workers too, whose lines stay out of its
// stderr.
test({ errors: ["Failed to create log directory"] }, async (t) => {
    await t.cli("index");
});
