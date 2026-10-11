/// A case's output: blocks in run order, each a label and its body indented
/// under it (`label: text` when the body is one line), at any depth. Tests
/// of a script with several open with `=== name`, files of an answer with
/// `--- path`, commands with `$ clice …`. A label known wrong carries
/// `  ✗ known wrong: why` in output.snap itself, kept by every update while
/// the label exists. Compared with output.snap, rewritten under
/// UPDATE_SNAPSHOTS=1 with a summary of what moved.

import * as fs from "node:fs";
import * as path from "node:path";
import { createTwoFilesPatch } from "diff";
import type { Case } from "./case.ts";

const SIZE_BUDGET = 50;
const FLAG = "  ✗ known wrong: ";
const RESERVED = ["$ ", "--- ", "=== "];

function indent(lines: readonly string[]): string[] {
    return lines.map((line) => (line === "" ? "" : `  ${line}`));
}

/// A label names a block: one line, no `: ` (which ends it), no prefix the
/// output reserves for commands and sections.
function checkLabel(label: string): void {
    if (
        label === "" ||
        label.trim() !== label ||
        label.includes("\n") ||
        label.includes(": ") ||
        label.endsWith(":") ||
        RESERVED.some((prefix) => label.startsWith(prefix))
    ) {
        throw new Error(
            `'${label}' is no label: one line, no ': ', not starting with ${RESERVED.join(" ")}`,
        );
    }
}

/// The lines of block `label`: `label: line` for a one-line body, else the
/// label and the body indented.
export function formatBlock(label: string, body: string | readonly string[]): string[] {
    checkLabel(label);
    const lines = typeof body === "string" ? body.split("\n") : [...body];
    while (lines.length > 0 && lines.at(-1) === "") {
        lines.pop();
    }
    const only = lines[0] ?? "(none)";
    if (lines.length <= 1 && !only.startsWith(" ")) {
        return [`${label}: ${only}`];
    }
    return [label, ...indent(lines.length === 0 ? [only] : lines)];
}

/// One test's blocks.
export class Output {
    readonly lines: string[] = [];
    private readonly labels = new Set<string>();

    private claim(label: string): void {
        if (this.labels.has(label)) {
            throw new Error(`two blocks labeled '${label}'`);
        }
        this.labels.add(label);
    }

    block(label: string, body: string | readonly string[]): void {
        this.claim(label);
        this.lines.push(...formatBlock(label, body));
    }

    /// A command's transcript, headed by its command line.
    transcript(header: string, body: readonly string[]): void {
        this.lines.push(header, ...indent(body));
    }

    /// Blocks an answer already formatted, labels unique in their file's
    /// section.
    blocks(lines: readonly string[]): void {
        let section = "";
        for (const line of lines) {
            if (line.startsWith("--- ")) {
                section = `${line} ▸ `;
            } else if (line !== "" && !line.startsWith(" ")) {
                this.claim(`${section}${headerLabel(line)}`);
            }
        }
        this.lines.push(...lines);
    }
}

/// The whole output of a case from its tests' blocks, in registration order.
export function assemble(tests: readonly { name: string; lines: readonly string[] }[]): string {
    const several = tests.length > 1;
    const out: string[] = [];
    for (const { name, lines } of tests) {
        if (lines.length === 0) {
            continue;
        }
        if (several) {
            if (out.length > 0) {
                out.push("");
            }
            out.push(`=== ${name}`);
        }
        out.push(...lines);
    }
    return out.length === 0 ? "" : `${out.join("\n")}\n`;
}

/// A header line split into its label, its one-line body and its flag.
function parseHeader(line: string): { label: string; inline?: string; reason?: string } {
    const flagAt = line.indexOf(FLAG);
    const head = flagAt < 0 ? line : line.slice(0, flagAt);
    const reason = flagAt < 0 ? undefined : line.slice(flagAt + FLAG.length);
    const withReason = reason === undefined ? {} : { reason };
    if (head.startsWith("$ ")) {
        return { label: head, ...withReason };
    }
    const colon = head.indexOf(": ");
    return colon < 0
        ? { label: head, ...withReason }
        : { label: head.slice(0, colon), inline: head.slice(colon + 2), ...withReason };
}

function headerLabel(line: string): string {
    return parseHeader(line).label;
}

interface Parsed {
    key: string;
    label: string;
    inline?: string;
    reason?: string;
    /// Index of the header line, and of the line after the block.
    from: number;
    to: number;
}

