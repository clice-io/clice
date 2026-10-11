/// A case's output, snap.toml: per test, the blocks it showed in run order,
/// `[<test>.<label>]` each, a table: a request's answer under its kind,
/// a record's entries under their labels, a command's transcript under its
/// command line. A block known wrong carries `wrong = "why"`, kept by every
/// update while the block exists. Compared with snap.toml, rewritten under
/// UPDATE_SNAPSHOTS=1 with a summary of what moved.

import * as fs from "node:fs";
import * as path from "node:path";
import { createTwoFilesPatch } from "diff";
import { WRONG } from "../render/shown.ts";
import { readToml, writeToml, type Table } from "../render/value.ts";
import type { Case } from "./case.ts";

export const SNAPSHOT = "snap.toml";
const SEPARATOR = " ▸ ";

/// One test's blocks, in the order shown.
export class Output {
    readonly blocks = new Map<string, Table>();

    block(label: string, table: Table): void {
        if (this.blocks.has(label) || label === WRONG) {
            throw new Error(`two blocks labeled '${label}'`);
        }
        this.blocks.set(label, table);
    }

    /// A command's transcript under its command line; a command run again
    /// is `#2`, and so on.
    transcript(label: string, table: Table): void {
        let key = label;
        for (let count = 2; this.blocks.has(key); count += 1) {
            key = `${label} #${String(count)}`;
        }
        this.blocks.set(key, table);
    }
}

export type Blocks = ReadonlyMap<string, Table>;

/// A case's tests' blocks by test name, in registration order.
export type Snapshot = ReadonlyMap<string, Blocks>;

function entries(snapshot: Snapshot): { path: string[]; table: Table }[] {
    return [...snapshot].flatMap(([test, blocks]) =>
        [...blocks].map(([label, table]) => ({ path: [test, label], table })),
    );
}

export function writeSnapshot(snapshot: Snapshot): string {
    return writeToml(entries(snapshot));
}

function snapshotPath(c: Case): string {
    return path.join(c.dir, SNAPSHOT);
}

function readFile(c: Case): string {
    const file = snapshotPath(c);
    return fs.existsSync(file) ? fs.readFileSync(file, "utf8") : "";
}

/// The blocks of `text` by test, as snap.toml holds them.
export function readSnapshot(text: string): Map<string, Map<string, Table>> {
    const out = new Map<string, Map<string, Table>>();
    for (const [test, blocks] of Object.entries(readToml(text))) {
        out.set(test, new Map(Object.entries(blocks as Record<string, Table>)));
    }
    return out;
}

/// The known-wrong reasons of `text`, by `test ▸ label`.
function flags(text: string): Map<string, string> {
    const out = new Map<string, string>();
    for (const [test, blocks] of readSnapshot(text)) {
        for (const [label, table] of blocks) {
            const reason = table[WRONG];
            if (reason === undefined) {
                continue;
            }
            if (typeof reason !== "string") {
                throw new Error(`[${test}.${label}]: ${WRONG} is the reason, a string`);
            }
            out.set(`${test}${SEPARATOR}${label}`, reason);
        }
    }
    return out;
}

/// `snapshot` with the blocks `wrong` names flagged, the reason first.
function flagged(snapshot: Snapshot, wrong: ReadonlyMap<string, string>): Snapshot {
    return new Map(
        [...snapshot].map(([test, blocks]) => [
            test,
            new Map(
                [...blocks].map(([label, table]) => {
                    const reason = wrong.get(`${test}${SEPARATOR}${label}`);
                    return [label, reason === undefined ? table : { [WRONG]: reason, ...table }];
                }),
            ),
        ]),
    );
}

/// How much of the output is right: every block, some, or none.
export function statusOf(text: string): "supported" | "partial" | "unsupported" {
    const all = [...readSnapshot(text).values()].reduce((sum, blocks) => sum + blocks.size, 0);
    const wrong = flags(text).size;
    return wrong === 0 ? "supported" : wrong === all ? "unsupported" : "partial";
}

