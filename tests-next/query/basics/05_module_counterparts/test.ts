import { test } from "@clice/tools/testing";

// A module implementation unit's counterpart is its interface: it declares
// what the unit defines, and it is the interface of the unit's module.
test(async (t) => {
    await t.cli("index");
    await t.cli("query --method counterparts --path store_impl.cpp");
    await t.cli("query --method counterparts --path store.cppm");
});
