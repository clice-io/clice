/// The compilation database a scenario's build wrote, read the way its
/// own consumers read it.

import * as fs from "node:fs";
import * as path from "node:path";
import { parse } from "shell-quote";

export interface DatabaseEntry {
    directory: string;
    file: string;
    arguments?: string[];
    command?: string;
}

/// The database at the project root or in `build/`, the two places clice
/// looks without configuration.
export function readDatabase(root: string): DatabaseEntry[] {
    const found = ["compile_commands.json", "build/compile_commands.json"]
        .map((rel) => path.join(root, rel))
        .filter((file) => fs.existsSync(file));
    const [only] = found;
    if (found.length !== 1 || only === undefined) {
        throw new Error(`expected one compile_commands.json under ${root}, found ${found.length}`);
    }
    return JSON.parse(fs.readFileSync(only, "utf8")) as DatabaseEntry[];
}

/// A Windows command line split by the CommandLineToArgvW rules the
/// quoting of build tools relies on: whitespace outside quotes separates, 2n backslashes
/// before a quote are n backslashes and the quote toggles quoting, 2n+1
/// are n backslashes and a literal quote, other backslashes are literal.
/// No maintained npm package implements these rules.
function splitWindows(command: string): string[] {
    const args: string[] = [];
    let current = "";
    let started = false;
    let quoted = false;
    for (let i = 0; i < command.length; i += 1) {
        const c = command[i] ?? "";
        if (c === "\\") {
            let count = 0;
            while (command[i + count] === "\\") {
                count += 1;
            }
            if (command[i + count] === '"') {
                current += "\\".repeat(Math.floor(count / 2));
                if (count % 2 === 1) {
                    current += '"';
                    i += count;
                } else {
                    i += count - 1;
                }
            } else {
                current += "\\".repeat(count);
                i += count - 1;
            }
            started = true;
        } else if (c === '"') {
            quoted = !quoted;
            started = true;
        } else if (!quoted && (c === " " || c === "\t")) {
            if (started) {
                args.push(current);
                current = "";
                started = false;
            }
        } else {
            current += c;
            started = true;
        }
    }
    if (started) {
        args.push(current);
    }
    return args;
}

/// An entry's argv: `arguments` as written, or `command` split by the
/// rules of the platform it was written for — on POSIX by shell-quote,
/// the npm ecosystem's standard shell lexer, with variables kept literal
/// as the compiler received them.
export function entryArguments(entry: DatabaseEntry): string[] {
    if (entry.arguments !== undefined) {
        return entry.arguments;
    }
    if (process.platform === "win32") {
        return splitWindows(entry.command ?? "");
    }
    return parse(entry.command ?? "", (name) => `$${name}`).map((token) => {
        if (typeof token === "string") {
            return token;
        }
        if ("op" in token && token.op === "glob") {
            return token.pattern;
        }
        throw new Error(`shell syntax a compile command cannot carry: ${entry.command}`);
    });
}

export function entrySource(entry: DatabaseEntry): string {
    return path.resolve(entry.directory, entry.file);
}

/// Whether two absolute paths name the same file as the platform compares
/// them: Windows build tools spell drive letters either case.
export function samePath(a: string, b: string): boolean {
    return process.platform === "win32" ? a.toLowerCase() === b.toLowerCase() : a === b;
}
