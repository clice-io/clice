import * as assert from "assert";
import * as fs from "fs";
import * as path from "path";

/// A new folder outside the workspace, which .vscode-test.mjs removes once
/// VS Code has exited.
export function scratchFolder(prefix: string): string {
    const scratch = process.env.CLICE_E2E_SCRATCH;
    assert.ok(scratch, "CLICE_E2E_SCRATCH names the folder .vscode-test.mjs made");
    return fs.mkdtempSync(path.join(scratch, prefix));
}
