import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index --jobs 2");
    await t.cli("refactor rename comput evaluate --dry-run");
    await t.cli("refactor rename Compute evaluate --dry-run");
    await t.cli('refactor rename "comp*" evaluate --dry-run');
    await t.cli(
        `refactor rename ${t.place(t.at("other.cpp", "return |compute(3)"))} evaluate --dry-run`,
    );
});
