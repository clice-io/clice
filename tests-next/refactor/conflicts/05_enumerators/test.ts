import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index --workers 2");
    // An unscoped enumerator is declared in the scope around its enum.
    await t.cli("refactor rename --symbol fast --to compute --dry-run");
    await t.cli("refactor rename --symbol fast --to slow --dry-run");
    // A scoped one only in its enum.
    await t.cli("refactor rename --symbol Color::red --to compute --dry-run");
    await t.cli("refactor rename --symbol Color::red --to green --dry-run");
});
