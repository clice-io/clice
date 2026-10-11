import * as path from "node:path";
import { test } from "@clice/tools/testing";

// clice format runs a clang-format executable; the editor formats with the
// formatter built into clice.

test("a named clang-format that does not exist", async (t) => {
    await t.cli("format --clang-format bin/clang-format");
});

test("no clang-format on PATH", async (t) => {
    // A PATH holding no clang-format, as on a machine with only clice
    // installed; Windows spells the variable Path.
    const bin = path.join(t.root, "bin");
    await t.cli("format", { env: { PATH: bin, Path: bin } });
});
