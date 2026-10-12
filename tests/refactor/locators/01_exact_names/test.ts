import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index --workers 2");
    await t.cli("refactor rename --symbol comput --to evaluate --dry-run");
    await t.cli("refactor rename --symbol Compute --to evaluate --dry-run");
    await t.cli('refactor rename --symbol "comp*" --to evaluate --dry-run');
    await t.cli(
        `refactor rename --symbol ${t.place(t.at("other.cpp", "return |compute(3)"))} --to evaluate --dry-run`,
    );
});
