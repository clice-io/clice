import * as path from "node:path";
import { test } from "@clice/tools/testing";

// clice format formats with the formatter built into clice, as the editor
// does: no clang-format executable takes part.

test("no clang-format on PATH", async (t) => {
    // A PATH holding no clang-format, as on a machine with only clice
    // installed; Windows spells the variable Path. The file is named: the
    // build's files would ask the compiler, which this PATH hides too.
    // macOS keeps /usr/bin, which holds no clang-format: the sanitizer
    // builds find their symbolizer, atos, there.
    const bin = path.join(t.root, "bin");
    const dirs = process.platform === "darwin" ? [bin, "/usr/bin"] : [bin];
    const search = dirs.join(path.delimiter);
    await t.cli("format main.cpp", { env: { PATH: search, Path: search } });
});
