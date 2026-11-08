import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The sources a module rewrites start with a byte order mark, which the
// files they become keep: cl reads a file without one in the ANSI code page.
test(async (t) => {
    for (const file of ["app/core/text.h", "app/core/text.cpp"]) {
        t.disk.write(file, `\ufeff${t.disk.read(file)}`);
    }
    await t.cli("index");
    await t.cli("modularize --scope app/** --partition partition.json --out wrap");
    expect(t.disk.read("app/core/text.cppm").startsWith("\ufeff")).toBe(true);
    expect(t.disk.read("app/core/text.cpp").startsWith("\ufeff")).toBe(true);
    expect(t.disk.read("app/main.cpp").startsWith("\ufeff")).toBe(false);
});
