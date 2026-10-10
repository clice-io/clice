/// Copy a sample project to a directory and write the compilation database
/// its manifest describes: the editor tests' workspaces, and a project to
/// try clice on by hand.
///
/// Usage: node tools/sample.ts <sample> <dir>   (a sample is a directory of
/// samples/, e.g. tiny or shapes/headers; <dir> is replaced)

import * as fs from "node:fs";
import * as path from "node:path";
import { materialize, writeDatabase } from "./client/project.ts";
import { Workspace } from "./client/workspace.ts";

const [sample, dir, ...rest] = process.argv.slice(2);
if (sample === undefined || dir === undefined || rest.length > 0) {
    console.error("Usage: node tools/sample.ts <sample> <dir>");
    process.exit(64);
}
fs.rmSync(dir, { recursive: true, force: true });
fs.mkdirSync(dir, { recursive: true });
const workspace = new Workspace(path.resolve(dir));
const manifest = materialize(sample, workspace);
if (manifest.units !== undefined) {
    writeDatabase(workspace, manifest);
}
console.log(`${sample} -> ${workspace.root}`);
