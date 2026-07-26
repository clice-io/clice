/// Navigation right after an edit must resolve against the edited buffer:
/// the server settles the file's compile before answering, with no timeout.

import * as proto from "vscode-languageserver-protocol";
import { expect, test } from "../../fixtures.ts";

const SOURCE_V1 = "int foo() { return 1; }\nint main() { return foo(); }\n";

/// Inserts a line at the top: every position shifts, so a query resolved
/// against the pre-edit index would name the wrong symbol or nothing.
const SOURCE_V2 = "// shift\nint foo() { return 1; }\nint main() { return foo(); }\n";

test("navigation after change", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", SOURCE_V1);
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait("main.cpp");

    // No wait after the edit: the requests below must settle the compile
    // themselves before resolving the cursor.
    client.change(uri, 2, SOURCE_V2);

    const defs = (await client.definitionAt(uri, 2, 20)) as proto.Location[] | null;
    expect(defs?.length ?? 0, "definition right after an edit returned nothing").toBeGreaterThan(0);
    expect(defs![0]!.range.start.line).toBe(1);

    const refs = await client.referencesAt(uri, 2, 20, { includeDeclaration: false });
    expect(refs?.length ?? 0, "references right after an edit returned nothing").toBeGreaterThan(0);
    expect(new Set(refs!.map((loc) => loc.range.start.line))).toEqual(new Set([2]));
});
