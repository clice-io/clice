import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The list and main.cpp are saved as UTF-8 with a byte order mark, as Windows
// tools write them: the mark is no part of the first locator, nor of the name
// main.cpp starts with.
test(async (t) => {
    for (const file of ["renames.txt", "main.cpp"]) {
        t.disk.write(file, `\ufeff${t.disk.read(file)}`);
    }
    await t.cli("index");
    await t.cli("refactor rename --spec renames.txt");
    expect(t.disk.read("main.cpp").startsWith("\ufeffTally make()")).toBe(true);
});
