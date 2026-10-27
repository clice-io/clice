/// Compatibility scenarios: one real build system driving one real
/// toolchain over the shared project in tests/compat/project. A scenario
/// builds a throwaway copy of the project, so the compilation database it
/// is checked against is what the tools write today, never a stored copy.

import { execFile, spawnSync } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";

/// What clice must make of one source file's compile command, beyond the
/// checks every file gets (a database entry, a resolved toolchain, the
/// real compiler's macro values, a clean parse).
export interface FileExpectation {
    /// Argument sequences the database entry itself must contain: the
    /// shape the scenario exists to exercise, so a tool release that stops
    /// writing it fails the scenario instead of quietly testing less.
    recorded?: string[][];

    /// Argument sequences the resolved command must contain, each adjacent
    /// and in order; `${root}` stands for the project copy.
    contains?: string[][];

    /// Arguments the resolved command must not contain.
    excludes?: string[];
}

export interface Scenario {
    /// What the scenario exercises, e.g. "cmake ninja gcc".
    name: string;

    /// The platforms whose system toolchain the scenario names.
    platforms: NodeJS.Platform[];

    /// Build in the Visual Studio developer environment, which cl and
    /// clang-cl need for their headers and libraries. clice runs without
    /// it, as it does under an editor.
    msvc?: boolean;

    /// The MSVC toolset that environment selects (vcvars' `-vcvars_ver`,
    /// e.g. "14.29"); the newest installed when unset.
    toolset?: string;

    /// Executables the scenario runs, by name or absolute path.
    requires: string[];
    /// Directories the toolchain's own programs need on PATH, where its
    /// users keep them for the build and the editor alike: MSYS2's
    /// compilers load their DLLs from there.
    path?: string[];

    /// The name of the project copy's directory, where its path is what
    /// the scenario exercises: spaces and letters past ASCII go through the
    /// build, its database and clice.
    directory?: string;

    /// The bin directory of the Cygwin or MSYS2 the build runs in, whose
    /// database spells POSIX paths: its cygpath maps them for the checks,
    /// and its env runs the compiler as the build did.
    posix?: string;

    /// Build steps run in order in the project copy, each an argv.
    build: [string, ...string[]][];

    /// The project's clice.toml: rules a user writes on top of the build.
    config?: string;

    /// The project's source files under check, project-relative.
    files: Record<string, FileExpectation>;
}

export interface ProcessRun {
    /// The exit status, null when the process could not run or was killed.
    status: number | null;
    stdout: string;
    stderr: string;
    /// Why the process did not run to an exit status.
    error?: string;
}

/// Run a process to completion without blocking the event loop: vitest's
/// worker must keep answering its runner while a build takes a minute.
export function run(
    tool: string,
    args: string[],
    options: { cwd?: string; env: NodeJS.ProcessEnv },
): Promise<ProcessRun> {
    return new Promise((resolve) => {
        execFile(
            tool,
            args,
            { ...options, encoding: "utf8", maxBuffer: 64 * 1024 * 1024, timeout: 300_000 },
            (error, stdout, stderr) => {
                if (error === null) {
                    resolve({ status: 0, stdout, stderr });
                } else if (typeof error.code === "number") {
                    resolve({ status: error.code, stdout, stderr });
                } else {
                    resolve({ status: null, stdout, stderr, error: error.message });
                }
            },
        );
    });
}

/// A copy of Windows' case-insensitive environment is an ordinary object,
/// where the variable keeps its own spelling (`Path`).
function pathKey(env: NodeJS.ProcessEnv): string {
    return Object.keys(env).find((name) => name.toUpperCase() === "PATH") ?? "PATH";
}

/// The environment clice runs in: the caller's, minus pixi environments.
/// Their compilers and build tools would otherwise shadow the system
/// toolchain a scenario names by bare name (meson and bear write `cc`,
/// `x86_64-w64-mingw32-g++` as found on PATH), and the flags their
/// activation exports (LDFLAGS with the environment's library paths) would
/// reach every build.
export function systemEnv(): NodeJS.ProcessEnv {
    const pixi = `.pixi${path.sep}envs`;
    const key = pathKey(process.env);
    const env: NodeJS.ProcessEnv = Object.fromEntries(
        Object.entries(process.env).filter(
            ([name, value]) => name === key || value?.includes(pixi) !== true,
        ),
    );
    env[key] = (env[key] ?? "")
        .split(path.delimiter)
        .filter((entry) => !entry.includes(pixi))
        .join(path.delimiter);
    return env;
}

