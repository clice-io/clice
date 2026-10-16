import { test, type TestContext } from "@clice/tools/testing";

/// The id of the symbol defined at `anchor`, as `clice query` answers it.
async function symbolAt(t: TestContext, anchor: string): Promise<string> {
    const run = await t.cli(`query definition ${t.place(t.at("main.cpp", anchor))}`);
    return (run.json as { result: { symbol: string } }).result.symbol;
}

test("renames the set", async (t) => {
    await t.cli("index --jobs 2");
    // Renamed together, the template's open call keeps naming them.
    await t.cli("refactor rename scale resize --dry-run");
});

test("renames one overload", async (t) => {
    await t.cli("index --jobs 2");
    const symbol = await symbolAt(t, "double |scale");
    await t.cli(["refactor", "rename", symbol, "resize", "--dry-run"]);
});

test("lists both overloads", async (t) => {
    await t.cli("index --jobs 2");
    const first = await symbolAt(t, "int |scale");
    const second = await symbolAt(t, "double |scale");
    // The macro body spells both: it is noted once.
    t.disk.write("same.txt", `${first} => resize\n${second} => resize\n`);
    await t.cli("refactor rename --spec same.txt --dry-run");
    // The template's call names both, which would get two names.
    t.disk.write("apart.txt", `${first} => grow\n${second} => shrink\n`);
    await t.cli("refactor rename --spec apart.txt --dry-run");
});
