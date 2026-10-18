import { test } from "@clice/tools/testing";

// The database lists a fetched dependency's source beside the project's own.
test(async (t) => {
    await t.cli("format");
});
