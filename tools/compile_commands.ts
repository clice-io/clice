/// Compilation database helpers for test fixtures: entry construction for
/// harness-generated workspaces, and CMake generation for fixtures that
/// carry a CMakeLists.txt. Plain fixtures under tests/data ship their
/// compile_commands.json with relative paths, so nothing regenerates them.

import { execFileSync } from "node:child_process";
import * as path from "node:path";
import { fileURLToPath } from "node:url";

export const REPO_ROOT = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const TESTS_DIR = path.join(REPO_ROOT, "tests");
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
///
/// The toolchain file wires ccache in as the compiler launcher, which only
/// the configure-time probes would go through here: they compile in a
/// scratch directory with a fresh random name each time, and on Windows
/// with -g, so ccache hashes that directory and every probe is a guaranteed
/// miss stored into the shared cache.
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
        { timeout: 120_000, stdio: "pipe", env: { ...process.env, CCACHE_DISABLE: "1" } },
    );
}
