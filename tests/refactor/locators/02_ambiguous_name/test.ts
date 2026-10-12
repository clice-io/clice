import { test } from "@clice/tools/testing";

interface Ambiguous {
    result: { renames: { candidates: { symbol: string }[] }[] };
}

test(async (t) => {
    await t.cli("index --workers 2");
    const ambiguous = await t.cli("refactor rename --symbol value --to amount --dry-run");
    await t.cli("refactor rename --symbol a::value --to amount --dry-run");
    const [, second] = (ambiguous.json as Ambiguous).result.renames[0]?.candidates ?? [];
    if (second === undefined) {
        throw new Error("the ambiguous name lists no second candidate");
    }
    await t.cli(["refactor", "rename", "--symbol", second.symbol, "--to", "amount"]);
});
