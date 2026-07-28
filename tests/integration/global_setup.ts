/// vitest globalSetup: runs once in the runner process before any worker,
/// like pytest_configure on the xdist controller — workers would race
/// writing the same compile_commands.json files.

import { generateTestDataCDBs } from "@clice/tools/compile-commands";
import { generateSnapCDBs } from "@clice/tools/snap/standalone";

export default function setup(): void {
    generateTestDataCDBs();
    // Behavioral tests may run against snap corpora as workspaces (e.g.
    // the document-links PCH tests), which need their CDBs in place.
    generateSnapCDBs();
}
