/// Fragments compiled in their includer's context: a class body or an
/// X-macro list sits inside a declaration the includer opens before the
/// include and closes after it.

import * as path from "node:path";
import { runProcess } from "@clice/tools/client";
import { cliceExecutable, expect, test } from "../fixtures.ts";

test("inspect closes the fragment", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.write(
        "body.inc",
        "int field_a;\nint field_b;\nint sum() const { return field_a + field_b; }\n",
    );
    workspace.write("base.h", "#pragma once\nstruct Base {};\n");
    workspace.write(
        "rec.cpp",
        '#include "base.h"\nstruct Rec : Base {\n#include "body.inc"\n};\n' +
            "int main() { Rec r; return r.sum(); }\n",
    );
    workspace.writeCDB(["rec.cpp"]);

    const run = await runProcess(
        cliceExecutable(),
        ["inspect", "document_symbol", path.join(workspace.root, "body.inc")],
        { timeout: 120_000 },
    );
    expect(run.status, run.stderr).toBe(0);
    const output = JSON.parse(run.stdout) as {
        files: Record<string, { result: { name: string }[]; diagnostics: string[] | null }>;
    };
    const body = output.files["body.inc"];
    expect(body?.diagnostics ?? []).toEqual([]);
    expect(body?.result.map((symbol) => symbol.name)).toEqual(["field_a", "field_b", "sum"]);
});
