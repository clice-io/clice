import { test } from "@clice/tools/testing";

interface Ambiguous {
    result: { renames: { candidates: { symbol: string }[] }[] };
}

test(async (t) => {
    await t.cli("index --jobs 2");
    const ambiguous = await t.cli("refactor rename value amount --dry-run");
    await t.cli("refactor rename a::value amount --dry-run");
    const [, second] = (ambiguous.json as Ambiguous).result.renames[0]?.candidates ?? [];
    if (second === undefined) {
        throw new Error("the ambiguous name lists no second candidate");
    }
    await t.cli(["refactor", "rename", second.symbol, "amount"]);
});
