/// `node tools/testing/editors/gate.ts <dir>`, after an editor's end-to-end
/// tests on the fixtures copied under <dir>: the servers they started left
/// no crash, anomaly or error-level line in their logs, as a case's
/// teardown checks.

import * as path from "node:path";
import * as fs from "node:fs";
import { ANOMALY_PATTERN, logFiles } from "../../process_gate.ts";
import { readLogs, undeclaredErrors } from "../logs/logs.ts";

/// What the logs of each copy under `dir` hold that they must not; a copy
/// no server logged under fails too, as a gate reading nothing passes
/// anything.
export function gateLogs(dir: string): string[] {
    const failures: string[] = [];
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
        if (!entry.isDirectory()) {
            continue;
        }
        const copy = path.join(dir, entry.name);
        const files = logFiles(copy);
        if (files.length === 0) {
            failures.push(`${entry.name}: no server logged under ${copy}`);
            continue;
        }
        const lines = readLogs(copy, files);
        // A worker's crash is an anomaly, the server's own a fatal line.
        const bad = [
            ...undeclaredErrors(lines, []),
            ...lines.filter((line) => ANOMALY_PATTERN.test(line.text)),
        ];
        for (const line of new Set(bad)) {
            failures.push(`${entry.name}: ${line.file}: ${line.text}`);
        }
    }
    return failures;
}

if (import.meta.main) {
    const [dir, ...rest] = process.argv.slice(2);
    if (dir === undefined || rest.length > 0) {
        console.error("usage: node tools/testing/editors/gate.ts <dir>");
        process.exit(64);
    }
    const failures = gateLogs(dir);
    if (failures.length > 0) {
        console.error(`the editors' servers logged what they must not:\n${failures.join("\n")}`);
        process.exit(1);
    }
    console.log(`the servers' logs under ${dir} are clean`);
}
