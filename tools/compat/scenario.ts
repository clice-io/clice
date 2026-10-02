/// Compatibility scenarios: one real build system driving one real
/// toolchain over the shared project in tests/compat/project. A scenario
/// builds a throwaway copy of the project, so the compilation database it
/// is checked against is what the tools write today, never a stored copy.

import { spawnSync } from "node:child_process";
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

    /// What clice does not handle yet here. Its checks are skipped; the
    /// build still runs, so the scenario stays ready for the day they are
    /// turned on.
    unsupported?: string;

    /// Build in the Visual Studio developer environment, which cl and
    /// clang-cl need for their headers and libraries. clice runs without
    /// it, as it does under an editor.
    msvc?: boolean;

    /// Executables the scenario runs, by name or absolute path.
    requires: string[];

    /// Build steps run in order in the project copy, each an argv.
    build: [string, ...string[]][];

    /// The project's source files under check, project-relative.
    files: Record<string, FileExpectation>;
}

/// A copy of Windows' case-insensitive environment is an ordinary object,
/// where the variable keeps its own spelling (`Path`).
function pathKey(env: NodeJS.ProcessEnv): string {
    return Object.keys(env).find((name) => name.toUpperCase() === "PATH") ?? "PATH";
}

/// The environment clice runs in: the caller's, minus pixi environments,
/// whose compilers and build tools would otherwise shadow the system
/// toolchain a scenario names by bare name (meson and bear write `cc`,
/// `x86_64-w64-mingw32-g++` as found on PATH).
export function systemEnv(): NodeJS.ProcessEnv {
    const env = { ...process.env };
    const key = pathKey(env);
    env[key] = (env[key] ?? "")
        .split(path.delimiter)
        .filter((entry) => !entry.includes(`.pixi${path.sep}envs`))
        .join(path.delimiter);
    return env;
}

let developer: NodeJS.ProcessEnv | null | undefined;

/// What vcvars64.bat of the newest Visual Studio with the C++ tools sets
/// on top of systemEnv(); null without one.
function developerEnv(): NodeJS.ProcessEnv | null {
    if (developer !== undefined) {
        return developer;
    }
    developer = null;
    const vswhere = path.join(
        process.env["ProgramFiles(x86)"] ?? "C:\\Program Files (x86)",
        "Microsoft Visual Studio",
        "Installer",
        "vswhere.exe",
    );
    if (process.platform !== "win32" || !fs.existsSync(vswhere)) {
        return developer;
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
        return developer;
    }
    const run = spawnSync("cmd.exe", ["/d", "/s", "/c", `""${vcvars}" >nul && set"`], {
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
    developer = env;
    return developer;
}

/// The environment the scenario's build and its compiler run in; null
/// when it needs a Visual Studio the machine does not have.
export function buildEnv(scenario: Scenario): NodeJS.ProcessEnv | null {
    return scenario.msvc === true ? developerEnv() : systemEnv();
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
        return ["Visual Studio with the C++ tools"];
    }
    return scenario.requires.filter((tool) => locate(tool, env) === undefined);
}
