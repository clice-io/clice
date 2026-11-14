import { test } from "@clice/tools/testing";

test("class members", async (t) => {
    await t.cli("index --jobs 2");
    await t.cli("refactor rename Base::size capacity --dry-run");
    await t.cli("refactor rename Derived::capacity size --dry-run");
    await t.cli("refactor rename Base::grow Base --dry-run");
    await t.cli("refactor rename Derived capacity --dry-run");
});

test("one scope", async (t) => {
    await t.cli("index --jobs 2");
    await t.cli("refactor rename alpha beta --dry-run");
    await t.cli("refactor rename volume LIMIT --dry-run");
    // Another function's name with the same parameters: a redefinition.
    await t.cli("refactor rename area volume --dry-run");
});

test("one function", async (t) => {
    await t.cli("index --jobs 2");
    await t.cli('refactor rename "factor path:main.cpp" length --dry-run');
    await t.cli('refactor rename "result path:main.cpp" length --dry-run');
});
