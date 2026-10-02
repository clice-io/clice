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

/// An entry's argv: `arguments` as written, or `command` split as the
/// POSIX shell the build system wrote it for would (shell-quote, the
/// npm ecosystem's standard shell lexer). Variables stay literal, as the
/// compiler received them.
export function entryArguments(entry: DatabaseEntry): string[] {
    if (entry.arguments !== undefined) {
        return entry.arguments;
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
