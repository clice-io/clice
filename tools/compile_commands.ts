/// Compilation database helpers for test fixtures: entry construction for
/// harness-generated workspaces, and the repository's test directories.

import * as path from "node:path";
import { fileURLToPath } from "node:url";

export const REPO_ROOT = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const TESTS_DIR = path.join(REPO_ROOT, "tests");
export const SNAP_DIR = path.join(TESTS_DIR, "snap");

export interface CDBEntry {
    directory: string;
    file: string;
    arguments: string[];
}

export interface CDBEntryOptions {
    extraArgs?: string[] | undefined;
    std?: string | undefined;
}

export function posix(p: string): string {
    return p.split(path.sep).join("/");
}

export function buildCDBEntry(
    directory: string,
    source: string,
    options: CDBEntryOptions = {},
): CDBEntry {
    const file = posix(source);
    return {
        directory: posix(directory),
        file,
        arguments: [
            "clang++",
            `-std=${options.std ?? "c++17"}`,
            "-fsyntax-only",
            ...(options.extraArgs ?? []),
            file,
        ],
    };
}
