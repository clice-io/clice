/// A project made ready to index: its pinned commit checked out and its
/// preparation run, the compilation database and every file its units
/// include written. A preparation that fails is the setup's failure, never
/// clice's.

import { spawn } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { REPO_ROOT } from "../repo.ts";
import { currentPlatform, type Platform, type Project, type Step } from "./projects.ts";

/// Run a command with its output streamed through, resolving once it
/// exits 0 and rejecting with its exit otherwise.
export function stream(command: string, args: readonly string[], cwd: string): Promise<void> {
    return new Promise((resolve, reject) => {
        const child = spawn(command, args, { cwd, stdio: ["ignore", "inherit", "inherit"] });
        child.on("error", reject);
        child.on("close", (status, signal) => {
            if (status === 0) {
                resolve();
            } else {
                const how = signal ?? `exit ${String(status)}`;
                reject(new Error(`${[command, ...args].join(" ")} in ${cwd}: ${how}`));
            }
        });
    });
}

function output(command: string, args: readonly string[], cwd: string): Promise<string> {
    return new Promise((resolve, reject) => {
        const child = spawn(command, args, { cwd, stdio: ["ignore", "pipe", "inherit"] });
        let text = "";
        child.stdout.setEncoding("utf8").on("data", (chunk: string) => {
            text += chunk;
        });
        child.on("error", reject);
        child.on("close", (status) => {
            if (status === 0) {
                resolve(text);
            } else {
                reject(
                    new Error(`${[command, ...args].join(" ")} in ${cwd}: exit ${String(status)}`),
                );
            }
        });
    });
}

/// Where the project is checked out: clice itself is the repository the
/// run belongs to, every other project a directory of its own in `dir`.
export function checkoutOf(project: Project, dir: string): string {
    return project.repository === undefined ? REPO_ROOT : path.join(dir, project.name);
}

/// Fetch the pinned commit alone into a fresh checkout; a checkout already
/// at it is kept, with what an earlier preparation built in it.
async function checkout(project: Project, target: string): Promise<boolean> {
    const { repository, commit } = project;
    if (repository === undefined || commit === undefined) {
        return false;
    }
    if (fs.existsSync(path.join(target, ".git"))) {
        const head = (await output("git", ["rev-parse", "HEAD"], target)).trim();
        if (head === commit) {
            return true;
        }
        throw new Error(`${target} holds ${head}, not the pinned ${commit}: remove it first`);
    }
    fs.mkdirSync(target, { recursive: true });
    await stream("git", ["init", "-q"], target);
    await stream("git", ["fetch", "-q", "--depth", "1", repository, commit], target);
    // Windows bounds a path at 260 characters unless git is told otherwise.
    await stream("git", ["-c", "core.longpaths=true", "checkout", "-q", "FETCH_HEAD"], target);
    return false;
}

/// The build directory's generated sources and headers built, and only
/// what they need: the order-only dependencies CMake adds to a custom
/// command (the whole target its output belongs to, the libraries of its
/// tools) are dropped from build.ninja first. LLVM's take 11 minutes on 4
/// cores this way, more than an hour with them.
async function buildGenerated(build: string): Promise<void> {
    const ninja = path.join(build, "build.ninja");
    const lines = fs.readFileSync(ninja, "utf8").split("\n");
    const out: string[] = [];
    for (let i = 0; i < lines.length; i += 1) {
        let line = lines[i] ?? "";
        if (line.startsWith("build ") && line.includes(" CUSTOM_COMMAND")) {
            while (line.endsWith("$") && i + 1 < lines.length) {
                i += 1;
                line = line.slice(0, -1) + (lines[i] ?? "").trimStart();
            }
            line = line.split(" || ")[0] ?? line;
        }
        out.push(line);
    }
    fs.writeFileSync(ninja, out.join("\n"));
    const targets = (await output("ninja", ["-t", "targets", "rule", "CUSTOM_COMMAND"], build))
        .split(/\r?\n/)
        .filter((target) => !path.isAbsolute(target) && /\.(cpp|h|inc)$/.test(target));
    // A few hundred targets a command: Windows bounds a command line at
    // 32767 characters, and LLVM has over a thousand.
    for (let i = 0; i < targets.length; i += 300) {
        await stream("ninja", ["-k", "0", ...targets.slice(i, i + 300)], build);
    }
}

/// What `${CMAKE_TOOLCHAIN}` stands for on each platform: the target most
/// of its code builds for, where xclang's default is another (MinGW on
/// Windows, where most code builds against MSVC).
const CMAKE_TOOLCHAIN: Record<Platform, string[]> = {
    linux: [],
    macos: [],
    windows: [
        "-DCMAKE_C_COMPILER_TARGET=x86_64-pc-windows-msvc",
        "-DCMAKE_CXX_COMPILER_TARGET=x86_64-pc-windows-msvc",
    ],
};

function expand(arg: string, target: string): string[] {
    if (arg === "${CMAKE_TOOLCHAIN}") {
        return CMAKE_TOOLCHAIN[currentPlatform()];
    }
    return [
        arg
            .replaceAll("${JOBS}", String(os.availableParallelism()))
            .replaceAll("${CHECKOUT}", target.split(path.sep).join("/")),
    ];
}

async function run(step: Step, target: string): Promise<void> {
    if (!Array.isArray(step)) {
        await buildGenerated(path.join(target, step.generated));
        return;
    }
    const [command, ...args] = step.flatMap((arg) => expand(arg, target));
    if (command === undefined) {
        throw new Error("an empty preparation step");
    }
    await stream(command, args, target);
}

/// The marker a finished preparation leaves, so that running a job again
/// on the same directory skips straight to the index.
const PREPARED = ".nightly/prepared";

/// Check the project out into `dir` and prepare it; returns the checkout.
export async function prepare(project: Project, dir: string): Promise<string> {
    const target = checkoutOf(project, dir);
    const kept = await checkout(project, target);
    if (kept && fs.existsSync(path.join(target, PREPARED))) {
        return target;
    }
    for (const step of project.prepare) {
        await run(step, target);
    }
    if (project.repository !== undefined) {
        fs.mkdirSync(path.join(target, path.dirname(PREPARED)), { recursive: true });
        fs.writeFileSync(path.join(target, PREPARED), "");
    }
    return target;
}