/// The case's snapshot as it stands: the blocks of a test that has no
/// meaning on this platform, kept where it runs.
export function recorded(c: Case): Map<string, Map<string, Table>> {
    return readSnapshot(readFile(c));
}

interface Change {
    id: string;
    changed: string[];
    fixed: string[];
    added: string[];
    removed: string[];
}

/// Each block's text, by `test ▸ label`, its flag left out.
function blockTexts(text: string): Map<string, string> {
    const out = new Map<string, string>();
    for (const [test, blocks] of readSnapshot(text)) {
        for (const [label, table] of blocks) {
            const rest = Object.entries(table).filter(([key]) => key !== WRONG);
            out.set(`${test}${SEPARATOR}${label}`, JSON.stringify(rest));
        }
    }
    return out;
}

function recordChange(dir: string, c: Case, before: string, after: string): void {
    const wrong = flags(before);
    const old = blockTexts(before);
    const now = blockTexts(after);
    const change: Change = { id: c.id, changed: [], fixed: [], added: [], removed: [] };
    for (const [key, text] of now) {
        const previous = old.get(key);
        if (previous === undefined) {
            change.added.push(key);
        } else if (previous !== text) {
            (wrong.has(key) ? change.fixed : change.changed).push(key);
        }
    }
    for (const key of old.keys()) {
        if (!now.has(key)) {
            change.removed.push(wrong.has(key) ? `${key} (was known wrong)` : key);
        }
    }
    fs.mkdirSync(dir, { recursive: true });
    fs.appendFileSync(
        path.join(dir, `${String(process.pid)}.jsonl`),
        `${JSON.stringify(change)}\n`,
    );
}

/// What the update run changed, case by case: known-wrong blocks whose
/// output moved (likely fixed) apart from the rest (likely regressed).
export function renderSummary(dir: string): string {
    if (!fs.existsSync(dir)) {
        return "";
    }
    const changes = fs
        .readdirSync(dir)
        .flatMap((name) => fs.readFileSync(path.join(dir, name), "utf8").split("\n"))
        .filter((line) => line !== "")
        .map((line) => JSON.parse(line) as Change)
        .sort((a, b) => (a.id < b.id ? -1 : 1));
    if (changes.length === 0) {
        return "";
    }
    const regressed = changes.filter((change) => change.changed.length + change.removed.length > 0);
    const out = [
        `outputs changed in ${String(changes.length)} cases, ${String(regressed.length)} with endorsed output changed or gone`,
    ];
    for (const change of changes) {
        out.push(`  ${change.id}`);
        const kinds: [string, string[]][] = [
            ["changed", change.changed],
            ["removed", change.removed],
            ["changed, was known wrong", change.fixed],
            ["added", change.added],
        ];
        for (const [kind, keys] of kinds) {
            if (keys.length > 0) {
                out.push(`    ${kind}: ${keys.join(", ")}`);
            }
        }
    }
    return out.join("\n");
}

/// Compare `snapshot` with the case's snap.toml, its known-wrong flags
/// kept, or rewrite it when updating. Returns the snapshot flagged.
export function checkOutput(c: Case, snapshot: Snapshot): Snapshot {
    const file = snapshotPath(c);
    const before = readFile(c);
    const result = flagged(snapshot, flags(before));
    const text = writeSnapshot(result);
    if (process.env["UPDATE_SNAPSHOTS"] === "1") {
        if (before !== text) {
            if (text === "") {
                fs.rmSync(file);
            } else {
                fs.writeFileSync(file, text);
            }
            const summary = process.env["CLICE_SNAPSHOT_SUMMARY_DIR"];
            if (summary !== undefined) {
                recordChange(summary, c, before, text);
            }
        }
        return result;
    }
    if (before !== text) {
        const patch = createTwoFilesPatch(SNAPSHOT, "this run", before, text, "", "", {
            context: 3,
        });
        throw new Error(
            `${c.id}: the output differs from ${SNAPSHOT} (UPDATE_SNAPSHOTS=1 accepts it)\n${patch}`,
        );
    }
    return result;
}
