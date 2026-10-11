/// What a run exercised of the product surface: LSP and clice methods sent,
/// command lines run, configuration keys set. Each test process
/// appends to its own file under CLICE_COVERAGE_DIR; the coverage check
/// reads them all after the run. Without the variable nothing is recorded.

import * as fs from "node:fs";
import * as path from "node:path";

export type Surface = "method" | "invocation" | "config";

const recorded = new Set<string>();

export function record(surface: Surface, item: string): void {
    const dir = process.env["CLICE_COVERAGE_DIR"];
    if (dir === undefined || dir === "") {
        return;
    }
    const entry = `${surface} ${item}`;
    if (recorded.has(entry)) {
        return;
    }
    recorded.add(entry);
    fs.mkdirSync(dir, { recursive: true });
    fs.appendFileSync(path.join(dir, `${process.pid}.txt`), `${entry}\n`);
}

/// A command line run: its subcommand words and the flags it passed,
/// values and positional arguments dropped (`query --method --name`).
export function recordInvocation(args: readonly string[]): void {
    const words: string[] = [];
    let subcommand = true;
    for (const arg of args) {
        if (arg.startsWith("--")) {
            subcommand = false;
            words.push(arg.split("=")[0] ?? arg);
        } else if (subcommand && /^[a-z][\w-]*$/.test(arg)) {
            words.push(arg);
        } else {
            subcommand = false;
        }
    }
    record("invocation", words.join(" "));
}

/// Every key a clice.toml sets, by a line scan: `[section]` and
/// `[[section]]` headers prefix the `key = value` lines below them.
export function recordToml(text: string): void {
    let prefix = "";
    for (const line of text.split("\n")) {
        const header = /^\s*\[\[?([\w.]+)\]\]?\s*(#.*)?$/.exec(line);
        if (header !== null) {
            prefix = header[1] ?? "";
            continue;
        }
        const key = /^\s*([\w.]+)\s*=/.exec(line);
        if (key !== null) {
            record("config", prefix === "" ? (key[1] ?? "") : `${prefix}.${key[1] ?? ""}`);
        }
    }
}

/// Every key path a configuration object sets: `project.readonly`,
/// `rules.append`, ... (array elements fold into their key).
export function recordConfig(config: unknown, prefix = ""): void {
    if (Array.isArray(config)) {
        for (const element of config) {
            recordConfig(element, prefix);
        }
        return;
    }
    if (typeof config !== "object" || config === null) {
        if (prefix !== "") {
            record("config", prefix);
        }
        return;
    }
    for (const [key, value] of Object.entries(config)) {
        recordConfig(value, prefix === "" ? key : `${prefix}.${key}`);
    }
}

export function readRecords(dir: string): Map<Surface, Set<string>> {
    const out = new Map<Surface, Set<string>>();
    if (!fs.existsSync(dir)) {
        return out;
    }
    for (const name of fs.readdirSync(dir)) {
        for (const line of fs.readFileSync(path.join(dir, name), "utf8").split("\n")) {
            const space = line.indexOf(" ");
            if (space < 0) {
                continue;
            }
            const surface = line.slice(0, space) as Surface;
            const set = out.get(surface) ?? new Set<string>();
            set.add(line.slice(space + 1));
            out.set(surface, set);
        }
    }
    return out;
}