/// The top-level blocks of `text`, keyed by test, file and label; a command
/// run again is keyed `$ clice … #2`.
function parse(text: string): { lines: string[]; blocks: Parsed[] } {
    const lines = text.split("\n");
    const out: Parsed[] = [];
    let test = "";
    let file = "";
    const seen = new Map<string, number>();
    const close = (end: number): void => {
        const last = out.at(-1);
        if (last !== undefined && last.to < 0) {
            last.to = end;
        }
    };
    lines.forEach((line, index) => {
        if (line.startsWith("=== ")) {
            close(index);
            test = `${line.slice(4)} ▸ `;
            file = "";
        } else if (line.startsWith("--- ")) {
            close(index);
            file = `${line.slice(4)} ▸ `;
        } else if (line !== "" && !line.startsWith(" ")) {
            close(index);
            const header = parseHeader(line);
            let key = `${test}${file}${header.label}`;
            const count = (seen.get(key) ?? 0) + 1;
            seen.set(key, count);
            if (count > 1) {
                key = `${key} #${String(count)}`;
            }
            out.push({ key, ...header, from: index, to: -1 });
        }
    });
    close(lines.length);
    return { lines, blocks: out };
}

/// The output's blocks by key, each its lines with the flag dropped.
export function blocks(text: string): Map<string, string> {
    const { lines, blocks: parsed } = parse(text);
    return new Map(
        parsed.map((block) => {
            const body = lines.slice(block.from + 1, block.to);
            while (body.length > 0 && body.at(-1) === "") {
                body.pop();
            }
            const head = block.inline === undefined ? [] : [block.inline];
            return [block.key, [...head, ...body.map((line) => line.slice(2))].join("\n")];
        }),
    );
}

/// The labels flagged known wrong in `text`, by key, with the reason.
export function flags(text: string): Map<string, string> {
    return new Map(
        parse(text).blocks.flatMap((block) =>
            block.reason === undefined ? [] : [[block.key, block.reason] as const],
        ),
    );
}

/// `text` with the blocks `wrong` names flagged; a flagged block always has
/// its body below the label.
export function applyFlags(text: string, wrong: ReadonlyMap<string, string>): string {
    const { lines, blocks: parsed } = parse(text);
    for (const block of [...parsed].reverse()) {
        const reason = wrong.get(block.key);
        if (reason === undefined) {
            continue;
        }
        const body = block.inline === undefined ? [] : [`  ${block.inline}`];
        lines.splice(block.from, 1, `${block.label}${FLAG}${reason}`, ...body);
    }
    return lines.join("\n");
}

/// How much of the output is right: every top-level block, some, or none.
export function statusOf(text: string): "supported" | "partial" | "unsupported" {
    const parsed = parse(text).blocks;
    const wrong = parsed.filter((block) => block.reason !== undefined).length;
    return wrong === 0 ? "supported" : wrong === parsed.length ? "unsupported" : "partial";
}

/// The sections of a case's output.snap by test name: what a test that has
/// no meaning on this platform left where it runs.
export function sections(c: Case): Map<string, string[]> {
    const file = path.join(c.dir, "output.snap");
    const out = new Map<string, string[]>();
    if (!fs.existsSync(file)) {
        return out;
    }
    let name: string | undefined;
    for (const line of fs.readFileSync(file, "utf8").split("\n")) {
        if (line.startsWith("=== ")) {
            name = line.slice(4);
            out.set(name, []);
        } else if (name !== undefined) {
            out.get(name)?.push(line);
        }
    }
    for (const lines of out.values()) {
        while (lines.length > 0 && lines.at(-1) === "") {
            lines.pop();
        }
    }
    return out;
}

/// A test's output within the size budget, or a reason it may exceed it.
export function checkSize(c: Case, test: string, lines: number, long: string | undefined): void {
    if (lines > SIZE_BUDGET && long === undefined) {
        const which = test === "" ? "" : ` '${test}'`;
        throw new Error(
            `${c.id}: test${which} outputs ${String(lines)} lines, over ${String(SIZE_BUDGET)}; long: says why`,
        );
    }
}

interface Change {
    id: string;
    changed: string[];
    fixed: string[];
    added: string[];
    removed: string[];
}

function recordChange(dir: string, c: Case, before: string, after: string): void {
    const wrong = flags(before);
    const old = blocks(before);
    const now = blocks(after);
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

/// What the update run changed, case by case: known-wrong labels whose
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

/// Compare `text` with the case's output.snap, its known-wrong flags kept,
/// or rewrite it when updating.
export function checkOutput(c: Case, rendered: string): void {
    const file = path.join(c.dir, "output.snap");
    const before = fs.existsSync(file) ? fs.readFileSync(file, "utf8") : "";
    if (before.split("\n").some((line) => line.startsWith(" ") && line.includes(FLAG))) {
        throw new Error(`${c.id}/output.snap: a known-wrong flag goes on a label at the margin`);
    }
    const text = applyFlags(rendered, flags(before));
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
        return;
    }
    if (before !== text) {
        const patch = createTwoFilesPatch("output.snap", "this run", before, text, "", "", {
            context: 3,
        });
        throw new Error(
            `${c.id}: the output differs from output.snap (UPDATE_SNAPSHOTS=1 accepts it)\n${patch}`,
        );
    }
}
