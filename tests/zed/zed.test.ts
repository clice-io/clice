/// The Zed extension (editors/zed) in a real Zed. The built extension is
/// installed into a throwaway Zed data directory, then:
///
/// - opening a C++ file makes Zed start the clice the extension downloaded
///   from GitHub, and the download replaces older versions;
/// - that clice resolves its builtin headers from the `lib/clang` shipped
///   next to it;
/// - a C file and a CUDA file, each opened alone with the network cut off,
///   start the same installation.
///
/// ZED_EXECUTABLE names the Zed app and ZED_EXTENSION_WASM the extension
/// built for wasm32-wasip2. `clice` must not be on PATH, or the extension
/// uses it instead of downloading; on Linux, run under `xvfb-run`.

import { spawn, spawnSync, type ChildProcess } from "node:child_process";
import { once } from "node:events";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { CliceClient, sleep, waitUntil } from "@clice/tools/client";
import { afterAll, expect, test } from "vitest";

const STARTUP_TIMEOUT = 300_000;
/// How long a started server has to stay up for the start to count.
const SETTLE_TIME = 10_000;
/// Zed honors the proxy variables; nothing listens on this port.
const OFFLINE = { ALL_PROXY: "http://127.0.0.1:9", NO_PROXY: "", no_proxy: "" };

const STARTED = /starting language server process\. binary path: "((?:[^"\\]|\\.)*)"/;
const FAILED = /Failed to start language server "clice".*/;

function required(name: string): string {
    const value = process.env[name];
    if (!value) {
        throw new Error(`${name} is not set`);
    }
    return value;
}

const zedExecutable = required("ZED_EXECUTABLE");
const extensionWasm = required("ZED_EXTENSION_WASM");
const extensionDir = path.resolve(import.meta.dirname, "../../editors/zed");
const binaryName = process.platform === "win32" ? "clice.exe" : "clice";

const root = fs.realpathSync.native(fs.mkdtempSync(path.join(os.tmpdir(), "clice-zed-")));
const data = path.join(root, "zed");
const work = path.join(data, "extensions", "work", "clice");
const log = path.join(data, "logs", "Zed.log");

afterAll(() => {
    fs.rmSync(root, { recursive: true, force: true });
});

function write(file: string, text: string): void {
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, text);
}

function install(): void {
    const installed = path.join(data, "extensions", "installed", "clice");
    fs.mkdirSync(installed, { recursive: true });
    fs.copyFileSync(
        path.join(extensionDir, "extension.toml"),
        path.join(installed, "extension.toml"),
    );
    fs.copyFileSync(extensionWasm, path.join(installed, "extension.wasm"));
    // Only clice serves C and C++, so every server start in the log is clice's.
    // A first start would also install the html extension, whose reload
    // races clice's registration.
    const settings = {
        session: { trust_all_worktrees: true },
        auto_update: false,
        auto_install_extensions: { html: false },
        telemetry: { diagnostics: false, metrics: false },
        languages: {
            "C++": { language_servers: ["clice"] },
            C: { language_servers: ["clice"] },
        },
    };
    write(path.join(data, "config", "settings.json"), JSON.stringify(settings));
}

/// A project holding just `file`, so reopening Zed restores nothing else.
function project(file: string, text: string): string {
    const dir = path.join(root, path.extname(file).slice(1));
    write(path.join(dir, file), text);
    const command = {
        directory: dir,
        file: path.join(dir, file),
        arguments: ["clang++", "-c", file],
    };
    write(path.join(dir, "compile_commands.json"), JSON.stringify([command]));
    return dir;
}

function readLog(): string {
    return fs.existsSync(log) ? fs.readFileSync(log, "utf8") : "";
}

async function stop(zed: ChildProcess): Promise<void> {
    const exited = zed.exitCode === null ? once(zed, "exit") : Promise.resolve();
    if (process.platform === "win32") {
        spawnSync("taskkill", ["/F", "/T", "/PID", String(zed.pid)]);
    } else {
        process.kill(-zed.pid!, "SIGKILL");
    }
    await exited;
}

/// Opens `file` of `dir` alone in Zed and returns the clice binary Zed started.
async function startServer(dir: string, file: string, env: Record<string, string> = {}) {
    fs.rmSync(log, { force: true });
    const zed = spawn(zedExecutable, ["--user-data-dir", data, dir, path.join(dir, file)], {
        env: { ...process.env, ZED_ALLOW_EMULATED_GPU: "1", ...env },
        stdio: "ignore",
        detached: process.platform !== "win32",
    });
    const checkFailed = () => {
        const failure = FAILED.exec(readLog());
        if (failure) {
            throw new Error(`${file}: ${failure[0]}`);
        }
    };
    try {
        const started = await waitUntil(
            () => {
                if (zed.exitCode !== null) {
                    throw new Error(`${file}: Zed exited with code ${zed.exitCode}`);
                }
                checkFailed();
                return STARTED.exec(readLog())?.[1];
            },
            {
                timeout: STARTUP_TIMEOUT,
                interval: 2_000,
                description: `Zed to start clice for ${file}`,
            },
        ).catch((error: unknown) => {
            const tail = readLog().split("\n").slice(-40).join("\n");
            throw new Error(`${String(error)}\n--- end of ${log}:\n${tail}`);
        });
        await sleep(SETTLE_TIME);
        checkFailed();
        return fs.realpathSync.native(JSON.parse(`"${started}"`) as string);
    } finally {
        await stop(zed);
    }
}

let binary = "";
let marker = "";

test("a C++ file downloads and starts clice", async () => {
    const onPath = (process.env["PATH"] ?? "")
        .split(path.delimiter)
        .find((dir) => dir && fs.existsSync(path.join(dir, binaryName)));
    expect(onPath, "clice on PATH would stop the extension from downloading").toBeUndefined();

    install();
    const outdated = path.join(work, "clice-0.0.0");
    write(path.join(outdated, "clice", "bin", binaryName), "");

    binary = await startServer(
        project("main.cpp", "#include <stddef.h>\nsize_t size;\n"),
        "main.cpp",
    );
    const versionDir = path.dirname(path.dirname(path.dirname(binary)));
    expect(path.dirname(versionDir)).toBe(fs.realpathSync.native(work));
    expect(path.basename(versionDir)).toMatch(/^clice-\d/);
    expect(fs.existsSync(outdated), "an outdated version is removed").toBe(false);
    expect(fs.existsSync(path.join(work, "download")), "no staging leftovers").toBe(false);

    // A reinstall replaces the whole version directory, marker included.
    marker = path.join(versionDir, "e2e-marker");
    fs.writeFileSync(marker, "");
});

test("the download resolves builtin headers from its lib/clang", async () => {
    const dir = path.join(root, "cpp");
    const client = CliceClient.start(binary, { cwd: dir });
    try {
        await client.initialize(dir);
        const [uri] = await client.openAndWait("main.cpp");
        const hover = await client.hoverAt(uri, 1, 2);
        expect(JSON.stringify(hover?.contents)).toContain("size_t");
    } finally {
        await client.shutdown();
    }
});

test.each([
    ["util.c", "int twice(int x) { return 2 * x; }\n"],
    ["kernel.cu", "__global__ void kernel() {}\n"],
])("%s starts the installed clice offline", async (file, text) => {
    expect(await startServer(project(file, text), file, OFFLINE)).toBe(binary);
    expect(fs.existsSync(marker), "the installation was not replaced").toBe(true);
});
