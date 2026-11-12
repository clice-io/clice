import { answers } from "@clice/tools/testing";

// Each unit's definitions of a name are one macro with those of the headers
// it shares with another unit, and with the definitions one macro body names
// in each; a unit defining nothing tests the name for its own.
answers({ paths: { server: "clice inspect does not rename" }, indexing: true }, async (a) => {
    await a.show("shared_header", a.rename(a.marker("limit"), "BOUND"));
    await a.show("unrelated", a.rename(a.marker("check"), "VERIFY"));
    await a.show("linked_by_body", a.rename(a.marker("debug_type"), "LOG_TYPE"));
});
