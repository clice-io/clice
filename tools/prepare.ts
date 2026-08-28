/// Prepare test data fixtures for editor E2E tests.
///
/// Usage: node tools/prepare.ts <fixture> [<fixture> ...]
///
/// Each fixture is a subdirectory of tests/data. Fixtures with a
/// CMakeLists.txt get compile_commands.json generated via CMake; plain
/// fixtures (e.g. hello_world) are covered by generateTestDataCDBs.
/// Stale .clice caches are removed so every run starts fresh.

import * as fs from "node:fs";
import * as path from "node:path";
import { DATA_DIR, generateCDB, generateTestDataCDBs } from "./compile_commands.ts";

const USAGE = "Usage: node tools/prepare.ts <fixture> [<fixture> ...]";

/// Remove the clice cache for the workspace.
function cleanCache(workspace: string): void {
    fs.rmSync(path.join(workspace, ".clice"), { recursive: true, force: true });
}

function main(fixtures: string[]): number {
    if (fixtures.length === 0) {
        console.error(USAGE);
        return 64;
    }

    generateTestDataCDBs(DATA_DIR);

    for (const fixture of fixtures) {
        const workspace = path.join(DATA_DIR, fixture);
        if (!fs.existsSync(workspace) || !fs.statSync(workspace).isDirectory()) {
            console.error(`error: no such fixture: ${workspace}`);
            return 1;
        }
        if (fs.existsSync(path.join(workspace, "CMakeLists.txt"))) {
            generateCDB(workspace);
        }
        const cdbs = [
            path.join(workspace, "compile_commands.json"),
            path.join(workspace, "build", "compile_commands.json"),
        ];
        if (!cdbs.some((cdb) => fs.existsSync(cdb))) {
            console.error(`error: no compile_commands.json for ${fixture}`);
            return 1;
        }
        cleanCache(workspace);
        console.log(`prepared ${fixture}`);
    }

    return 0;
}

process.exit(main(process.argv.slice(2)));