const developer = new Map<string, NodeJS.ProcessEnv | null>();

/// What vcvars64.bat of the newest Visual Studio with the C++ tools sets
/// on top of systemEnv() for `toolset` ("" for the newest); null without
/// one.
function developerEnv(toolset: string): NodeJS.ProcessEnv | null {
    const known = developer.get(toolset);
    if (known !== undefined) {
        return known;
    }
    developer.set(toolset, null);
    const vswhere = path.join(
        process.env["ProgramFiles(x86)"] ?? "C:\\Program Files (x86)",
        "Microsoft Visual Studio",
        "Installer",
        "vswhere.exe",
    );
    if (process.platform !== "win32" || !fs.existsSync(vswhere)) {
        return null;
    }
    const install = spawnSync(
        vswhere,
        [
            "-latest",
            "-products",
            "*",
            "-requires",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-property",
            "installationPath",
        ],
        { encoding: "utf8" },
    ).stdout.trim();
    const vcvars = path.join(install, "VC", "Auxiliary", "Build", "vcvars64.bat");
    if (install === "" || !fs.existsSync(vcvars)) {
        return null;
    }
    // vcvars fails on a toolset that is not installed.
    const toolsets = path.join(install, "VC", "Tools", "MSVC");
    if (
        toolset !== "" &&
        !fs
            .readdirSync(toolsets)
            .some((installed) => installed === toolset || installed.startsWith(`${toolset}.`))
    ) {
        return null;
    }
    const version = toolset === "" ? "" : ` -vcvars_ver=${toolset}`;
    const run = spawnSync("cmd.exe", ["/d", "/s", "/c", `""${vcvars}"${version} 1>&2 && set"`], {
        env: systemEnv(),
        encoding: "utf8",
        windowsVerbatimArguments: true,
    });
    if (run.status !== 0) {
        throw new Error(`${vcvars} failed:\n${run.stdout}\n${run.stderr}`);
    }
    const env: NodeJS.ProcessEnv = {};
    for (const line of run.stdout.split(/\r?\n/)) {
        const at = line.indexOf("=");
        if (at > 0) {
            env[line.slice(0, at)] = line.slice(at + 1);
        }
    }
    developer.set(toolset, env);
    return env;
}

function withPath(env: NodeJS.ProcessEnv, dirs: string[] | undefined): NodeJS.ProcessEnv {
    if (dirs === undefined) {
        return env;
    }
    const key = pathKey(env);
    return { ...env, [key]: [...dirs, env[key] ?? ""].join(path.delimiter) };
}

/// The environment the scenario's build and its compiler run in; null
/// when it needs a Visual Studio, or a toolset of it, the machine does not
/// have.
export function buildEnv(scenario: Scenario): NodeJS.ProcessEnv | null {
    const env = scenario.msvc === true ? developerEnv(scenario.toolset ?? "") : systemEnv();
    return env === null ? null : withPath(env, scenario.path);
}

/// The environment clice runs in for the scenario: never the developer
/// prompt's, as an editor runs it, even when the suite runs in one. Its
/// header lists would stand in for the ones clang finds itself.
export function cliceEnv(scenario: Scenario): NodeJS.ProcessEnv {
    const prompt = new Set(["INCLUDE", "EXTERNAL_INCLUDE"]);
    const env = Object.fromEntries(
        Object.entries(systemEnv()).filter(([name]) => !prompt.has(name.toUpperCase())),
    );
    return withPath(env, scenario.path);
}

function locate(tool: string, env: NodeJS.ProcessEnv): string | undefined {
    const names = process.platform === "win32" ? [tool, `${tool}.exe`] : [tool];
    if (path.isAbsolute(tool)) {
        return names.find((name) => fs.existsSync(name));
    }
    for (const dir of (env[pathKey(env)] ?? "").split(path.delimiter)) {
        const found = names.map((name) => path.join(dir, name)).find((p) => fs.existsSync(p));
        if (found !== undefined) {
            return found;
        }
    }
    return undefined;
}

export function missingTools(scenario: Scenario): string[] {
    const env = buildEnv(scenario);
    if (env === null) {
        return [
            scenario.toolset === undefined
                ? "Visual Studio with the C++ tools"
                : `Visual Studio with the MSVC ${scenario.toolset} toolset`,
        ];
    }
    return scenario.requires.filter((tool) => locate(tool, env) === undefined);
}
