/// Copy cases out as projects an editor opens, for the editors' end-to-end
/// tests: `node tools/testing/cases/copy.ts <dir> <case>...` writes each
/// case to <dir>/<name>, its name without the number, the way a test's copy
/// is made: markers stripped, its databases written.

import * as fs from "node:fs";
import * as path from "node:path";
import { loadCases } from "./case.ts";
import { CASES_DIR, materialize, writeDatabases } from "./project.ts";

const [dir, ...ids] = process.argv.slice(2);
if (dir === undefined || ids.length === 0) {
    console.error("usage: node tools/testing/cases/copy.ts <dir> <case>...");
    process.exit(64);
}

const cases = loadCases(CASES_DIR);
for (const id of ids) {
    const c = cases.find((each) => each.id === id);
    if (c === undefined) {
        console.error(`error: no case ${id}`);
        process.exit(1);
    }
    const target = path.resolve(dir, c.name.replace(/^\d\d_/, ""));
    fs.rmSync(target, { recursive: true, force: true });
    fs.mkdirSync(target, { recursive: true });
    const root = fs.realpathSync.native(target);
    writeDatabases(c.project, materialize(c.dir, root), root);
    console.log(`${id} -> ${root}`);
}
