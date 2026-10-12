import { test } from "@clice/tools/testing";

test("class members", async (t) => {
    await t.cli("index --workers 2");
    await t.cli("refactor rename --symbol Base::size --to capacity --dry-run");
    await t.cli("refactor rename --symbol Derived::capacity --to size --dry-run");
    await t.cli("refactor rename --symbol Base::grow --to Base --dry-run");
    await t.cli("refactor rename --symbol Derived --to capacity --dry-run");
});

test("one scope", async (t) => {
    await t.cli("index --workers 2");
    await t.cli("refactor rename --symbol alpha --to beta --dry-run");
    await t.cli("refactor rename --symbol volume --to LIMIT --dry-run");
    // Another function's name with the same parameters: a redefinition.
    await t.cli("refactor rename --symbol area --to volume --dry-run");
});

test("one function", async (t) => {
    await t.cli("index --workers 2");
    await t.cli('refactor rename --symbol "factor path:main.cpp" --to length --dry-run');
    await t.cli('refactor rename --symbol "result path:main.cpp" --to length --dry-run');
});
