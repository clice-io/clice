import { test } from "@clice/tools/testing";

// An enumerator of an unnamed enum in a header is one symbol in every
// source including it.
test(async (t) => {
    await t.cli("index");
    await t.cli("query --method references --symbol a.cpp:2:18 --include-declaration");
});
