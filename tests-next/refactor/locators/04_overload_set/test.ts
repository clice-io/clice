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

/// The id of the symbol defined at `anchor`, as `clice query` answers it.
async function symbolAt(t: TestContext, anchor: string): Promise<string> {
    const run = await t.cli(`query --method definition --symbol ${place(t, "main.cpp", anchor)}`);
    return (run.json as { result: { symbol: string } }).result.symbol;
}

test("renames the set", async (t) => {
    await t.cli("index --workers 2");
    // Renamed together, the template's open call keeps naming them.
    await t.cli("refactor rename --symbol scale --to resize --dry-run");
});

test("renames one overload", async (t) => {
    await t.cli("index --workers 2");
    const symbol = await symbolAt(t, "double |scale");
    await t.cli(["refactor", "rename", "--symbol", symbol, "--to", "resize", "--dry-run"]);
});

test("lists both overloads", async (t) => {
    await t.cli("index --workers 2");
    const first = await symbolAt(t, "int |scale");
    const second = await symbolAt(t, "double |scale");
    // The macro body spells both: it is noted once.
    t.disk.write("same.txt", `${first} => resize\n${second} => resize\n`);
    await t.cli("refactor rename --spec same.txt --dry-run");
    // The template's call names both, which would get two names.
    t.disk.write("apart.txt", `${first} => grow\n${second} => shrink\n`);
    await t.cli("refactor rename --spec apart.txt --dry-run");
});
