/// Compilation database generation for test fixtures.

import { execFileSync } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import { fileURLToPath } from "node:url";

export const TESTS_DIR = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
export const REPO_ROOT = path.dirname(TESTS_DIR);
export const DATA_DIR = path.join(TESTS_DIR, "data");
export const SNAPSHOTS_DIR = path.join(TESTS_DIR, "snapshots");

interface CdbEntry {
    directory: string;
    file: string;
    arguments: string[];
}

function posix(p: string): string {
    return p.split(path.sep).join("/");
}

/// Generate compile_commands.json using CMake with Ninja backend.
export function generateCdb(workspace: string): void {
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
export function generateTestDataCdbs(dataDir: string = DATA_DIR): void {
    const write = (directory: string, entries: CdbEntry[]) => {
        // Atomic write: concurrent vitest invocations (one per porting agent)
        // regenerate the same CDBs; a rename never exposes a truncated file
        // to a server reading it.
        const target = path.join(directory, "compile_commands.json");
        const tmp = `${target}.tmp-${process.pid}`;
        fs.writeFileSync(tmp, JSON.stringify(entries, null, 2));
        fs.renameSync(tmp, target);
    };

    const entry = (directory: string, source: string, extraArgs: string[] = []): CdbEntry => ({
        directory: posix(directory),
        file: posix(source),
        arguments: ["clang++", "-std=c++17", "-fsyntax-only", ...extraArgs, posix(source)],
    });

    const sourcesIn = (dir: string, recursive: boolean): string[] => {
        if (!fs.existsSync(dir)) {
            return [];
        }
        return fs
            .readdirSync(dir, { recursive, encoding: "utf8" })
            .filter((name) => name.endsWith(".cpp"))
            .sort()
            .map((name) => path.join(dir, name));
    };

    const single = (name: string, extraArgs: string[] = []) => {
        const dir = path.join(dataDir, name);
        const main = path.join(dir, "main.cpp");
        if (fs.existsSync(main)) {
            write(dir, [entry(dir, main, extraArgs)]);
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
        write(mcDir, [entry(mcDir, mcMain, ["-DCONFIG_A"]), entry(mcDir, mcMain, ["-DCONFIG_B"])]);
    }

    single("include_completion", ["-I."]);

    // document_links
    const dlDir = path.join(dataDir, "document_links");
    const dlSources = sourcesIn(dlDir, false);
    if (dlSources.length > 0) {
        write(
            dlDir,
            dlSources.map((src) => entry(dlDir, src, [`-I${posix(dlDir)}`, "-std=c++23"])),
        );
    }

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
            .map((src) => entry(ptDir, src));
        if (entries.length > 0) {
            write(ptDir, entries);
        }
    }

    // Snapshot fixture corpora, shared with the unit snapshot glob tests.
    // -std matches the unit side's compile_file default (c++20) so both
    // layers compile each fixture identically. Keep in sync with FEATURES
    // in tests/integration/features/snapshots.test.ts (document_links has
    // its own block above).
    for (const corpus of ["document_symbol", "folding_range", "inlay_hint", "semantic_tokens"]) {
        const corpusDir = path.join(dataDir, corpus);
        const sources = sourcesIn(corpusDir, true);
        if (sources.length > 0) {
            write(
                corpusDir,
                sources.map((src) => entry(corpusDir, src, ["-std=c++20"])),
            );
        }
    }
}

/// Write a compile_commands.json for the given source files.
export function writeCdb(
    workspace: string,
    files: string[],
    options: { extraArgs?: string[]; std?: string } = {},
): void {
    const entries = files.map((f) => ({
        directory: workspace,
        file: path.join(workspace, f),
        arguments: [
            "clang++",
            `-std=${options.std ?? "c++17"}`,
            "-fsyntax-only",
            ...(options.extraArgs ?? []),
            path.join(workspace, f),
        ],
    }));
    fs.writeFileSync(
        path.join(workspace, "compile_commands.json"),
        JSON.stringify(entries, null, 2),
    );
}

/// Write a compile_commands.json with per-file extra arguments; a file may
/// appear multiple times to model multi-configuration projects.
export function writeEntries(workspace: string, entries: [string, string[]][]): void {
    const data = entries.map(([f, args]) => ({
        directory: workspace,
        file: path.join(workspace, f),
        arguments: ["clang++", "-std=c++17", "-fsyntax-only", ...args, path.join(workspace, f)],
    }));
    fs.writeFileSync(path.join(workspace, "compile_commands.json"), JSON.stringify(data));
}
