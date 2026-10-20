import { test } from "@clice/tools/testing";

// `clice inspect --markers` answers at the places the file names, by byte
// offset: a point for hover, a range for inlay hints, both for code actions,
// a point as an empty selection. Inlay hints without a range cover the file;
// code actions without a place have nothing to answer. It refuses a place
// outside the text, a name both a point and a range, and a directory.

test(async (t) => {
    const source = t.disk.read("main.cpp");
    const markers = (table: object): string => {
        t.disk.write("markers.json", JSON.stringify(table));
        return "--markers=markers.json";
    };
    const table = {
        points: { call: source.indexOf("add(1") },
        ranges: { use: { begin: source.indexOf("int use"), end: source.length } },
    };
    await t.cli(["inspect", "hover", "main.cpp", markers(table)]);
    await t.cli(["inspect", "inlay-hint", "main.cpp", markers(table)]);
    await t.cli(["inspect", "inlay-hint", "main.cpp"]);
    await t.cli(["inspect", "code-action", "main.cpp", markers(table)]);
    await t.cli(["inspect", "code-action", "main.cpp"]);
    await t.cli(["inspect", "hover", "main.cpp", markers({ points: { far: source.length + 1 } })]);
    await t.cli([
        "inspect",
        "inlay-hint",
        "main.cpp",
        markers({ ranges: { reversed: { begin: 3, end: 0 } } }),
    ]);
    await t.cli([
        "inspect",
        "hover",
        "main.cpp",
        markers({ points: { use: 0 }, ranges: { use: { begin: 0, end: 3 } } }),
    ]);
    await t.cli(["inspect", "hover", ".", markers(table)]);
});
