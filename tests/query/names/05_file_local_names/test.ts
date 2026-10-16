import { test } from "@clice/tools/testing";

// Symbols no other file can name (internal linkage, locals, parameters)
// are searched by name only within a file, `path:`; two of one name there
// are told apart by their candidates' locators.
test(async (t) => {
    await t.cli("index");
    for (const text of [
        '"rows"',
        '"rows" path:a.cpp',
        '"rows" path:b.cpp',
        "helper::rows path:a.cpp",
    ]) {
        await t.cli(["query", "symbol-search", text]);
    }
    await t.cli(["query", "definition", "a::rows path:a.cpp"]);
    const ambiguous = await t.cli(["query", "definition", '"rows" path:a.cpp']);
    const { candidates } = ambiguous.json as { candidates: { symbol: string }[] };
    for (const candidate of candidates) {
        await t.cli(["query", "definition", candidate.symbol]);
    }
});
