/// Compilation database generation for test fixtures.

import { execFileSync } from "node:child_process";
import * as fs from "node:fs";
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

/// Generate compile_commands.json for all static test data directories.
export function generateTestDataCDBs(dataDir: string = DATA_DIR): void {
    const write = (directory: string, entries: CDBEntry[]) => {
        // Atomic write: concurrent vitest invocations (one per porting agent)
        // regenerate the same CDBs; a rename never exposes a truncated file
        // to a server reading it.
        const target = path.join(directory, "compile_commands.json");
        const tmp = `${target}.tmp-${process.pid}`;
        fs.writeFileSync(tmp, JSON.stringify(entries, null, 2));
        fs.renameSync(tmp, target);
    };

    const single = (name: string, extraArgs: string[] = []) => {
        const dir = path.join(dataDir, name);
        const main = path.join(dir, "main.cpp");
        if (fs.existsSync(main)) {
            write(dir, [buildCDBEntry(dir, main, { extraArgs })]);
        }
    };

    single("hello_world");

    // header_context (always regenerate — absolute paths)
    const hcDir = path.join(dataDir, "header_context");
    single("header_context", [`-I${posix(hcDir)}`]);

    // multi_context (same file, two configs)
    const mcDir = path.join(dataDir, "multi_context");
    const mcMain = path.join(mcDir, "main.cpp");
    if (fs.existsSync(mcMain)) {
        write(mcDir, [
            buildCDBEntry(mcDir, mcMain, { extraArgs: ["-DCONFIG_A"] }),
            buildCDBEntry(mcDir, mcMain, { extraArgs: ["-DCONFIG_B"] }),
        ]);
    }

    single("include_completion", ["-I."]);

    // config_rules_toml / config_rules_no_config — rules tests must start
    // from a CDB that does NOT include the flag the rule will append, so the
    // rule's effect is observable through diagnostics.
    single("config_rules_toml");
    single("config_rules_no_config");

    single("formatting");

    // pch_test
    const ptDir = path.join(dataDir, "pch_test");
    if (fs.existsSync(ptDir)) {
        const entries = ["main.cpp", "no_includes.cpp"]
            .map((name) => path.join(ptDir, name))
            .filter((src) => fs.existsSync(src))
            .map((src) => buildCDBEntry(ptDir, src));
        if (entries.length > 0) {
            write(ptDir, entries);
        }
    }
}
