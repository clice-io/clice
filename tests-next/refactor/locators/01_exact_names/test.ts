import { test, type TestContext } from "@clice/tools/testing";

/// `file:line:column` of the cursor `|` in the one place `anchor` occurs.
function place(t: TestContext, file: string, anchor: string): string {
    const text = t.disk.read(file);
    const snippet = anchor.replace("|", "");
    const begin = text.indexOf(snippet);
    if (begin < 0 || text.includes(snippet, begin + 1)) {
        throw new Error(`${file} does not hold ${JSON.stringify(snippet)} exactly once`);
    }
    const lines = text.slice(0, begin + anchor.indexOf("|")).split("\n");
    return `${file}:${String(lines.length)}:${String((lines.at(-1) ?? "").length + 1)}`;
}

test(async (t) => {
    await t.cli("index --workers 2");
    await t.cli("refactor rename --symbol comput --to evaluate --dry-run");
    await t.cli("refactor rename --symbol Compute --to evaluate --dry-run");
    await t.cli('refactor rename --symbol "comp*" --to evaluate --dry-run');
    await t.cli(
        `refactor rename --symbol ${place(t, "other.cpp", "return |compute(3)")} --to evaluate --dry-run`,
    );
});
