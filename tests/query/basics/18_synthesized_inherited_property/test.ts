import { test } from "@clice/tools/testing";

// A subclass synthesizes an inherited property. The accessor stub belongs to
// Thing's implementation, while the property belongs to Base.
test(async (t) => {
    await t.cli("index");
    await t.cli("query references value --include-declaration");
});
