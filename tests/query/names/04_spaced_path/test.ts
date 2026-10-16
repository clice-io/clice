import { test } from "@clice/tools/testing";

// The locator of a file-local symbol names its file; a path with a space
// is quoted, and the locator reads back.
test(async (t) => {
    await t.cli("index");
    const outline = await t.cli(["query", "document-symbols", "my dir/util.h"]);
    const [listed] = (outline.json as { result: { symbols: { symbol: string }[] } }).result.symbols;
    if (listed === undefined) {
        throw new Error("the outline of my dir/util.h lists nothing");
    }
    await t.cli(["query", "definition", listed.symbol]);
});
