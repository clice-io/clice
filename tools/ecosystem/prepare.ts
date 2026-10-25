/// A configuration made ready to index: its project's pinned commit
/// checked out and its preparation run, the compilation database and every
/// file its units include written. A preparation that fails is the setup's
/// failure, never clice's.

import { execFileSync, spawn } from "node:child_process";
import { createHash } from "node:crypto";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { runProcess } from "../client/process.ts";
import { REPO_ROOT } from "../repo.ts";
import {
    configurationOf,
    currentPlatform,
    type Configuration,
    type Platform,
    type Project,
    type Step,
} from "./project.ts";

function stream(
    command: string,
    args: readonly string[],
    cwd: string,
    env?: NodeJS.ProcessEnv,
): Promise<void> {
    return new Promise((resolve, reject) => {
        const child = spawn(command, args, { cwd, env, stdio: ["ignore", "inherit", "inherit"] });
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

/// Where a configuration is checked out: clice itself is the repository the
/// run belongs to, every other project's configuration a directory of its
/// own in `dir`.
export function checkoutOf(project: Project, configuration: string, dir: string): string {
    return project.repository === undefined && project.archive === undefined
        ? REPO_ROOT
        : path.join(dir, project.name, configuration);
}

/// Fetch the pinned commit alone into a fresh checkout; a checkout already
/// at it is kept, with what an earlier preparation built in it.
async function checkout(project: Project, target: string): Promise<boolean> {
    const { repository, commit } = project;
    if (repository === undefined || commit === undefined) {
        return false;
    }
    if (fs.existsSync(path.join(target, ".git"))) {
        const head = execFileSync("git", ["rev-parse", "HEAD"], {
            cwd: target,
            encoding: "utf8",
        }).trim();
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

/// The marker an unpacked archive leaves: the hash it was checked against.
const UNPACKED = ".ecosystem/archive";

/// An archive downloaded, checked against its pinned SHA-256 and unpacked
/// into `target`, its one top-level directory (if it has one) taken as
/// `target` itself; one unpacked there already is kept, and says so.
export async function unpackArchive(url: string, sha256: string, target: string): Promise<boolean> {
    const marker = path.join(target, UNPACKED);
    if (fs.existsSync(marker)) {
        if (fs.readFileSync(marker, "utf8") === sha256) {
            return true;
        }
        throw new Error(`${target} holds another archive than ${url}: remove it first`);
    }
    const response = await fetch(url);
    if (!response.ok) {
        throw new Error(`${url}: ${String(response.status)} ${response.statusText}`);
    }
    const bytes = Buffer.from(await response.arrayBuffer());
    const got = createHash("sha256").update(bytes).digest("hex");
    if (got !== sha256) {
        throw new Error(`${url} has the SHA-256 ${got}, not the pinned ${sha256}`);
    }
    fs.mkdirSync(target, { recursive: true });
    const file = path.join(os.tmpdir(), `${got}${path.extname(url)}`);
    fs.writeFileSync(file, bytes);
    // GNU tar reads no zip, and takes `C:` for a remote host: the bsdtar
    // of macOS and of Windows' System32 (which Git Bash's own tar hides on
    // the path) reads both.
    await (process.platform === "linux" && file.endsWith(".zip")
        ? stream("unzip", ["-q", file, "-d", target], target)
        : stream(
              process.platform === "win32"
                  ? path.join(process.env["SystemRoot"] ?? "C:\\Windows", "System32", "tar.exe")
                  : "tar",
              ["-xf", file, "-C", target],
              target,
          ));
    fs.rmSync(file);
    const unpacked = fs.readdirSync(target, { withFileTypes: true });
    const [top] = unpacked;
    if (unpacked.length === 1 && top?.isDirectory() === true) {
        const inner = path.join(target, top.name);
        for (const name of fs.readdirSync(inner)) {
            fs.renameSync(path.join(inner, name), path.join(target, name));
        }
        fs.rmdirSync(inner);
    }
    fs.mkdirSync(path.dirname(marker), { recursive: true });
    fs.writeFileSync(marker, sha256);
    return false;
}

/// build.ninja without the order-only dependencies of its custom commands,
/// and the TableGen files TableGen itself writes there (MLIR's
/// AccCommon.td), which other TableGen runs include.
export function withoutOrderOnly(ninja: string): { ninja: string; tablegenFiles: string[] } {
    const lines = ninja.split("\n");
    const out: string[] = [];
    const tablegenFiles: string[] = [];
    for (let i = 0; i < lines.length; i += 1) {
        let line = lines[i] ?? "";
        if (line.startsWith("build ") && line.includes(" CUSTOM_COMMAND")) {
            while (line.endsWith("$") && i + 1 < lines.length) {
                i += 1;
                line = line.slice(0, -1) + (lines[i] ?? "").trimStart();
            }
            line = line.split(" || ")[0] ?? line;
            const [outputs = "", inputs = ""] = line
                .slice("build ".length)
                .split(": CUSTOM_COMMAND");
            if (inputs.includes("tblgen")) {
                tablegenFiles.push(
                    ...outputs
                        .split(" ")
                        .filter((each) => each.endsWith(".td") && !each.startsWith("$")),
                );
            }
        }
        out.push(line);
    }
    return { ninja: out.join("\n"), tablegenFiles };
}

/// The build directory's generated sources and headers built, and only
/// what they need: the order-only dependencies CMake adds to a custom
/// command (the whole target its output belongs to, the libraries of its
/// tools) are dropped from build.ninja first. LLVM's take 11 minutes on 4
/// cores this way, more than an hour with them.
async function buildGenerated(build: string, env: NodeJS.ProcessEnv | undefined): Promise<void> {
    const file = path.join(build, "build.ninja");
    const { ninja, tablegenFiles } = withoutOrderOnly(fs.readFileSync(file, "utf8"));
    fs.writeFileSync(file, ninja);
    const listed = await runProcess("ninja", ["-t", "targets", "rule", "CUSTOM_COMMAND"], {
        cwd: build,
        ...(env === undefined ? {} : { env }),
    });
    if (listed.status !== 0) {
        throw new Error(`ninja -t targets in ${build}: ${listed.stderr}`);
    }
    const generated = listed.stdout.split(/\r?\n/).filter((target) => !path.isAbsolute(target));
    // The TableGen files TableGen writes come first: the runs including them
    // came after them only through the dropped order-only dependencies. One
    // a tool of the tree writes (MLIR's yamlgen.td) its readers name as an
    // input; building that tool first would compile code whose generated
    // headers race the first pass.
    const passes = [
        tablegenFiles,
        generated.filter((target) => /\.(c|cc|cpp|cxx|h|hpp|inc)$/.test(target)),
    ];
    for (const targets of passes) {
        // A few hundred targets a command: Windows bounds a command line at
        // 32767 characters, and LLVM has over a thousand.
        for (let i = 0; i < targets.length; i += 300) {
            await stream("ninja", ["-k", "0", ...targets.slice(i, i + 300)], build, env);
        }
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

function expanded(text: string, target: string): string {
    return text
        .replaceAll("${JOBS}", String(os.availableParallelism()))
        .replaceAll("${CHECKOUT}", target.split(path.sep).join("/"));
}

function expand(arg: string, target: string): string[] {
    return arg === "${CMAKE_TOOLCHAIN}"
        ? CMAKE_TOOLCHAIN[currentPlatform()]
        : [expanded(arg, target)];
}

/// Visual Studio's x64 environment, as its vcvars64.bat sets it up.
async function vcvars(): Promise<NodeJS.ProcessEnv> {
    const vswhere = path.join(
        process.env["ProgramFiles(x86)"] ?? "C:\\Program Files (x86)",
        "Microsoft Visual Studio",
        "Installer",
        "vswhere.exe",
    );
    const found = await runProcess(vswhere, ["-latest", "-property", "installationPath"]);
    const installation = found.stdout.trim();
    if (found.status !== 0 || installation === "") {
        throw new Error(`no Visual Studio found by ${vswhere}: ${found.stderr}`);
    }
    const script = path.join(installation, "VC", "Auxiliary", "Build", "vcvars64.bat");
    // A batch file of its own: cmd parses quotes in its command line its own
    // way, which an argv cannot spell.
    const batch = path.join(fs.mkdtempSync(path.join(os.tmpdir(), "vcvars-")), "env.bat");
    fs.writeFileSync(batch, `@call "${script}" >nul\r\n@set\r\n`);
    const set = await runProcess("cmd.exe", ["/d", "/c", batch]);
    fs.rmSync(path.dirname(batch), { recursive: true });
    if (set.status !== 0) {
        throw new Error(`${script}: ${set.stdout}\n${set.stderr}`);
    }
    return Object.fromEntries(
        set.stdout.split(/\r?\n/).flatMap((line) => {
            const equals = line.indexOf("=");
            return equals > 0 ? [[line.slice(0, equals), line.slice(equals + 1)]] : [];
        }),
    );
}

/// The environment a configuration's compiler runs in: the runner's own,
/// unless it names another.
export async function compilerEnvironment(
    config: Configuration,
): Promise<NodeJS.ProcessEnv | undefined> {
    return config.environment === "vcvars" ? vcvars() : undefined;
}

async function run(
    step: Step,
    target: string,
    database: string,
    env: NodeJS.ProcessEnv | undefined,
): Promise<void> {
    if ("generated" in step) {
        await buildGenerated(path.join(target, step.generated), env);
        return;
    }
    if ("database" in step) {
        const entries = JSON.parse(expanded(JSON.stringify(step.database), target)) as unknown;
        fs.mkdirSync(path.dirname(path.join(target, database)), { recursive: true });
        fs.writeFileSync(path.join(target, database), JSON.stringify(entries, null, 1));
        return;
    }
    const [command, ...args] = step.run.flatMap((arg) => expand(arg, target));
    if (command === undefined) {
        throw new Error("an empty preparation step");
    }
    await stream(command, args, target, env);
}

/// The marker a finished preparation leaves, so that running a job again
/// on the same directory skips the preparation: the configuration it ran.
const PREPARED = ".ecosystem/prepared";

export async function prepare(
    project: Project,
    configuration: string,
    dir: string,
): Promise<string> {
    const config = configurationOf(project, configuration);
    const target = checkoutOf(project, configuration, dir);
    const kept =
        project.archive === undefined
            ? await checkout(project, target)
            : await unpackArchive(project.archive.url, project.archive.sha256, target);
    const marker = path.join(target, PREPARED);
    const ran = JSON.stringify({ prepare: config.prepare, environment: config.environment });
    if (kept && fs.existsSync(marker)) {
        if (fs.readFileSync(marker, "utf8") !== ran) {
            throw new Error(`${target} was prepared by another configuration: remove it first`);
        }
        return target;
    }
    const env = await compilerEnvironment(config);
    for (const step of config.prepare) {
        await run(step, target, config.database, env);
    }
    if (target !== REPO_ROOT) {
        fs.mkdirSync(path.dirname(marker), { recursive: true });
        fs.writeFileSync(marker, ran);
    }
    return target;
}
