import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index --jobs 2");
    // An unscoped enumerator is declared in the scope around its enum.
    await t.cli("refactor rename fast compute --dry-run");
    await t.cli("refactor rename fast slow --dry-run");
    // A scoped one only in its enum.
    await t.cli("refactor rename Color::red compute --dry-run");
    await t.cli("refactor rename Color::red green --dry-run");
});
