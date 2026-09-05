/// Compilation database helpers for test fixtures: entry construction for
/// harness-generated workspaces, and CMake generation for fixtures that
/// carry a CMakeLists.txt. Plain fixtures under tests/data ship their
/// compile_commands.json with relative paths, so nothing regenerates them.

import { execFileSync } from "node:child_process";
import * as path from "node:path";
import { fileURLToPath } from "node:url";

export const REPO_ROOT = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
export const TESTS_DIR = path.join(REPO_ROOT, "tests");
export const DATA_DIR = path.join(TESTS_DIR, "data");
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

function posix(p: string): string {
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

/// Generate compile_commands.json using CMake with Ninja backend.
export function generateCDB(workspace: string): void {
    const toolchain = path.join(REPO_ROOT, "cmake", "toolchain.cmake");
    execFileSync(
        "cmake",
        [
            "-G",
            "Ninja",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            `-DCMAKE_TOOLCHAIN_FILE=${toolchain}`,
            "-S",
            workspace,
            "-B",
            path.join(workspace, "build"),
        ],
        { timeout: 120_000, stdio: "pipe" },
    );
}
