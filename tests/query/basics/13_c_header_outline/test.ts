import { test } from "@clice/tools/testing";

// A header only C sources include reads as C: `R"(` opens no raw string
// there, so the `#define` after it is in the outline.
test(async (t) => {
    await t.cli("index");
    await t.cli("query document-symbols api.h");
});
